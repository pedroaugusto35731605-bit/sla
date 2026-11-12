// =============================================================================
//  XOne-Emu — testes do núcleo (sem framework externo)
// =============================================================================
#include "bios.hpp"
#include "cpu.hpp"
#include "gpu.hpp"
#include "jit_engine.hpp"
#include "memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace xone;

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (cond) ++g_passed;                                                        \
        else { ++g_failed; std::printf("  FALHOU %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static void test(const char* name, const std::function<void()>& fn) {
    std::printf("[TEST] %s\n", name);
    try { fn(); } catch (const std::exception& e) { ++g_failed; std::printf("  EXCEÇÃO: %s\n", e.what()); }
}

// Executa bytes de código em core0 até HLT.
static CpuCore& run_code(MemoryManager& mem, CpuCluster& cl, const std::vector<std::uint8_t>& code,
                         CpuCluster::Mode mode = CpuCluster::Mode::Interpreter) {
    const GuestVA va = 0x400000;
    const GuestPA pa = mem.map_anonymous(va, 0x1000, pte::kWritable | pte::kUser);
    mem.write_physical(pa, code.data(), code.size());
    mem.map_anonymous(0x7FFE0000, 0x10000, pte::kWritable | pte::kUser | pte::kNoExec);
    cl.core(0).reset(va, 0x7FFF0000 - 16);
    CpuCluster::RunOptions o; o.trace = false; o.mode = mode; o.max_instructions = 10000;
    const auto rep = cl.main_loop(o);
    if (!rep.fault.empty()) std::printf("  fault: %s\n", rep.fault.c_str());
    return cl.core(0);
}

int main() {
    test("memória: 8 GiB reservados e page walk de 4 níveis", [] {
        MemoryManager mem;
        CHECK(mem.ram_size() == 8ull * 1024 * 1024 * 1024);
        const GuestPA pa = mem.allocate_frames(0x2000);
        mem.map(0x1234'5000, pa, 0x2000, pte::kWritable | pte::kUser);
        CHECK(mem.translate(0x1234'5678, Access::Read) == pa + 0x678);
        CHECK(mem.translate(0x1234'6010, Access::Read) == pa + 0x1010);
        mem.write<std::uint64_t>(0x1234'5FFC, 0x1122334455667788ull);   // cruza fronteira de página
        CHECK(mem.read<std::uint64_t>(0x1234'5FFC) == 0x1122334455667788ull);
        // PML4 real: entrada presente no índice correto do CR3
        std::uint64_t pml4e = 0;
        mem.read_physical(mem.cr3() + ((0x1234'5000ull >> 39) & 0x1FF) * 8, &pml4e, 8);
        CHECK((pml4e & pte::kPresent) != 0);
    });

    test("memória: page faults (não presente, somente leitura, NX)", [] {
        MemoryManager mem;
        bool f1 = false, f2 = false, f3 = false;
        try { mem.read<std::uint8_t>(0xDEAD'0000); } catch (const PageFault&) { f1 = true; }
        mem.map_anonymous(0x5000'0000, 0x1000, pte::kUser);                       // RO
        try { mem.write<std::uint8_t>(0x5000'0000, 1); } catch (const PageFault&) { f2 = true; }
        mem.map_anonymous(0x5000'1000, 0x1000, pte::kUser | pte::kNoExec);
        try { mem.read<std::uint8_t>(0x5000'1000, Access::Execute); } catch (const PageFault&) { f3 = true; }
        CHECK(f1); CHECK(f2); CHECK(f3);
        mem.unmap(0x5000'0000, 0x1000);
        bool f4 = false;
        try { mem.read<std::uint8_t>(0x5000'0000); } catch (const PageFault&) { f4 = true; }
        CHECK(f4);
    });

    test("cpu: aritmética, flags, laço e pilha", [] {
        MemoryManager mem; KernelHle k(mem); CpuCluster cl(mem, k);
        // mov ecx,5 ; xor eax,eax ; L: add rax,rcx ; dec rcx ; jne L ; push rax ; pop rbx ; hlt
        auto& c = run_code(mem, cl, {0xB9,5,0,0,0, 0x31,0xC0, 0x48,0x01,0xC8, 0x48,0xFF,0xC9, 0x75,0xF8,
                                     0x50, 0x5B, 0xF4});
        CHECK(c.state().gpr[RAX] == 15);
        CHECK(c.state().gpr[RBX] == 15);
        CHECK(c.state().gpr[RCX] == 0);
        CHECK(c.state().gpr[RSP] == 0x7FFF0000 - 16);
    });

    test("cpu: escrita de 32 bits zera metade alta / CMP + JL com sinal", [] {
        MemoryManager mem; KernelHle k(mem); CpuCluster cl(mem, k);
        // mov rax, -1 ; mov eax, 7 ; mov rdx, -3 ; cmp rdx, 2 ; jl +5 ; mov ebx,1 ; hlt ; mov ebx,2 ; hlt
        std::vector<std::uint8_t> code = {0x48,0xB8, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                          0xB8,7,0,0,0,
                                          0x48,0xBA, 0xFD,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                          0x48,0x83,0xFA,0x02,
                                          0x7C,0x06,
                                          0xBB,1,0,0,0, 0xF4,
                                          0xBB,2,0,0,0, 0xF4};
        auto& c = run_code(mem, cl, code);
        CHECK(c.state().gpr[RAX] == 7);
        CHECK(c.state().gpr[RBX] == 2);
    });

    test("cpu: instrução inválida gera #UD", [] {
        MemoryManager mem; KernelHle k(mem); CpuCluster cl(mem, k);
        const GuestPA pa = mem.map_anonymous(0x400000, 0x1000, pte::kUser);
        const std::uint8_t ud[] = {0x0F, 0x0B};
        mem.write_physical(pa, ud, 2);
        cl.core(0).reset(0x400000, 0);
        CpuCluster::RunOptions o; o.trace = false;
        const auto rep = cl.main_loop(o);
        CHECK(rep.fault.find("#UD") != std::string::npos);
    });

    test("boot completo da ROM de diagnóstico (interpretador e JIT)", [] {
        for (auto mode : {CpuCluster::Mode::Interpreter, CpuCluster::Mode::Jit}) {
            MemoryManager mem; KernelHle k(mem); CpuCluster cl(mem, k); GpuDevice gpu(mem);
            k.set_gpu_submit([&](GuestVA va, std::uint32_t n) { gpu.submit_command_buffer(va, n); });
            Bios bios(mem, cl, gpu);
            CHECK(bios.power_on_self_test());
            bios.build_memory_map();
            bios.boot(Bios::builtin_test_rom());
            CpuCluster::RunOptions o; o.trace = false; o.mode = mode;
            const auto rep = cl.main_loop(o);
            CHECK(rep.fault.empty());
            CHECK(k.terminated());
            CHECK(rep.exit_code == 0);
            CHECK(gpu.stats().draws == 2);
            CHECK(gpu.stats().vertices == 3 + 6 * 4);
            CHECK(gpu.stats().frames == 1);
            if (mode == CpuCluster::Mode::Jit) CHECK(cl.jit().stats().blocks_translated > 0);
        }
    });

    std::printf("\n%d verificações OK, %d falhas\n", g_passed, g_failed);
    return g_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
