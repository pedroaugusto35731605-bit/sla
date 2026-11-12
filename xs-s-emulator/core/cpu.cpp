// =============================================================================
//  SeriesS-Emu — cpu.cpp
//  Decodificador x86-64, semântica das instruções e loop principal (main_loop).
// =============================================================================
#include "cpu.hpp"
#include "jit_engine.hpp"

#include <chrono>
#include <cstdio>
#include <format>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace xss {

// ============================================================================
//  Utilidades
// ============================================================================
const char* reg_name(unsigned r, bool wide) {
    static const char* n64[] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                "r8","r9","r10","r11","r12","r13","r14","r15"};
    static const char* n32[] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi",
                                "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"};
    return (wide ? n64 : n32)[r & 15];
}

namespace {
std::mutex g_log_mu;  // evita linhas embaralhadas quando vários núcleos imprimem

constexpr std::uint64_t width_mask(bool wide) { return wide ? ~0ull : 0xFFFF'FFFFull; }
constexpr std::uint64_t sign_bit(bool wide)   { return wide ? (1ull << 63) : (1ull << 31); }

const char* cc_name(unsigned cc) {
    static const char* n[] = {"o","no","b","ae","e","ne","be","a","s","ns","p","np","l","ge","le","g"};
    return n[cc & 15];
}
}  // namespace

// ============================================================================
//  Instruction
// ============================================================================
bool Instruction::ends_block() const noexcept {
    switch (op) {
        case Op::Call: case Op::Ret: case Op::Jmp: case Op::Jcc:
        case Op::Syscall: case Op::Hlt: case Op::Int3: case Op::Invalid:
            return true;
        default:
            return false;
    }
}

std::string Instruction::hex_bytes() const {
    std::string s;
    for (unsigned i = 0; i < length; ++i) s += std::format("{:02X} ", bytes[i]);
    if (!s.empty()) s.pop_back();
    return s;
}

std::string Instruction::disassemble() const {
    const char* d = reg_name(dst, wide);
    const char* s = reg_name(src, wide);
    switch (op) {
        case Op::Nop:         return "nop";
        case Op::MovRegImm:   return std::format("mov {}, 0x{:X}", d, static_cast<std::uint64_t>(imm));
        case Op::MovRegReg:   return std::format("mov {}, {}", d, s);
        case Op::LoadRegMem:  return std::format("mov {}, [{}{:+d}]", d, reg_name(src), imm);
        case Op::StoreMemReg: return std::format("mov [{}{:+d}], {}", reg_name(dst), imm, s);
        case Op::AddRegReg:   return std::format("add {}, {}", d, s);
        case Op::SubRegReg:   return std::format("sub {}, {}", d, s);
        case Op::XorRegReg:   return std::format("xor {}, {}", d, s);
        case Op::CmpRegReg:   return std::format("cmp {}, {}", d, s);
        case Op::TestRegReg:  return std::format("test {}, {}", d, s);
        case Op::AddRegImm:   return std::format("add {}, {}", d, imm);
        case Op::SubRegImm:   return std::format("sub {}, {}", d, imm);
        case Op::CmpRegImm:   return std::format("cmp {}, {}", d, imm);
        case Op::IncReg:      return std::format("inc {}", d);
        case Op::DecReg:      return std::format("dec {}", d);
        case Op::Push:        return std::format("push {}", reg_name(dst));
        case Op::Pop:         return std::format("pop {}", reg_name(dst));
        case Op::Call:        return std::format("call 0x{:X}", static_cast<std::uint64_t>(imm));
        case Op::Ret:         return "ret";
        case Op::Jmp:         return std::format("jmp 0x{:X}", static_cast<std::uint64_t>(imm));
        case Op::Jcc:         return std::format("j{} 0x{:X}", cc_name(cond), static_cast<std::uint64_t>(imm));
        case Op::Syscall:     return "syscall";
        case Op::Hlt:         return "hlt";
        case Op::Int3:        return "int3";
        default:              return "(bad)";
    }
}

// ============================================================================
//  Decoder — subconjunto x86-64 com REX
// ============================================================================
Instruction Decoder::decode(MemoryManager& mem, GuestVA rip) {
    Instruction in;
    in.address = rip;

    unsigned n = 0;
    auto next = [&]() -> std::uint8_t {
        if (n >= 15) throw std::runtime_error(std::format("#GP instrução > 15 bytes em 0x{:X}", rip));
        const auto b = mem.read<std::uint8_t>(rip + n, Access::Execute);
        in.bytes[n++] = b;
        return b;
    };
    auto imm8  = [&] { return static_cast<std::int64_t>(static_cast<std::int8_t>(next())); };
    auto imm32 = [&] {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(next()) << (8 * i);
        return static_cast<std::int64_t>(static_cast<std::int32_t>(v));
    };
    auto imm64 = [&] {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(next()) << (8 * i);
        return static_cast<std::int64_t>(v);
    };
    auto finish = [&](Op op) { in.op = op; in.length = static_cast<std::uint8_t>(n); return in; };
    auto rel_target = [&](std::int64_t rel) {
        return static_cast<std::int64_t>(rip + n + static_cast<std::uint64_t>(rel));
    };

    // ---- prefixo REX
    bool rexW = false, rexR = false, rexB = false;
    std::uint8_t b = next();
    if ((b & 0xF0) == 0x40) {
        rexW = b & 8; rexR = b & 4; rexB = b & 1;
        b = next();
    }
    in.wide = rexW;

    struct ModRM { unsigned mod, reg, rm; };
    auto modrm = [&] {
        const std::uint8_t m = next();
        return ModRM{static_cast<unsigned>(m >> 6),
                     static_cast<unsigned>(((m >> 3) & 7) | (rexR ? 8 : 0)),
                     static_cast<unsigned>((m & 7) | (rexB ? 8 : 0))};
    };
    // Endereço de memória simples [base] ou [base+disp8/disp32] (sem SIB/RIP-relative)
    auto mem_operand = [&](const ModRM& m) -> bool {
        if (m.mod == 3 || (m.rm & 7) == 4) return false;
        if (m.mod == 0 && (m.rm & 7) == 5) return false;
        in.imm = m.mod == 1 ? imm8() : m.mod == 2 ? imm32() : 0;
        return true;
    };

    switch (b) {
        // ---- ALU r/m, r
        case 0x01: case 0x29: case 0x31: case 0x39: case 0x85: case 0x89: {
            const ModRM m = modrm();
            in.src = static_cast<std::uint8_t>(m.reg);
            in.dst = static_cast<std::uint8_t>(m.rm);
            if (m.mod == 3) {
                switch (b) {
                    case 0x01: return finish(Op::AddRegReg);
                    case 0x29: return finish(Op::SubRegReg);
                    case 0x31: return finish(Op::XorRegReg);
                    case 0x39: return finish(Op::CmpRegReg);
                    case 0x85: return finish(Op::TestRegReg);
                    case 0x89: return finish(Op::MovRegReg);
                }
            }
            if (b == 0x89 && mem_operand(m)) return finish(Op::StoreMemReg);
            break;
        }
        case 0x8B: {  // mov r, r/m
            const ModRM m = modrm();
            in.dst = static_cast<std::uint8_t>(m.reg);
            in.src = static_cast<std::uint8_t>(m.rm);
            if (m.mod == 3) return finish(Op::MovRegReg);
            if (mem_operand(m)) return finish(Op::LoadRegMem);
            break;
        }
        case 0x83: {  // grupo 1 com imm8
            const ModRM m = modrm();
            if (m.mod != 3) break;
            in.dst = static_cast<std::uint8_t>(m.rm);
            in.imm = imm8();
            switch (m.reg & 7) {
                case 0: return finish(Op::AddRegImm);
                case 5: return finish(Op::SubRegImm);
                case 7: return finish(Op::CmpRegImm);
            }
            break;
        }
        case 0xFF: {  // grupo 5
            const ModRM m = modrm();
            if (m.mod != 3) break;
            in.dst = static_cast<std::uint8_t>(m.rm);
            if ((m.reg & 7) == 0) return finish(Op::IncReg);
            if ((m.reg & 7) == 1) return finish(Op::DecReg);
            break;
        }
        case 0x90: return finish(Op::Nop);
        case 0xC3: return finish(Op::Ret);
        case 0xCC: return finish(Op::Int3);
        case 0xF4: return finish(Op::Hlt);
        case 0xE8: { auto r = imm32(); in.imm = rel_target(r); return finish(Op::Call); }
        case 0xE9: { auto r = imm32(); in.imm = rel_target(r); return finish(Op::Jmp); }
        case 0xEB: { auto r = imm8();  in.imm = rel_target(r); return finish(Op::Jmp); }
        case 0x0F: {
            const std::uint8_t b2 = next();
            if (b2 == 0x05) return finish(Op::Syscall);
            if ((b2 & 0xF0) == 0x80) {
                in.cond = b2 & 0x0F;
                auto r = imm32(); in.imm = rel_target(r);
                return finish(Op::Jcc);
            }
            if (b2 == 0x1F) {  // NOP multi-byte (0F 1F /0)
                const ModRM m = modrm();
                if (m.mod != 3 && (m.rm & 7) == 4) {
                    const std::uint8_t sib = next();
                    if (m.mod == 0 && (sib & 7) == 5) imm32();
                }
                if (m.mod == 1) imm8();
                else if (m.mod == 2 || (m.mod == 0 && (m.rm & 7) == 5)) imm32();
                return finish(Op::Nop);
            }
            break;
        }
        default:
            if (b >= 0xB8 && b <= 0xBF) {  // mov r, imm32/imm64
                in.dst = static_cast<std::uint8_t>((b & 7) | (rexB ? 8 : 0));
                in.imm = rexW ? imm64() : static_cast<std::int64_t>(static_cast<std::uint32_t>(imm32()));
                return finish(Op::MovRegImm);
            }
            if (b >= 0x50 && b <= 0x57) {
                in.dst = static_cast<std::uint8_t>((b & 7) | (rexB ? 8 : 0));
                in.wide = true;
                return finish(Op::Push);
            }
            if (b >= 0x58 && b <= 0x5F) {
                in.dst = static_cast<std::uint8_t>((b & 7) | (rexB ? 8 : 0));
                in.wide = true;
                return finish(Op::Pop);
            }
            if (b >= 0x70 && b <= 0x7F) {
                in.cond = b & 0x0F;
                auto r = imm8(); in.imm = rel_target(r);
                return finish(Op::Jcc);
            }
            break;
    }
    in.length = static_cast<std::uint8_t>(n);
    in.op = Op::Invalid;
    return in;
}

// ============================================================================
//  CpuCore
// ============================================================================
CpuCore::CpuCore(unsigned id, MemoryManager& mem, SystemCalls& kernel)
    : id_(id), mem_(mem), kernel_(kernel) {}

void CpuCore::reset(GuestVA entry, GuestVA stack_top) {
    st_ = CpuState{};
    st_.rip = entry;
    st_.gpr[RSP] = stack_top;
    halted_ = false;
}

void CpuCore::set(unsigned r, std::uint64_t v, bool wide) noexcept {
    st_.gpr[r] = v & width_mask(wide);  // escrita de 32 bits zera a metade alta (regra x86-64)
}

void CpuCore::push(std::uint64_t v) {
    st_.gpr[RSP] -= 8;
    mem_.write<std::uint64_t>(st_.gpr[RSP], v);
}

std::uint64_t CpuCore::pop() {
    const auto v = mem_.read<std::uint64_t>(st_.gpr[RSP]);
    st_.gpr[RSP] += 8;
    return v;
}

bool CpuCore::condition(unsigned cc) const noexcept {
    const auto f = st_.rflags;
    const bool CF = f & flags::CF, ZF = f & flags::ZF, SF = f & flags::SF, OF = f & flags::OF;
    bool r = false;
    switch (cc >> 1) {
        case 0: r = OF; break;
        case 1: r = CF; break;
        case 2: r = ZF; break;
        case 3: r = CF || ZF; break;
        case 4: r = SF; break;
        case 5: r = false; break;          // paridade: PF não emulado
        case 6: r = SF != OF; break;
        case 7: r = ZF || (SF != OF); break;
    }
    return (cc & 1) ? !r : r;
}

void CpuCore::set_flags_logic(std::uint64_t r, bool wide) noexcept {
    r &= width_mask(wide);
    std::uint64_t f = st_.rflags & ~(flags::CF | flags::ZF | flags::SF | flags::OF);
    if (r == 0) f |= flags::ZF;
    if (r & sign_bit(wide)) f |= flags::SF;
    st_.rflags = f;
}

void CpuCore::set_flags_add(std::uint64_t a, std::uint64_t b, std::uint64_t r, bool wide) noexcept {
    const auto m = width_mask(wide);
    a &= m; b &= m; r &= m;
    set_flags_logic(r, wide);
    if (r < a) st_.rflags |= flags::CF;
    if (((a ^ r) & (b ^ r)) & sign_bit(wide)) st_.rflags |= flags::OF;
}

void CpuCore::set_flags_sub(std::uint64_t a, std::uint64_t b, std::uint64_t r, bool wide) noexcept {
    const auto m = width_mask(wide);
    a &= m; b &= m; r &= m;
    set_flags_logic(r, wide);
    if (a < b) st_.rflags |= flags::CF;
    if (((a ^ b) & (a ^ r)) & sign_bit(wide)) st_.rflags |= flags::OF;
}

void CpuCore::step(bool trace) {
    const Instruction in = Decoder::decode(mem_, st_.rip);
    if (trace) {
        std::lock_guard lk(g_log_mu);
        std::printf("[XSS-CORE] Executing instruction at address 0x%016llX | core%u | %-30s | %s\n",
                    static_cast<unsigned long long>(in.address), id_, in.hex_bytes().c_str(),
                    in.disassemble().c_str());
    }
    op_table()[static_cast<std::size_t>(in.op)](*this, in);
    account(1, in.op == Op::Syscall ? 100 : (in.op == Op::LoadRegMem || in.op == Op::StoreMemReg) ? 3 : 1);
}

// ============================================================================
//  Semântica das instruções (tabela compartilhada interpretador/JIT)
// ============================================================================
namespace {
inline void advance(CpuCore& c, const Instruction& i) { c.state().rip = i.address + i.length; }

void op_invalid(CpuCore& c, const Instruction& i) {
    c.halt();
    throw std::runtime_error(std::format("#UD instrução não suportada em 0x{:016X}: [{}]",
                                         i.address, i.hex_bytes()));
}
void op_nop(CpuCore& c, const Instruction& i)       { advance(c, i); }
void op_mov_imm(CpuCore& c, const Instruction& i)   { c.set(i.dst, static_cast<std::uint64_t>(i.imm), i.wide); advance(c, i); }
void op_mov_rr(CpuCore& c, const Instruction& i)    { c.set(i.dst, c.get(i.src), i.wide); advance(c, i); }
void op_load(CpuCore& c, const Instruction& i) {
    const GuestVA ea = c.get(i.src) + static_cast<std::uint64_t>(i.imm);
    const std::uint64_t v = i.wide ? c.memory().read<std::uint64_t>(ea) : c.memory().read<std::uint32_t>(ea);
    c.set(i.dst, v, i.wide);
    advance(c, i);
}
void op_store(CpuCore& c, const Instruction& i) {
    const GuestVA ea = c.get(i.dst) + static_cast<std::uint64_t>(i.imm);
    if (i.wide) c.memory().write<std::uint64_t>(ea, c.get(i.src));
    else        c.memory().write<std::uint32_t>(ea, static_cast<std::uint32_t>(c.get(i.src)));
    advance(c, i);
}
void op_add_rr(CpuCore& c, const Instruction& i) {
    const auto a = c.get(i.dst), b = c.get(i.src), r = a + b;
    c.set_flags_add(a, b, r, i.wide); c.set(i.dst, r, i.wide); advance(c, i);
}
void op_sub_rr(CpuCore& c, const Instruction& i) {
    const auto a = c.get(i.dst), b = c.get(i.src), r = a - b;
    c.set_flags_sub(a, b, r, i.wide); c.set(i.dst, r, i.wide); advance(c, i);
}
void op_xor_rr(CpuCore& c, const Instruction& i) {
    const auto r = c.get(i.dst) ^ c.get(i.src);
    c.set_flags_logic(r, i.wide); c.set(i.dst, r, i.wide); advance(c, i);
}
void op_cmp_rr(CpuCore& c, const Instruction& i) {
    const auto a = c.get(i.dst), b = c.get(i.src);
    c.set_flags_sub(a, b, a - b, i.wide); advance(c, i);
}
void op_test_rr(CpuCore& c, const Instruction& i) {
    c.set_flags_logic(c.get(i.dst) & c.get(i.src), i.wide); advance(c, i);
}
void op_add_ri(CpuCore& c, const Instruction& i) {
    const auto a = c.get(i.dst), b = static_cast<std::uint64_t>(i.imm), r = a + b;
    c.set_flags_add(a, b, r, i.wide); c.set(i.dst, r, i.wide); advance(c, i);
}
void op_sub_ri(CpuCore& c, const Instruction& i) {
    const auto a = c.get(i.dst), b = static_cast<std::uint64_t>(i.imm), r = a - b;
    c.set_flags_sub(a, b, r, i.wide); c.set(i.dst, r, i.wide); advance(c, i);
}
void op_cmp_ri(CpuCore& c, const Instruction& i) {
    const auto a = c.get(i.dst), b = static_cast<std::uint64_t>(i.imm);
    c.set_flags_sub(a, b, a - b, i.wide); advance(c, i);
}
void op_inc(CpuCore& c, const Instruction& i) {  // INC/DEC preservam CF
    const auto cf = c.state().rflags & flags::CF;
    const auto a = c.get(i.dst), r = a + 1;
    c.set_flags_add(a, 1, r, i.wide);
    c.state().rflags = (c.state().rflags & ~flags::CF) | cf;
    c.set(i.dst, r, i.wide); advance(c, i);
}
void op_dec(CpuCore& c, const Instruction& i) {
    const auto cf = c.state().rflags & flags::CF;
    const auto a = c.get(i.dst), r = a - 1;
    c.set_flags_sub(a, 1, r, i.wide);
    c.state().rflags = (c.state().rflags & ~flags::CF) | cf;
    c.set(i.dst, r, i.wide); advance(c, i);
}
void op_push(CpuCore& c, const Instruction& i) { c.push(c.get(i.dst)); advance(c, i); }
void op_pop(CpuCore& c, const Instruction& i)  { const auto v = c.pop(); c.set(i.dst, v, true); advance(c, i); }
void op_call(CpuCore& c, const Instruction& i) {
    c.push(i.address + i.length);
    c.state().rip = static_cast<std::uint64_t>(i.imm);
}
void op_ret(CpuCore& c, const Instruction&)    { c.state().rip = c.pop(); }
void op_jmp(CpuCore& c, const Instruction& i)  { c.state().rip = static_cast<std::uint64_t>(i.imm); }
void op_jcc(CpuCore& c, const Instruction& i) {
    c.state().rip = c.condition(i.cond) ? static_cast<std::uint64_t>(i.imm) : i.address + i.length;
}
void op_syscall(CpuCore& c, const Instruction& i) {
    advance(c, i);
    c.state().gpr[RCX] = c.state().rip;      // semântica real do SYSCALL
    c.state().gpr[R11] = c.state().rflags;
    c.kernel().dispatch(c);
}
void op_hlt(CpuCore& c, const Instruction& i)  { advance(c, i); c.halt(); }
void op_int3(CpuCore& c, const Instruction& i) {
    advance(c, i);
    std::printf("[XSS-CORE] core%u: breakpoint (int3) em 0x%llX\n", c.id(),
                static_cast<unsigned long long>(i.address));
    c.halt();
}
}  // namespace

const std::array<OpHandler, static_cast<std::size_t>(Op::Count)>& op_table() {
    static const std::array<OpHandler, static_cast<std::size_t>(Op::Count)> table = [] {
        std::array<OpHandler, static_cast<std::size_t>(Op::Count)> t{};
        t.fill(op_invalid);
        auto set = [&](Op o, OpHandler h) { t[static_cast<std::size_t>(o)] = h; };
        set(Op::Nop, op_nop);             set(Op::MovRegImm, op_mov_imm);
        set(Op::MovRegReg, op_mov_rr);    set(Op::LoadRegMem, op_load);
        set(Op::StoreMemReg, op_store);   set(Op::AddRegReg, op_add_rr);
        set(Op::SubRegReg, op_sub_rr);    set(Op::XorRegReg, op_xor_rr);
        set(Op::CmpRegReg, op_cmp_rr);    set(Op::TestRegReg, op_test_rr);
        set(Op::AddRegImm, op_add_ri);    set(Op::SubRegImm, op_sub_ri);
        set(Op::CmpRegImm, op_cmp_ri);    set(Op::IncReg, op_inc);
        set(Op::DecReg, op_dec);          set(Op::Push, op_push);
        set(Op::Pop, op_pop);             set(Op::Call, op_call);
        set(Op::Ret, op_ret);             set(Op::Jmp, op_jmp);
        set(Op::Jcc, op_jcc);             set(Op::Syscall, op_syscall);
        set(Op::Hlt, op_hlt);             set(Op::Int3, op_int3);
        return t;
    }();
    return table;
}

// ============================================================================
//  CpuCluster — main_loop
// ============================================================================
CpuCluster::CpuCluster(MemoryManager& mem, SystemCalls& kernel) : mem_(mem), kernel_(kernel) {
    for (unsigned i = 0; i < spec::kCpu.threads; ++i)  // 16 threads lógicas (SMT2)
        cores_.push_back(std::make_unique<CpuCore>(i, mem_, kernel_));
    jit_ = std::make_unique<JitEngine>(mem_);
}

CpuCluster::~CpuCluster() = default;

CpuCluster::Report CpuCluster::main_loop(const RunOptions& opt) {
    Report rep;
    std::mutex rep_mu;
    const auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (auto& up : cores_) {
        CpuCore* c = up.get();
        if (c->halted()) continue;  // núcleo estacionado
        threads.emplace_back([&, c] {
            std::uint64_t n = 0;
            try {
                while (!c->halted() && !kernel_.terminated() && n < opt.max_instructions) {
                    if (opt.mode == Mode::Jit) n += jit_->execute_block(*c, opt.trace);
                    else { c->step(opt.trace); ++n; }
                }
            } catch (const std::exception& e) {
                c->halt();
                std::lock_guard lk(rep_mu);
                rep.fault = std::format("core{}: {}", c->id(), e.what());
            }
            if (n >= opt.max_instructions) {
                std::lock_guard lk(rep_mu);
                rep.budget_exhausted = true;
            }
        });
    }
    for (auto& t : threads) t.join();

    std::uint64_t max_cycles = 0;
    for (auto& c : cores_) {
        rep.instructions += c->instructions_retired();
        rep.cycles       += c->cycles();
        max_cycles = std::max(max_cycles, c->cycles());
    }
    rep.emulated_seconds = static_cast<double>(max_cycles) / static_cast<double>(spec::kCpu.clock_hz);
    rep.host_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    rep.exit_code = kernel_.exit_code();
    return rep;
}

}  // namespace xss
