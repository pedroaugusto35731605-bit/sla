// =============================================================================
//  XOne-Emu — jit_engine.hpp
//  JIT básico: traduz blocos básicos do convidado (x86-64 Xbox) em blocos
//  "threaded code" do host — uma sequência pré-decodificada de ponteiros para
//  handlers especializados, armazenada em cache por endereço de entrada.
//
//  É a primeira camada (tier-1) de um tradutor binário: elimina fetch/decode
//  repetidos, que dominam o custo do interpretador. A camada tier-2 (emissão de
//  código nativo x86-64/ARM64 com alocação de registradores) se encaixa em
//  `TranslatedBlock::native` sem mudar a interface do CpuCluster.
// =============================================================================
#pragma once

#include "cpu.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace xone {

struct TranslatedBlock {
    GuestVA                  entry = 0;
    GuestVA                  end   = 0;      // endereço após a última instrução
    std::vector<Instruction> insns;
    std::vector<OpHandler>   ops;            // handlers resolvidos na tradução
    std::uint64_t            cycles = 0;     // custo estimado do bloco
    std::uint64_t            exec_count = 0;
    void*                    native = nullptr; // reservado para codegen tier-2
};

class JitEngine {
public:
    static constexpr std::size_t kMaxBlockInsns = 64;

    explicit JitEngine(MemoryManager& mem) : mem_(mem) {}

    // Executa (traduzindo se necessário) o bloco em RIP. Devolve nº de instruções.
    std::uint64_t execute_block(CpuCore& core, bool trace);

    // Self-modifying code / unmap: descarta blocos que tocam [va, va+size).
    void invalidate(GuestVA va, std::uint64_t size);
    void flush();

    struct Stats {
        std::uint64_t blocks_translated = 0, cache_hits = 0, invalidations = 0, cached_blocks = 0;
    };
    Stats stats() const;

private:
    TranslatedBlock* lookup_or_translate(GuestVA rip);
    std::unique_ptr<TranslatedBlock> translate(GuestVA rip);

    MemoryManager& mem_;
    mutable std::mutex mu_;
    std::unordered_map<GuestVA, std::unique_ptr<TranslatedBlock>> cache_;
    Stats stats_{};
};

}  // namespace xone
