// =============================================================================
//  SeriesS-Emu — codec.cpp  (XLZ: LZ77 com sequências no estilo LZ4)
//
//  Sequência: token (nibble alto = literais, nibble baixo = match-4; 15 = estende
//  com bytes 255…) | literais | offset u16 | extensão do match.
//  Regras de fim de bloco: os últimos 5 bytes são sempre literais e nenhum match
//  começa nos últimos 12 bytes — o último item do bloco é só literais.
// =============================================================================
#include "codec.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

namespace xss::codec {

namespace {

constexpr unsigned kHashBits = 14;
constexpr std::size_t kMinMatch = 4;
constexpr std::size_t kLastLiterals = 5;
constexpr std::size_t kMatchSafeEnd = 12;
constexpr std::size_t kMaxOffset = 65535;

inline std::uint32_t load32(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
inline std::uint32_t hash32(std::uint32_t v) { return (v * 2654435761u) >> (32 - kHashBits); }

template <typename T> void put(std::vector<std::uint8_t>& v, std::size_t at, T x) {
    std::memcpy(v.data() + at, &x, sizeof(T));
}
template <typename T> T get(const std::uint8_t* p) { T x; std::memcpy(&x, p, sizeof(T)); return x; }

unsigned resolve_threads(unsigned t, std::size_t blocks) {
    if (t == 0) t = std::max(1u, std::thread::hardware_concurrency());
    return static_cast<unsigned>(std::min<std::size_t>(t, std::max<std::size_t>(1, blocks)));
}

template <typename F> void parallel_for(std::size_t n, unsigned threads, F&& fn) {
    if (threads <= 1 || n <= 1) { for (std::size_t i = 0; i < n; ++i) fn(i); return; }
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&] { for (std::size_t i; (i = next.fetch_add(1)) < n;) fn(i); });
    for (auto& th : pool) th.join();
}

}  // namespace

// ----------------------------------------------------------------------------
std::size_t compress_block(const std::uint8_t* src, std::size_t n, std::uint8_t* dst, std::size_t cap) {
    std::uint8_t* op = dst;
    std::uint8_t* const oend = dst + cap;
    std::size_t anchor = 0;

    auto emit = [&](std::size_t lit_len, const std::uint8_t* lit, std::size_t offset, std::size_t mlen, bool last) -> bool {
        // pior caso: token + extensões + literais + offset + extensões
        const std::size_t need = 1 + lit_len / 255 + 1 + lit_len + 2 + (mlen / 255) + 1;
        if (static_cast<std::size_t>(oend - op) < need) return false;
        std::uint8_t* token = op++;
        const std::size_t ml = last ? 0 : mlen - kMinMatch;
        *token = static_cast<std::uint8_t>(((lit_len >= 15 ? 15 : lit_len) << 4) | (last ? 0 : (ml >= 15 ? 15 : ml)));
        if (lit_len >= 15) {
            std::size_t r = lit_len - 15;
            for (; r >= 255; r -= 255) *op++ = 255;
            *op++ = static_cast<std::uint8_t>(r);
        }
        std::memcpy(op, lit, lit_len);
        op += lit_len;
        if (last) return true;
        *op++ = static_cast<std::uint8_t>(offset & 0xFF);
        *op++ = static_cast<std::uint8_t>(offset >> 8);
        if (ml >= 15) {
            std::size_t r = ml - 15;
            for (; r >= 255; r -= 255) *op++ = 255;
            *op++ = static_cast<std::uint8_t>(r);
        }
        return true;
    };

    if (n > kMatchSafeEnd) {
        std::uint32_t table[1u << kHashBits];
        std::memset(table, 0, sizeof(table));
        const std::size_t limit = n - kMatchSafeEnd;
        std::size_t i = 0;
        while (i < limit) {
            const std::uint32_t seq = load32(src + i);
            const std::uint32_t h = hash32(seq);
            const std::size_t cand = table[h];
            table[h] = static_cast<std::uint32_t>(i + 1);   // +1: 0 significa vazio
            if (cand && i - (cand - 1) <= kMaxOffset && load32(src + cand - 1) == seq) {
                const std::size_t c = cand - 1;
                std::size_t m = kMinMatch;
                const std::size_t maxm = n - kLastLiterals - i;
                while (m < maxm && src[c + m] == src[i + m]) ++m;
                if (!emit(i - anchor, src + anchor, i - c, m, false)) return 0;
                i += m;
                anchor = i;
                if (i - 2 < limit) table[hash32(load32(src + i - 2))] = static_cast<std::uint32_t>(i - 1);
            } else {
                i += 1 + ((i - anchor) >> 6);   // acelera em dados incompressíveis
            }
        }
    }
    if (!emit(n - anchor, src + anchor, 0, 0, true)) return 0;
    return static_cast<std::size_t>(op - dst);
}

bool decompress_block(const std::uint8_t* src, std::size_t n, std::uint8_t* dst, std::size_t out_n) {
    const std::uint8_t* ip = src;
    const std::uint8_t* const iend = src + n;
    std::uint8_t* op = dst;
    std::uint8_t* const oend = dst + out_n;

    auto read_len = [&](std::size_t base) -> std::size_t {
        std::size_t len = base;
        if (base == 15) {
            std::uint8_t b;
            do {
                if (ip >= iend) return SIZE_MAX;
                b = *ip++;
                len += b;
            } while (b == 255);
        }
        return len;
    };

    while (ip < iend) {
        const std::uint8_t token = *ip++;
        const std::size_t lit = read_len(token >> 4);
        if (lit == SIZE_MAX || lit > static_cast<std::size_t>(iend - ip) || lit > static_cast<std::size_t>(oend - op))
            return false;
        std::memcpy(op, ip, lit);
        ip += lit; op += lit;
        if (ip == iend) break;                         // última sequência: só literais
        if (iend - ip < 2) return false;
        const std::size_t offset = static_cast<std::size_t>(ip[0]) | (static_cast<std::size_t>(ip[1]) << 8);
        ip += 2;
        const std::size_t mlen = read_len(token & 15);
        if (mlen == SIZE_MAX) return false;
        const std::size_t len = mlen + kMinMatch;
        if (offset == 0 || offset > static_cast<std::size_t>(op - dst) || len > static_cast<std::size_t>(oend - op))
            return false;
        const std::uint8_t* match = op - offset;
        if (offset >= len) {
            std::memcpy(op, match, len);
        } else if (offset >= 8) {
            std::size_t k = 0;
            for (; k + 8 <= len; k += 8) std::memcpy(op + k, match + k, 8);  // blocos não sobrepostos
            for (; k < len; ++k) op[k] = match[k];
        } else {
            for (std::size_t k = 0; k < len; ++k) op[k] = match[k];   // run curto (ex.: RLE)
        }
        op += len;
    }
    return op == oend;
}

// ----------------------------------------------------------------------------
std::vector<std::uint8_t> compress(std::span<const std::uint8_t> src, std::uint32_t block_size, unsigned threads) {
    if (block_size == 0) block_size = kDefaultBlock;
    const std::size_t blocks = (src.size() + block_size - 1) / block_size;
    const std::size_t header = 20 + 4 * blocks;

    std::vector<std::vector<std::uint8_t>> out(blocks);
    std::vector<std::uint32_t> lens(blocks);
    parallel_for(blocks, resolve_threads(threads, blocks), [&](std::size_t b) {
        const std::size_t off = b * block_size;
        const std::size_t n = std::min<std::size_t>(block_size, src.size() - off);
        out[b].resize(n);   // se não couber em n bytes, armazena cru
        const std::size_t c = compress_block(src.data() + off, n, out[b].data(), n);
        if (c == 0 || c >= n) {
            std::memcpy(out[b].data(), src.data() + off, n);
            lens[b] = static_cast<std::uint32_t>(n) | kStoredFlag;
        } else {
            out[b].resize(c);
            lens[b] = static_cast<std::uint32_t>(c);
        }
    });

    std::size_t total = header;
    for (auto& o : out) total += o.size();
    std::vector<std::uint8_t> s(total);
    put<std::uint32_t>(s, 0, kMagic);
    put<std::uint64_t>(s, 4, src.size());
    put<std::uint32_t>(s, 12, block_size);
    put<std::uint32_t>(s, 16, static_cast<std::uint32_t>(blocks));
    std::size_t pos = header;
    for (std::size_t b = 0; b < blocks; ++b) {
        put<std::uint32_t>(s, 20 + 4 * b, lens[b]);
        std::memcpy(s.data() + pos, out[b].data(), out[b].size());
        pos += out[b].size();
    }
    return s;
}

bool is_xlz(std::span<const std::uint8_t> d) {
    return d.size() >= 20 && get<std::uint32_t>(d.data()) == kMagic;
}

bool parse_header(std::span<const std::uint8_t> d, StreamInfo& info) {
    if (!is_xlz(d)) return false;
    info.raw_size    = get<std::uint64_t>(d.data() + 4);
    info.block_size  = get<std::uint32_t>(d.data() + 12);
    info.block_count = get<std::uint32_t>(d.data() + 16);
    info.header_bytes = 20ull + 4ull * info.block_count;
    if (info.block_size == 0 || info.header_bytes > d.size()) return false;
    if ((info.raw_size + info.block_size - 1) / info.block_size != info.block_count) return false;
    info.compressed_size = d.size();
    return true;
}

bool decompress(std::span<const std::uint8_t> src, std::span<std::uint8_t> dst, unsigned threads) {
    StreamInfo h;
    if (!parse_header(src, h) || dst.size() != h.raw_size) return false;

    // Offsets de cada bloco (prefix sum da tabela)
    std::vector<std::uint64_t> offs(h.block_count + 1);
    offs[0] = h.header_bytes;
    for (std::uint32_t b = 0; b < h.block_count; ++b) {
        offs[b + 1] = offs[b] + (get<std::uint32_t>(src.data() + 20 + 4 * b) & ~kStoredFlag);
        if (offs[b + 1] > src.size()) return false;
    }

    std::atomic<bool> ok{true};
    parallel_for(h.block_count, resolve_threads(threads, h.block_count), [&](std::size_t b) {
        const std::uint32_t len = get<std::uint32_t>(src.data() + 20 + 4 * b);
        const std::uint64_t out_off = b * static_cast<std::uint64_t>(h.block_size);
        const std::size_t out_n = static_cast<std::size_t>(std::min<std::uint64_t>(h.block_size, h.raw_size - out_off));
        const std::uint8_t* in = src.data() + offs[b];
        const std::size_t in_n = len & ~kStoredFlag;
        if (len & kStoredFlag) {
            if (in_n != out_n) { ok = false; return; }
            std::memcpy(dst.data() + out_off, in, out_n);
        } else if (!decompress_block(in, in_n, dst.data() + out_off, out_n)) {
            ok = false;
        }
    });
    return ok;
}

}  // namespace xss::codec
