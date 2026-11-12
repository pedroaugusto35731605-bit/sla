// =============================================================================
//  SeriesS-Emu — cpu.hpp
//  Emulação do AMD Zen 2 (x86-64): decodificador, interpretador e cluster de
//  16 threads lógicas (8 núcleos × SMT2). Reaproveitado do XOne-Emu.
//
//  Pipeline:  fetch (MemoryManager, Access::Execute)
//          →  decode (Decoder → Instruction)
//          →  execute (tabela de handlers `op_table()`, compartilhada com o JIT)
//
//  O decodificador cobre um subconjunto inteiro do ISA x86-64 (MOV/ADD/SUB/XOR/
//  CMP/TEST/INC/DEC/PUSH/POP/CALL/RET/JMP/Jcc/SYSCALL/HLT) com prefixo REX.
//  Instruções fora do subconjunto geram #UD com o endereço e os bytes.
// =============================================================================
#pragma once

#include "hardware_spec.hpp"
#include "memory_manager.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xss {

class JitEngine;

enum Reg : std::uint8_t {
    RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15
};
const char* reg_name(unsigned r, bool wide = true);

// Bits de RFLAGS emulados
namespace flags {
    inline constexpr std::uint64_t CF = 1ull << 0;
    inline constexpr std::uint64_t ZF = 1ull << 6;
    inline constexpr std::uint64_t SF = 1ull << 7;
    inline constexpr std::uint64_t OF = 1ull << 11;
    inline constexpr std::uint64_t kReserved1 = 1ull << 1;  // bit 1 sempre 1
}

enum class Op : std::uint8_t {
    Invalid, Nop,
    MovRegImm, MovRegReg, LoadRegMem, StoreMemReg,
    AddRegReg, SubRegReg, XorRegReg, CmpRegReg, TestRegReg,
    AddRegImm, SubRegImm, CmpRegImm,
    IncReg, DecReg,
    Push, Pop, Call, Ret, Jmp, Jcc,
    Syscall, Hlt, Int3,
    Count
};

struct Instruction {
    GuestVA       address = 0;
    std::uint8_t  length  = 0;
    Op            op      = Op::Invalid;
    std::uint8_t  dst     = 0;     // registrador destino (ou base de memória)
    std::uint8_t  src     = 0;     // registrador fonte
    std::uint8_t  cond    = 0;     // condição para Jcc (0x0..0xF)
    bool          wide    = true;  // REX.W → 64 bits; senão 32 bits com zero-extend
    std::int64_t  imm     = 0;     // imediato ou deslocamento relativo
    std::array<std::uint8_t, 15> bytes{};

    bool        ends_block() const noexcept;
    std::string disassemble() const;
    std::string hex_bytes() const;
};

class Decoder {
public:
    static Instruction decode(MemoryManager& mem, GuestVA rip);
};

struct CpuState {
    std::array<std::uint64_t, 16> gpr{};
    std::uint64_t rip    = 0;
    std::uint64_t rflags = flags::kReserved1;
};

class CpuCore;

// -----------------------------------------------------------------------------
//  Interface de chamadas de sistema. Implementada pelo kernel do Xbox OS em
//  bios_uefi.hpp (XboxOsKernel); mantém a CPU independente do sistema operacional.
// -----------------------------------------------------------------------------
class SystemCalls {
public:
    virtual ~SystemCalls() = default;
    virtual void         dispatch(CpuCore& core) = 0;     // chamado pela instrução SYSCALL
    virtual bool         terminated() const noexcept = 0;
    virtual std::int64_t exit_code() const noexcept = 0;
};

// -----------------------------------------------------------------------------
class CpuCore {
public:
    CpuCore(unsigned id, MemoryManager& mem, SystemCalls& kernel);

    unsigned        id()    const noexcept { return id_; }
    CpuState&       state()       noexcept { return st_; }
    const CpuState& state() const noexcept { return st_; }
    MemoryManager&  memory()      noexcept { return mem_; }
    SystemCalls&      kernel()      noexcept { return kernel_; }

    bool halted() const noexcept { return halted_; }
    void halt()   noexcept { halted_ = true; }
    void reset(GuestVA entry, GuestVA stack_top);

    // Um ciclo busca → decodifica → executa (interpretador).
    void step(bool trace);

    // Helpers usados pelos handlers de instrução
    std::uint64_t get(unsigned r) const noexcept { return st_.gpr[r]; }
    void          set(unsigned r, std::uint64_t v, bool wide) noexcept;
    void          push(std::uint64_t v);
    std::uint64_t pop();
    bool          condition(unsigned cc) const noexcept;
    void          set_flags_logic(std::uint64_t r, bool wide) noexcept;
    void          set_flags_add(std::uint64_t a, std::uint64_t b, std::uint64_t r, bool wide) noexcept;
    void          set_flags_sub(std::uint64_t a, std::uint64_t b, std::uint64_t r, bool wide) noexcept;

    std::uint64_t instructions_retired() const noexcept { return retired_; }
    std::uint64_t cycles() const noexcept { return cycles_; }
    void          account(std::uint64_t insns, std::uint64_t cycles) noexcept {
        retired_ += insns; cycles_ += cycles;
    }

private:
    unsigned       id_;
    MemoryManager& mem_;
    SystemCalls&     kernel_;
    CpuState       st_{};
    bool           halted_  = true;   // núcleos começam estacionados até receber reset/IPI
    std::uint64_t  retired_ = 0;
    std::uint64_t  cycles_  = 0;
};

// Handler de instrução: mesma função serve ao interpretador e ao JIT.
using OpHandler = void (*)(CpuCore&, const Instruction&);
const std::array<OpHandler, static_cast<std::size_t>(Op::Count)>& op_table();

// -----------------------------------------------------------------------------
class CpuCluster {
public:
    enum class Mode { Interpreter, Jit };
    struct RunOptions {
        Mode          mode             = Mode::Interpreter;
        bool          trace            = true;
        std::uint64_t max_instructions = 10'000'000;   // por núcleo
    };
    struct Report {
        std::uint64_t instructions = 0;
        std::uint64_t cycles       = 0;
        double        emulated_seconds = 0.0;
        double        host_seconds     = 0.0;
        std::int64_t  exit_code        = 0;
        std::string   fault;          // vazio se terminou normalmente
        bool          budget_exhausted = false;
    };

    CpuCluster(MemoryManager& mem, SystemCalls& kernel);
    ~CpuCluster();

    CpuCore&  core(unsigned i) { return *cores_.at(i); }
    unsigned  core_count() const noexcept { return static_cast<unsigned>(cores_.size()); }
    JitEngine& jit() { return *jit_; }

    // Loop principal de emulação. Cada núcleo ativo roda em uma thread do host.
    Report main_loop(const RunOptions& opt);

private:
    MemoryManager& mem_;
    SystemCalls&     kernel_;
    std::vector<std::unique_ptr<CpuCore>> cores_;
    std::unique_ptr<JitEngine> jit_;
};

}  // namespace xss
