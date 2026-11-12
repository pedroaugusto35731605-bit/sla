// =============================================================================
//  SeriesS-Emu — texture_streamer.hpp
//  Streaming de texturas sob demanda via SSD simulado (no espírito do
//  Sampler Feedback Streaming do Series X|S): só os níveis de mip que a cena
//  pede ficam residentes; o resto é lido do SSD quando necessário.
//
//  Integração com o orçamento de 7 GB:
//    • Pressão Elevated → mip bias +1 (texturas com metade da resolução)
//    • Pressão Critical → mip bias +2
//    • Reclaim handler: o MemoryManager pede memória → o streamer despeja os
//      mips menos usados recentemente que não foram pedidos neste frame.
//
//  Formato .xtex (little-endian):
//    "XTEX" | u16 versão=1 | u16 formato (1 = RGBA8) | u32 largura | u32 altura | u32 nº de mips
//    | mip[n] { u64 offset, u64 disk_size, u64 raw_size, u32 compressão (0 = nenhuma, 1 = XLZ), u32 reservado }
//    | payloads (cada mip comprimido de forma independente)
// =============================================================================
#pragma once

#include "../core/memory_manager.hpp"
#include "../core/ssd_io.hpp"
#include "iso_parser.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xss::fs {

inline constexpr std::uint32_t kXtexMagic = 0x58455458;   // "XTEX"
inline constexpr std::size_t   kXtexHeader = 24;
inline constexpr std::size_t   kXtexMipEntry = 32;

struct MipLevel {
    std::uint64_t offset = 0;        // absoluto na imagem
    std::uint64_t disk_size = 0;
    std::uint64_t raw_size = 0;
    bool          compressed = false;
    std::uint32_t width = 0, height = 0;
};

struct StreamedTexture {
    std::string                name;
    std::uint32_t              width = 0, height = 0;
    std::vector<MipLevel>      mips;
    std::vector<AllocationId>  resident;     // 0 = não residente
    std::vector<std::uint64_t> last_used;    // frame do último uso
};

class TextureStreamer {
public:
    struct Config {
        std::uint64_t pool_budget = 2 * spec::GiB;   // teto do pool de texturas do título
        std::uint32_t max_loads_per_frame = 128;
    };
    struct FrameReport {
        std::uint64_t frame = 0;
        std::uint32_t requested = 0, hits = 0, loads = 0, deferred = 0, evictions = 0;
        std::uint64_t disk_bytes = 0, raw_bytes = 0, pool_used = 0;
        double        io_ms = 0;
        std::uint32_t mip_bias = 0;
        PressureLevel pressure = PressureLevel::Normal;
    };
    struct Stats {
        std::uint64_t loads = 0, evictions = 0, deferred = 0, disk_bytes = 0, raw_bytes = 0, frames = 0;
        double        io_ms = 0;
    };

    TextureStreamer(MemoryManager& mem, SsdDevice& ssd, Config cfg);
    TextureStreamer(MemoryManager& mem, SsdDevice& ssd) : TextureStreamer(mem, ssd, Config{}) {}
    ~TextureStreamer();
    TextureStreamer(const TextureStreamer&) = delete;
    TextureStreamer& operator=(const TextureStreamer&) = delete;

    // Registra todos os .xtex da imagem. Devolve quantas texturas entraram.
    std::size_t register_image(const std::filesystem::path& image, const GameMetadata& meta);

    // Pede a textura `tex` no mip `mip` (0 = resolução máxima) para o frame atual.
    void request(std::size_t tex, std::uint32_t mip);
    // Processa os pedidos do frame: despeja, aloca, lê do SSD (lote DirectStorage).
    FrameReport update();

    std::size_t                   texture_count() const noexcept { return textures_.size(); }
    const StreamedTexture&        texture(std::size_t i) const { return textures_.at(i); }
    std::uint32_t                 mip_bias() const noexcept { return bias_; }
    std::uint64_t                 pool_used() const noexcept { return pool_used_; }
    std::uint64_t                 full_set_bytes() const;     // tudo em mip 0
    Stats                         stats() const noexcept { return stats_; }

private:
    std::uint64_t evict(std::uint64_t needed, const char* reason);

    MemoryManager& mem_;
    SsdDevice&     ssd_;
    Config         cfg_;
    int            file_ = -1;
    std::vector<StreamedTexture> textures_;
    struct Pending { std::size_t tex; std::uint32_t mip; };
    std::vector<Pending> requests_;
    std::uint64_t  frame_ = 1;
    std::uint64_t  pool_used_ = 0;
    std::uint32_t  bias_ = 0;
    std::uint32_t  frame_evictions_ = 0;
    int            reclaim_handle_ = 0, listener_handle_ = 0;
    Stats          stats_{};
};

}  // namespace xss::fs
