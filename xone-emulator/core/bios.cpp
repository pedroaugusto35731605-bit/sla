// =============================================================================
//  XOne-Emu — bios.cpp
// =============================================================================
#include "bios.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string_view>

namespace xone {

namespace {

// ----------------------------------------------------------------------------
//  Mini-assembler x86-64 usado para gerar a ROM de diagnóstico.
//  Emite exatamente as codificações que o Decoder entende.
// ----------------------------------------------------------------------------
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
    // Endereço absoluto de um label dentro da imagem (resolvido em finish()).
    void mov_label(unsigned r, const std::string& lbl, std::uint64_t image_va) {
        b({rex(true, 0, r), static_cast<std::uint8_t>(0xB8 + (r & 7))});
        abs_fixups_.push_back({here(), lbl, image_va});
        u64(0);
    }
    void alu_rr(std::uint8_t opc, unsigned dst, unsigned src, bool w = true) {  // opc dst(r/m), src(reg)
        b({rex(w, src, dst), opc, modrm(3, src, dst)});
    }
    void add(unsigned d, unsigned s) { alu_rr(0x01, d, s); }
    void mov(unsigned d, unsigned s) { alu_rr(0x89, d, s); }
    void xor32(unsigned d, unsigned s) { alu_rr(0x31, d, s, false); }
    void store(unsigned base, unsigned src) { b({rex(true, src, base), 0x89, modrm(0, src, base)}); }
    void load(unsigned dst, unsigned base)  { b({rex(true, dst, base), 0x8B, modrm(0, dst, base)}); }
    void dec(unsigned r) { b({rex(true, 0, r), 0xFF, modrm(3, 1, r)}); }
    void push(unsigned r) { if (r & 8) b({0x41}); b({static_cast<std::uint8_t>(0x50 + (r & 7))}); }
    void pop(unsigned r)  { if (r & 8) b({0x41}); b({static_cast<std::uint8_t>(0x58 + (r & 7))}); }
    void syscall() { b({0x0F, 0x05}); }
    void ret() { b({0xC3}); }
    void hlt() { b({0xF4}); }
    void jne(const std::string& l) { b({0x75}); rel8_.push_back({here(), l}); b({0}); }
    void call(const std::string& l) { b({0xE8}); rel32_.push_back({here(), l}); u32(0); }

    void data(const void* p, std::size_t n) {
        auto* c = static_cast<const std::uint8_t*>(p);
        out.insert(out.end(), c, c + n);
    }
    void align(std::size_t a) { while (out.size() % a) out.push_back(0x90); }

    void finish() {
        for (auto& f : rel8_) {
            const auto rel = static_cast<std::int64_t>(labels_.at(f.lbl)) - static_cast<std::int64_t>(f.at + 1);
            if (rel < -128 || rel > 127) throw std::logic_error("rel8 fora de alcance");
            out[f.at] = static_cast<std::uint8_t>(static_cast<std::int8_t>(rel));
        }
        for (auto& f : rel32_) {
            const auto rel = static_cast<std::int32_t>(static_cast<std::int64_t>(labels_.at(f.lbl)) -
                                                       static_cast<std::int64_t>(f.at + 4));
            std::memcpy(&out[f.at], &rel, 4);
        }
        for (auto& f : abs_fixups_) {
            const std::uint64_t va = f.base + labels_.at(f.lbl);
            std::memcpy(&out[f.at], &va, 8);
        }
    }

private:
    struct Fix { std::uint64_t at; std::string lbl; };
    struct AbsFix { std::uint64_t at; std::string lbl; std::uint64_t base; };
    std::map<std::string, std::uint64_t> labels_;
    std::vector<Fix> rel8_, rel32_;
    std::vector<AbsFix> abs_fixups_;
};

const char* efi_type_name(std::uint32_t t) {
    switch (t) {
        case EfiReservedMemoryType:  return "Reserved";
        case EfiLoaderCode:          return "LoaderCode";
        case EfiLoaderData:          return "LoaderData";
        case EfiBootServicesCode:    return "BootServicesCode";
        case EfiBootServicesData:    return "BootServicesData";
        case EfiRuntimeServicesCode: return "RuntimeServicesCode";
        case EfiRuntimeServicesData: return "RuntimeServicesData";
        case EfiConventionalMemory:  return "Conventional";
        case EfiACPIReclaimMemory:   return "ACPIReclaim";
        default:                     return "?";
    }
}
}  // namespace

// ============================================================================
Bios::Bios(MemoryManager& mem, CpuCluster& cpu, GpuDevice& gpu) : mem_(mem), cpu_(cpu), gpu_(gpu) {}

bool Bios::power_on_self_test() {
    std::printf("[XONE-BIOS] UEFI v2.70 (XOne-Emu) — SEC: vetor de reset em 0xFFFFFFF0\n");
    std::printf("[XONE-BIOS] PEI: CPU %s, %u núcleos @ %.2f GHz\n", std::string(spec::kCpu.name).c_str(),
                spec::kCpu.cores, static_cast<double>(spec::kCpu.clock_hz) / 1e9);

    // Teste de memória por amostragem (testar 8 GB inteiros forçaria commit de toda a RAM do host)
    constexpr std::uint64_t patterns[] = {0x5555'5555'5555'5555ull, 0xAAAA'AAAA'AAAA'AAAAull,
                                          0x0123'4567'89AB'CDEFull};
    const std::uint64_t step = mem_.ram_size() / 16;
    unsigned tested = 0;
    for (std::uint64_t pa = spec::layout::kGeneralBase; pa + 8 <= mem_.ram_size(); pa += step) {
        for (auto pat : patterns) {
            std::uint64_t back = 0;
            mem_.write_physical(pa, &pat, 8);
            mem_.read_physical(pa, &back, 8);
            if (back != pat) {
                std::printf("[XONE-BIOS] POST FALHOU: RAM em 0x%llX\n", static_cast<unsigned long long>(pa));
                return false;
            }
        }
        const std::uint64_t zero = 0;
        mem_.write_physical(pa, &zero, 8);
        ++tested;
    }
    std::printf("[XONE-BIOS] PEI: POST OK — %llu MiB %s (%u bancos amostrados)\n",
                static_cast<unsigned long long>(mem_.ram_size() / spec::MiB),
                std::string(spec::kMemory.type).c_str(), tested);
    return true;
}

void Bios::build_memory_map() {
    using namespace spec::layout;
    const auto pages = [](std::uint64_t bytes) { return bytes / spec::kMemory.page_size; };
    map_ = {
        {EfiRuntimeServicesData, 0, kFirmwareBase,  0, pages(kFirmwareSize),  0xF},
        {EfiBootServicesData,    0, kPageTableBase, 0, pages(kPageTableSize), 0xF},
        {EfiLoaderCode,          0, kKernelBase,    0, pages(kKernelSize),    0xF},
        {EfiReservedMemoryType,  0, kGpuRingBase,   0, pages(kGpuRingSize),   0xF},
        {EfiConventionalMemory,  0, kGeneralBase,   0, pages(mem_.ram_size() - kGeneralBase), 0xF},
    };
    // Publica o mapa na região de firmware, como o GetMemoryMap() do UEFI faria.
    mem_.write_physical(kFirmwareBase + 0x1000, map_.data(), map_.size() * sizeof(EfiMemoryDescriptor));

    std::printf("[XONE-BIOS] PEI: mapa de memória EFI (%zu descritores)\n", map_.size());
    for (const auto& d : map_)
        std::printf("            %-20s 0x%010llX - 0x%010llX (%llu páginas)\n", efi_type_name(d.type),
                    static_cast<unsigned long long>(d.physical_start),
                    static_cast<unsigned long long>(d.physical_start + d.number_of_pages * 4096 - 1),
                    static_cast<unsigned long long>(d.number_of_pages));
}

void Bios::initialize_devices(bool vk_validation) {
    std::printf("[XONE-BIOS] DXE: carregando drivers\n");
    gpu_.initialize(vk_validation);
    std::printf("[XONE-BIOS] DXE: %s — latência simulada %llu µs, %llu MiB/s\n",
                std::string(spec::kStorage.name).c_str(),
                static_cast<unsigned long long>(spec::kStorage.read_latency_ns / 1000),
                static_cast<unsigned long long>(spec::kStorage.seq_read_bytes_s / spec::MiB));
}

void Bios::boot(const BootImage& image) {
    using namespace spec;
    if (image.bytes.empty() || image.bytes.size() > layout::kKernelSize)
        throw std::runtime_error("imagem de boot vazia ou maior que a região do kernel");

    std::printf("[XONE-BIOS] BDS: dispositivo de boot -> \"%s\" (%zu bytes)\n", image.name.c_str(), image.bytes.size());

    // 1. Copia a imagem para a RAM física e mapeia em kBootEntryVA (R-X, usuário)
    const std::uint64_t size = (image.bytes.size() + 0xFFF) & ~0xFFFull;
    mem_.write_physical(layout::kKernelBase, image.bytes.data(), image.bytes.size());
    mem_.map(vmap::kBootEntryVA, layout::kKernelBase, size, pte::kUser);

    // 2. Pilha (RW-, NX) e ring buffer da GPU (memória unificada: mesmo PA visto pela GPU)
    mem_.map_anonymous(vmap::kStackTopVA - vmap::kStackSize, vmap::kStackSize,
                       pte::kWritable | pte::kUser | pte::kNoExec);
    mem_.map(vmap::kGpuRingVA, layout::kGpuRingBase, layout::kGpuRingSize,
             pte::kWritable | pte::kUser | pte::kNoExec);

    // 3. ExitBootServices → núcleo 0 recebe o entry point; os outros 7 ficam
    //    estacionados aguardando IPI (criação de threads pelo título).
    cpu_.core(0).reset(vmap::kBootEntryVA + image.entry_offset, vmap::kStackTopVA - 16);
    std::printf("[XONE-BIOS] ExitBootServices: core0 RIP=0x%llX RSP=0x%llX, cores 1-%u estacionados\n",
                static_cast<unsigned long long>(vmap::kBootEntryVA + image.entry_offset),
                static_cast<unsigned long long>(vmap::kStackTopVA - 16), spec::kCpu.cores - 1);
}

// ============================================================================
//  ROM de diagnóstico: exercita decodificador, flags, pilha, memória, HLE e GPU.
// ============================================================================
BootImage Bios::builtin_test_rom() {
    const std::uint64_t base = spec::vmap::kBootEntryVA;
    constexpr std::string_view kBanner = "XOne-Emu diagnostic ROM v0.1 iniciada";
    constexpr std::string_view kSub    = "CALL/RET + PUSH/POP funcionando";
    constexpr std::string_view kBye    = "Diagnostico concluido. Encerrando.";
    // Command buffer PM4 que o "título" envia à GPU (fica na seção de dados da imagem)
    const std::uint32_t pm4buf[] = {
        pm4::type3(pm4::SET_CONTEXT_REG, 2), kXoneClearColorReg - pm4::kContextRegBase, 0x107C10FFu, // verde Xbox
        pm4::type3(pm4::SET_UCONFIG_REG, 2), pm4::VGT_PRIMITIVE_TYPE - pm4::kUconfigRegBase,
                                             static_cast<std::uint32_t>(PrimitiveType::TriList),
        pm4::type3(pm4::NUM_INSTANCES, 1),   1,
        pm4::type3(pm4::DRAW_INDEX_AUTO, 2), 3, 0x2,     // 1 triângulo
        pm4::type3(pm4::NUM_INSTANCES, 1),   4,
        pm4::type3(pm4::DRAW_INDEX_AUTO, 2), 6, 0x2,     // quad instanciado 4x
        pm4::type3(pm4::EVENT_WRITE_EOP, 1), 0,
    };
    Asm a;

    // --- banner
    a.mov_label(RDI, "msg_banner", base);
    a.mov_imm64(RSI, kBanner.size());
    a.mov_imm32(RAX, svc::XDbgPrint);
    a.syscall();

    // --- soma 1..10 com laço (testa ADD, DEC, flags, JNE)
    a.xor32(RAX, RAX);
    a.mov_imm32(RCX, 10);
    a.label("loop");
    a.add(RAX, RCX);
    a.dec(RCX);
    a.jne("loop");
    a.mov(RDI, RAX);
    a.mov_imm32(RAX, svc::XDbgPrintHex);      // esperado: 55 (0x37)
    a.syscall();

    // --- CALL/RET + PUSH/POP
    a.call("subroutine");

    // --- heap: aloca 64 KiB, escreve e lê de volta
    a.mov_imm64(RDI, 0x10000);
    a.mov_imm32(RAX, svc::NtAllocateVirtualMemory);
    a.syscall();
    a.mov(RBX, RAX);
    a.mov_imm64(RCX, 0x0000'0000'DEAD'BEEFull);
    a.store(RBX, RCX);
    a.load(RDX, RBX);
    a.mov(RDI, RDX);
    a.mov_imm32(RAX, svc::XDbgPrintHex);      // esperado: 0xDEADBEEF
    a.syscall();

    // --- GPU: envia command buffer PM4 (2 draws + fim de frame)
    a.mov_label(RDI, "pm4", base);
    a.mov_imm64(RSI, std::size(pm4buf));  // dwords
    a.mov_imm32(RAX, svc::XGpuSubmitCommandBuffer);
    a.syscall();

    // --- encerra o processo
    a.mov_label(RDI, "msg_bye", base);
    a.mov_imm64(RSI, kBye.size());
    a.mov_imm32(RAX, svc::XDbgPrint);
    a.syscall();
    a.xor32(RDI, RDI);
    a.mov_imm32(RAX, svc::NtTerminateProcess);
    a.syscall();
    a.hlt();

    a.label("subroutine");
    a.push(RBX);
    a.mov_label(RDI, "msg_sub", base);
    a.mov_imm64(RSI, kSub.size());
    a.mov_imm32(RAX, svc::XDbgPrint);
    a.syscall();
    a.pop(RBX);
    a.ret();

    // --- dados
    a.label("msg_banner"); a.data(kBanner.data(), kBanner.size());
    a.label("msg_sub");    a.data(kSub.data(), kSub.size());
    a.label("msg_bye");    a.data(kBye.data(), kBye.size());
    a.align(4);
    a.label("pm4");
    a.data(pm4buf, sizeof(pm4buf));
    a.finish();

    return {"XOne-Emu Diagnostic ROM", std::move(a.out), 0};
}

BootImage Bios::load_flat_binary(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("não foi possível abrir " + p.string());
    BootImage img;
    img.name = p.filename().string();
    img.bytes.assign(std::istreambuf_iterator<char>(f), {});
    return img;
}

}  // namespace xone
