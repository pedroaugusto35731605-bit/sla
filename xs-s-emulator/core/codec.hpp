// =============================================================================
//  SeriesS-Emu — codec.hpp
//  "XLZ": codec LZ77 rápido em blocos independentes, usado para
//    • assets comprimidos no disco (descompressão "em hardware" do ssd_io);
//    • compressão de memória fria sob pressão (memory_manager).
//
//  O Series S usa zlib + BCPack em hardware (e títulos usam Oodle Kraken em
//  software). Esses formatos não são reimplementados aqui: o XLZ é um codec
//  próprio da família LZ4 que reproduz as características que importam para a
//  simulação — blocos de 64 KiB independentes (paralelizáveis como no bloco de
//  descompressão), decodificação muito mais rápida que a codificação, e taxa
//  de compressão típica de 2–4× em dados de jogo.
//
//  Formato do stream:
//    "XLZ1" | u64 raw_size | u32 block_size | u32 block_count
//    | u32 block_len[block_count]   (bit 31 = bloco armazenado sem compressão)
//    | blocos...
// =============================================================================
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace xss::codec {

inline constexpr std::uint32_t kMagic = 0x315A4C58;          // "XLZ1" little-endian
inline constexpr std::uint32_t kDefaultBlock = 64 * 1024;
inline constexpr std::uint32_t kStoredFlag = 0x8000'0000u;

struct StreamInfo {
    std::uint64_t raw_size = 0;
    std::uint32_t block_size = 0;
    std::uint32_t block_count = 0;
    std::uint64_t header_bytes = 0;
    std::uint64_t compressed_size = 0;
};

// Comprime `src`. threads = 0 → usa todos os núcleos do host.
std::vector<std::uint8_t> compress(std::span<const std::uint8_t> src,
                                   std::uint32_t block_size = kDefaultBlock, unsigned threads = 0);

bool is_xlz(std::span<const std::uint8_t> data);
bool parse_header(std::span<const std::uint8_t> data, StreamInfo& out);

// Descomprime para `dst` (que deve ter raw_size bytes). Valida limites: dados
// corrompidos retornam false em vez de escrever fora do buffer.
bool decompress(std::span<const std::uint8_t> src, std::span<std::uint8_t> dst, unsigned threads = 1);

// Primitivas de bloco (expostas para testes)
std::size_t compress_block(const std::uint8_t* src, std::size_t n, std::uint8_t* dst, std::size_t cap);
bool        decompress_block(const std::uint8_t* src, std::size_t n, std::uint8_t* dst, std::size_t out_n);

}  // namespace xss::codec
