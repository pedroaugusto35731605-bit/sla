// =============================================================================
//  SeriesS-Emu — bios_uefi.cpp
// =============================================================================
#include "bios_uefi.hpp"

#include "log.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace xss {

// ============================================================================
//  Kernel do Xbox OS (HLE)
// ============================================================================
XboxOsKernel::XboxOsKernel(MemoryManager& mem, GpuDevice& gpu)
    : mem_(mem), gpu_(gpu), heap_next_(spec::vmap::kHeapBaseVA) {}

void XboxOsKernel::dispatch(CpuCore& core) {
    auto& r = core.state().gpr;
    const auto service = static_cast<std::uint32_t>(r[RAX]);

    switch (service) {
        case svc::XDbgPrint: {
            const std::uint64_t len = std::min<std::uint64_t>(r[RSI], 4096);
            std::string s(len, '\0');
            mem_.read_virtual(r[RDI], s.data(), len);
            log_info("KRNL", "XDbgPrint(cpu{}): {}", core.id(), s);
            r[RAX] = status::Success;
            break;
        }
        case svc::XDbgPrintHex:
            log_info("KRNL", "XDbgPrintHex(cpu{}): 0x{:X} ({})", core.id(), r[RDI], r[RDI]);
            r[RAX] = status::Success;
            break;

        case svc::NtAllocateVirtualMemory: {
            const std::uint64_t size = r[RDI];
            std::lock_guard lk(mu_);
            try {
                const AllocationId id = mem_.allocate(Partition::User, size, std::format("título: heap @0x{:X}", heap_next_));
                const auto info = *mem_.info(id);
                const GuestVA va = heap_next_;
                mem_.map_allocation(va, id, pte::kWritable | pte::kUser | pte::kNoExec);
                heap_[va] = id;
                // próximo VA alinhado a 2 MB (permite páginas grandes) + página de guarda
                heap_next_ = (va + info.size + spec::kMemory.large_page * 2 - 1) & ~(spec::kMemory.large_page - 1);
                log_info("KRNL", "NtAllocateVirtualMemory({}) → 0x{:X} | User Space livre: {}", human_bytes(info.size),
                         va, human_bytes(mem_.available(Partition::User)));
                r[RAX] = va;
            } catch (const OutOfMemory&) {
                ++failed_allocs_;
                log_warn("KRNL", "NtAllocateVirtualMemory({}) negado: STATUS_NO_MEMORY", human_bytes(size));
                r[RAX] = 0;
                r[RDX] = status::NoMemory;
            }
            break;
        }
        case svc::NtFreeVirtualMemory: {
            std::lock_guard lk(mu_);
            auto it = heap_.find(r[RDI]);
            if (it == heap_.end()) { r[RAX] = status::NotImplemented; break; }
            mem_.unmap_allocation(it->first, it->second);
            mem_.free(it->second);
            heap_.erase(it);
            r[RAX] = status::Success;
            break;
        }
        case svc::XMemQueryBudget:
            r[RAX] = mem_.available(Partition::User);
            break;
        case svc::KeQueryPerformanceCounter: {
            const auto ns = std::chrono::steady_clock::now().time_since_epoch();
            r[RAX] = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(ns).count() / 100);
            break;
        }
        case svc::XGpuSubmitCommandBuffer:
            log_info("KRNL", "XGpuSubmitCommandBuffer(va=0x{:X}, dwords={})", r[RDI], r[RSI]);
            gpu_.submit_command_buffer(r[RDI], static_cast<std::uint32_t>(r[RSI]));
            r[RAX] = status::Success;
            break;
        case svc::NtTerminateProcess:
            exit_code_ = static_cast<std::int64_t>(r[RDI]);
            terminated_ = true;
            log_info("KRNL", "NtTerminateProcess({})", static_cast<std::int64_t>(r[RDI]));
            core.halt();
            break;
        default:
            log_warn("KRNL", "serviço 0x{:X} não implementado (stub)", service);
            r[RAX] = status::NotImplemented;
    }
}

// ============================================================================
//  Mini-assembler x86-64 para a ROM de diagnóstico
// ============================================================================
namespace {
class Asm {
public:
    std::vector<std::uint8_t> out;
    std::uint64_t here() const { return out.size(); }
    void label(const std::string& n) { labels_[n] = here(); }
    void b(std::initializer_list<std::uint8_t> v) { out.insert(out.end(), v); }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }

    static std::uint8_t rex(bool w, unsigned reg, unsigned rm) {
        return static_cast<std::uint8_t>(0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((rm & 8) ? 1 : 0));
    }
    static std::uint8_t modrm(unsigned mod, unsigned reg, unsigned rm) {
        return static_cast<std::uint8_t>((mod << 6) | ((reg & 7) << 3) | (rm & 7));
    }

    void mov_imm64(unsigned r, std::uint64_t v) { b({rex(true, 0, r), static_cast<std::uint8_t>(0xB8 + (r & 7))}); u64(v); }
    void mov_imm32(unsigned r, std::uint32_t v) {
        if (r & 8) b({0x41});
        b({static_cast<std::uint8_t>(0xB8 + (r & 7))}); u32(v);
    }
    void mov_label(unsigned r, const std::string& lbl, std::uint64_t base) {
        b({rex(true, 0, r), static_cast<std::uint8_t>(0xB8 + (r & 7))});
        abs_.push_back({here(), lbl, base});
        u64(0);
    }
    void alu(std::uint8_t opc, unsigned dst, unsigned src, bool w = true) { b({rex(w, src, dst), opc, modrm(3, src, dst)}); }
    void add(unsigned d, unsigned s)   { alu(0x01, d, s); }
    void mov(unsigned d, unsigned s)   { alu(0x89, d, s); }
    void test(unsigned d, unsigned s)  { alu(0x85, d, s); }
    void xor32(unsigned d, unsigned s) { alu(0x31, d, s, false); }
    void store(unsigned base, unsigned src) { b({rex(true, src, base), 0x89, modrm(0, src, base)}); }
    void load(unsigned dst, unsigned base)  { b({rex(true, dst, base), 0x8B, modrm(0, dst, base)}); }
    void inc(unsigned r) { b({rex(true, 0, r), 0xFF, modrm(3, 0, r)}); }
    void dec(unsigned r) { b({rex(true, 0, r), 0xFF, modrm(3, 1, r)}); }
    void push(unsigned r) { if (r & 8) b({0x41}); b({static_cast<std::uint8_t>(0x50 + (r & 7))}); }
    void pop(unsigned r)  { if (r & 8) b({0x41}); b({static_cast<std::uint8_t>(0x58 + (r & 7))}); }
    void syscall() { b({0x0F, 0x05}); }
    void ret() { b({0xC3}); }
    void hlt() { b({0xF4}); }
    void jne(const std::string& l) { b({0x75}); rel8_.push_back({here(), l}); b({0}); }
    void je(const std::string& l)  { b({0x74}); rel8_.push_back({here(), l}); b({0}); }
    void jmp(const std::string& l) { b({0xEB}); rel8_.push_back({here(), l}); b({0}); }
    void call(const std::string& l) { b({0xE8}); rel32_.push_back({here(), l}); u32(0); }
    void data(const void* p, std::size_t n) { auto* c = static_cast<const std::uint8_t*>(p); out.insert(out.end(), c, c + n); }
    void align(std::size_t a) { while (out.size() % a) out.push_back(0x90); }

    // syscall de impressão de string
    void print(const std::string& lbl, std::size_t len, std::uint64_t base) {
        mov_label(RDI, lbl, base); mov_imm64(RSI, len); mov_imm32(RAX, svc::XDbgPrint); syscall();
    }

    void finish() {
        for (auto& f : rel8_) {
            const auto rel = static_cast<std::int64_t>(labels_.at(f.lbl)) - static_cast<std::int64_t>(f.at + 1);
            if (rel < -128 || rel > 127) throw std::logic_error("rel8 fora de alcance: " + f.lbl);
            out[f.at] = static_cast<std::uint8_t>(static_cast<std::int8_t>(rel));
        }
        for (auto& f : rel32_) {
            const auto rel = static_cast<std::int32_t>(static_cast<std::int64_t>(labels_.at(f.lbl)) - static_cast<std::int64_t>(f.at + 4));
            std::memcpy(&out[f.at], &rel, 4);
        }
        for (auto& f : abs_) {
            const std::uint64_t va = f.base + labels_.at(f.lbl);
            std::memcpy(&out[f.at], &va, 8);
        }
    }

private:
    struct Fix { std::uint64_t at; std::string lbl; };
    struct AbsFix { std::uint64_t at; std::string lbl; std::uint64_t base; };
    std::map<std::string, std::uint64_t> labels_;
    std::vector<Fix> rel8_, rel32_;
    std::vector<AbsFix> abs_;
};
}  // namespace

// ============================================================================
//  Firmware UEFI
// ============================================================================
UefiFirmware::UefiFirmware(MemoryManager& mem, CpuCluster& cpu, GpuDevice& gpu, SsdDevice& ssd)
    : mem_(mem), cpu_(cpu), gpu_(gpu), ssd_(ssd) {}

bool UefiFirmware::power_on_self_test() {
    log_info("UEFI", "SEC: vetor de reset — {} ({} núcleos / {} threads @ {:.1f} GHz com SMT)",
             spec::kCpu.name, spec::kCpu.cores, spec::kCpu.threads, static_cast<double>(spec::kCpu.clock_hz) / 1e9);

    // POST por amostragem: um bloco temporário em cada partição, padrões gravados e relidos.
    constexpr std::uint64_t patterns[] = {0x5555'5555'5555'5555ull, 0xAAAA'AAAA'AAAA'AAAAull, 0x0123'4567'89AB'CDEFull};
    for (Partition p : {Partition::System, Partition::User}) {
        const AllocationId id = mem_.allocate(p, spec::kMemory.large_page, "POST");
        for (std::uint64_t off = 0; off < spec::kMemory.large_page; off += 64 * 1024)
            for (auto pat : patterns) {
                std::uint64_t back = 0;
                mem_.write_allocation(id, off, &pat, 8);
                mem_.read_allocation(id, off, &back, 8);
                if (back != pat) { log_error("UEFI", "POST falhou em {} +0x{:X}", to_string(p), off); return false; }
            }
        mem_.free(id);
    }
    log_info("UEFI", "PEI: POST OK — {} {} (pool rápido {:.0f} GB/s, pool lento {:.0f} GB/s)",
             human_bytes(spec::kMemory.total), spec::kMemory.type, spec::kMemory.fast_bw_gbs, spec::kMemory.slow_bw_gbs);
    log_info("UEFI", "PEI: mapa de memória  [0x000000000 – 0x07FFFFFFF] System (pool lento 2 GB)");
    log_info("UEFI", "PEI: mapa de memória  [0x080000000 – 0x0BFFFFFFF] System (pool rápido 1 GB)");
    log_info("UEFI", "PEI: mapa de memória  [0x0C0000000 – 0x27FFFFFFF] User Space do título (7 GB, limite rígido)");
    return true;
}

void UefiFirmware::initialize_devices(bool vk_validation) {
    log_info("UEFI", "DXE: carregando drivers");
    gpu_.initialize(vk_validation);
    log_info("UEFI", "DXE: {} — {} GB, {:.1f} GB/s bruto, até {:.1f} GB/s descomprimido, latência {} µs, {} filas × QD {}",
             spec::kSsd.name, spec::kSsd.capacity / spec::GB, static_cast<double>(spec::kSsd.raw_bytes_s) / 1e9,
             static_cast<double>(spec::kSsd.decompressed_max_bytes_s) / 1e9, spec::kSsd.command_latency_ns / 1000,
             spec::kSsd.queues, spec::kSsd.queue_depth);
    (void)ssd_;
    if (system_) {
        std::string types;
        for (const auto& t : system_->packages) types += (types.empty() ? "" : ", ") + t;
        log_info("UEFI", "DXE: firmware do sistema {} instalado — {} pacotes ({}), {}", system_->version,
                 system_->packages.size(), types, human_bytes(system_->bytes));
        log_info("UEFI", "DXE: pacotes criptografados com chaves do console — boot segue com o kernel do Xbox OS em HLE");
    } else {
        log_info("UEFI", "DXE: nenhum firmware do sistema instalado (OSU1) — usando o kernel HLE embutido");
    }
}

void UefiFirmware::boot(const BootImage& image) {
    using namespace spec;
    if (image.bytes.empty()) throw std::runtime_error("imagem de boot vazia");
    log_info("UEFI", "BDS: dispositivo de boot → \"{}\" ({} bytes)", image.name, image.bytes.size());

    // Imagem do título: User Space, mapeada R-X em kImageVA
    image_alloc_ = mem_.allocate(Partition::User, image.bytes.size(), "título: imagem executável", alloc_flags::kPinned);
    mem_.write_allocation(image_alloc_, 0, image.bytes.data(), image.bytes.size());
    mem_.map_allocation(vmap::kImageVA, image_alloc_, pte::kUser);

    // Pilha do thread principal: RW-, NX
    stack_alloc_ = mem_.allocate(Partition::User, vmap::kStackSize, "título: pilha", alloc_flags::kPinned);
    mem_.map_allocation(vmap::kStackTopVA - vmap::kStackSize, stack_alloc_, pte::kWritable | pte::kUser | pte::kNoExec);

    cpu_.core(0).reset(vmap::kImageVA + image.entry_offset, vmap::kStackTopVA - 16);
    log_info("UEFI", "ExitBootServices: cpu0 RIP=0x{:X} RSP=0x{:X}; cpus 1-{} estacionadas | User Space livre: {}",
             vmap::kImageVA + image.entry_offset, vmap::kStackTopVA - 16, kCpu.threads - 1,
             human_bytes(mem_.available(Partition::User)));
}

BootImage UefiFirmware::diagnostic_rom() {
    const std::uint64_t base = spec::vmap::kImageVA;
    constexpr std::string_view kBanner = "SeriesS-Emu diagnostic ROM v0.1 iniciada";
    constexpr std::string_view kSub    = "CALL/RET + PUSH/POP funcionando";
    constexpr std::string_view kBudget = "Teste de orcamento: alocando blocos de 1 GiB ate o limite de 7 GB...";
    constexpr std::string_view kCount  = "Blocos de 1 GiB concedidos antes do limite:";
    constexpr std::string_view kBye    = "Diagnostico concluido. Encerrando.";

    const std::uint32_t pm4buf[] = {
        pm4::type3(pm4::SET_CONTEXT_REG, 2), kXssClearColorReg - pm4::kContextRegBase, 0x0E7A0DFFu,  // verde Xbox
        pm4::type3(pm4::SET_UCONFIG_REG, 2), pm4::VGT_PRIMITIVE_TYPE - pm4::kUconfigRegBase,
                                             static_cast<std::uint32_t>(PrimitiveType::TriList),
        pm4::type3(pm4::NUM_INSTANCES, 1),   1,
        pm4::type3(pm4::DRAW_INDEX_AUTO, 2), 3, 0x2,
        pm4::type3(pm4::NUM_INSTANCES, 1),   4,
        pm4::type3(pm4::DRAW_INDEX_AUTO, 2), 6, 0x2,
        pm4::type3(pm4::DISPATCH_DIRECT, 4), 80, 45, 1, 0x1,       // compute 80x45 grupos
        pm4::type3(pm4::EVENT_WRITE_EOP, 1), 0,
    };

    Asm a;
    a.print("banner", kBanner.size(), base);

    // soma 1..10 (ADD, DEC, flags, JNE)
    a.xor32(RAX, RAX);
    a.mov_imm32(RCX, 10);
    a.label("loop");
    a.add(RAX, RCX);
    a.dec(RCX);
    a.jne("loop");
    a.mov(RDI, RAX);
    a.mov_imm32(RAX, svc::XDbgPrintHex);      // 55
    a.syscall();
    a.call("subroutine");

    // heap pequeno: escreve e lê de volta
    a.mov_imm64(RDI, 0x10000);
    a.mov_imm32(RAX, svc::NtAllocateVirtualMemory);
    a.syscall();
    a.mov(RBX, RAX);
    a.mov_imm64(RCX, 0xDEADBEEFull);
    a.store(RBX, RCX);
    a.load(RDX, RBX);
    a.mov(RDI, RDX);
    a.mov_imm32(RAX, svc::XDbgPrintHex);      // 0xDEADBEEF
    a.syscall();

    // GPU
    a.mov_label(RDI, "pm4", base);
    a.mov_imm64(RSI, std::size(pm4buf));
    a.mov_imm32(RAX, svc::XGpuSubmitCommandBuffer);
    a.syscall();

    // Orçamento: consulta e aloca 1 GiB até o kernel negar
    a.mov_imm32(RAX, svc::XMemQueryBudget);
    a.syscall();
    a.mov(RDI, RAX);
    a.mov_imm32(RAX, svc::XDbgPrintHex);
    a.syscall();
    a.print("budget", kBudget.size(), base);
    a.xor32(R12, R12);
    a.label("alloc_loop");
    a.mov_imm64(RDI, 1ull << 30);
    a.mov_imm32(RAX, svc::NtAllocateVirtualMemory);
    a.syscall();
    a.test(RAX, RAX);
    a.je("alloc_done");
    a.inc(R12);
    a.jmp("alloc_loop");
    a.label("alloc_done");
    a.print("count", kCount.size(), base);
    a.mov(RDI, R12);
    a.mov_imm32(RAX, svc::XDbgPrintHex);
    a.syscall();

    a.print("bye", kBye.size(), base);
    a.xor32(RDI, RDI);
    a.mov_imm32(RAX, svc::NtTerminateProcess);
    a.syscall();
    a.hlt();

    a.label("subroutine");
    a.push(RBX);
    a.print("sub", kSub.size(), base);
    a.pop(RBX);
    a.ret();

    a.label("banner"); a.data(kBanner.data(), kBanner.size());
    a.label("sub");    a.data(kSub.data(), kSub.size());
    a.label("budget"); a.data(kBudget.data(), kBudget.size());
    a.label("count");  a.data(kCount.data(), kCount.size());
    a.label("bye");    a.data(kBye.data(), kBye.size());
    a.align(4);
    a.label("pm4");
    a.data(pm4buf, sizeof(pm4buf));
    a.finish();
    return {"SeriesS-Emu Diagnostic ROM", std::move(a.out), 0};
}

BootImage UefiFirmware::load_flat_binary(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("não foi possível abrir " + p.string());
    BootImage img;
    img.name = p.filename().string();
    img.bytes.assign(std::istreambuf_iterator<char>(f), {});
    return img;
}

}  // namespace xss
