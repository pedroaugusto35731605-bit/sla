// =============================================================================
//  SeriesS-Emu — jit_engine.cpp
// =============================================================================
#include "jit_engine.hpp"

#include "log.hpp"

#include <cstdio>

namespace xss {

std::unique_ptr<TranslatedBlock> JitEngine::translate(GuestVA rip) {
    auto blk = std::make_unique<TranslatedBlock>();
    blk->entry = rip;
    GuestVA pc = rip;
    const auto& table = op_table();

    for (std::size_t i = 0; i < kMaxBlockInsns; ++i) {
        Instruction in = Decoder::decode(mem_, pc);
        blk->ops.push_back(table[static_cast<std::size_t>(in.op)]);
        blk->cycles += (in.op == Op::Syscall) ? 100
                     : (in.op == Op::LoadRegMem || in.op == Op::StoreMemReg) ? 3 : 1;
        pc += in.length;
        const bool end = in.ends_block();
        blk->insns.push_back(std::move(in));
        if (end) break;
    }
    blk->end = pc;
    return blk;
}

TranslatedBlock* JitEngine::lookup_or_translate(GuestVA rip) {
    std::lock_guard lk(mu_);
    if (auto it = cache_.find(rip); it != cache_.end()) {
        ++stats_.cache_hits;
        return it->second.get();
    }
    auto blk = translate(rip);
    ++stats_.blocks_translated;
    log_debug("JIT", "bloco traduzido 0x{:016X}..0x{:016X} ({} instruções)", blk->entry, blk->end, blk->insns.size());
    auto* raw = blk.get();
    cache_.emplace(rip, std::move(blk));
    return raw;
}

std::uint64_t JitEngine::execute_block(CpuCore& core, bool trace) {
    TranslatedBlock* blk = lookup_or_translate(core.state().rip);
    ++blk->exec_count;
    if (trace)
        std::printf("[XSS-CORE] Executing block at address 0x%016llX | core%u | exec #%llu\n",
                    static_cast<unsigned long long>(blk->entry), core.id(),
                    static_cast<unsigned long long>(blk->exec_count));

    std::uint64_t n = 0;
    for (std::size_t i = 0; i < blk->ops.size(); ++i) {
        blk->ops[i](core, blk->insns[i]);
        ++n;
        if (core.halted()) break;
    }
    core.account(n, blk->cycles);
    return n;
}

void JitEngine::invalidate(GuestVA va, std::uint64_t size) {
    std::lock_guard lk(mu_);
    for (auto it = cache_.begin(); it != cache_.end();) {
        const auto& b = *it->second;
        if (b.entry < va + size && va < b.end) {
            it = cache_.erase(it);
            ++stats_.invalidations;
        } else {
            ++it;
        }
    }
}

void JitEngine::flush() {
    std::lock_guard lk(mu_);
    stats_.invalidations += cache_.size();
    cache_.clear();
}

JitEngine::Stats JitEngine::stats() const {
    std::lock_guard lk(mu_);
    Stats s = stats_;
    s.cached_blocks = cache_.size();
    return s;
}

}  // namespace xss
