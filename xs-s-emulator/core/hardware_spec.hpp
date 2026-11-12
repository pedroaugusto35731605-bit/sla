// =============================================================================
//  SeriesS-Emu — hardware_spec.hpp
//  Constantes de hardware do Xbox Series S que todo o núcleo deve respeitar.
//
//  NOTAS DE FIDELIDADE (valores pedidos × console real):
//   • GPU: 20 CUs × 64 lanes × 2 FLOP (FMA) × 1,565 GHz = 4,0 TFLOPS.
//     Com 1050 MHz, como pedido, o resultado seria 2,69 TFLOPS. Mantemos
//     kGpu.clock_hz = 1,565 GHz para que os 4 TFLOPS fechem; o valor pedido
//     fica em kGpu.requested_clock_hz para referência.
//   • RAM: o Series S tem 10 GB GDDR6 em dois pools: 8 GB a 224 GB/s e 2 GB a
//     56 GB/s. A Microsoft reserva ~2 GB ao SO e libera ~8 GB aos jogos. O
//     projeto pede a divisão 7 + 3 GB, que adotamos como limite rígido (é a
//     política mais conservadora); o pool lento de 2 GB fica dentro dos 3 GB do
//     sistema, como no console real.
//   • SSD: 2,4 GB/s bruto. A Microsoft divulga 4,8 GB/s "típico" com o bloco de
//     descompressão; o teto de 8 GB/s pedido é usado como limite de saída do
//     descompressor simulado.
//   • CPU: 3,6 GHz sem SMT / 3,4 GHz com SMT (16 threads). Como emulamos 16
//     threads, o clock efetivo é 3,4 GHz.
// =============================================================================
#pragma once

#include <cstdint>
#include <string_view>

namespace xss::spec {

inline constexpr std::uint64_t KiB = 1024ull;
inline constexpr std::uint64_t MiB = 1024ull * KiB;
inline constexpr std::uint64_t GiB = 1024ull * MiB;
inline constexpr std::uint64_t GB  = 1'000'000'000ull;   // decimal, para throughput

// ---------------------------------------------------------------- CPU
struct CpuSpec {
    std::string_view name       = "AMD Zen 2 Custom (x86-64)";
    std::uint32_t    cores      = 8;
    std::uint32_t    threads    = 16;                 // SMT2
    std::uint64_t    clock_hz   = 3'400'000'000ull;   // com SMT
    std::uint64_t    clock_no_smt_hz = 3'600'000'000ull;
    std::uint32_t    ccx        = 2;                  // 2 CCX de 4 núcleos
    std::uint64_t    l2_per_core = 512 * KiB;
    std::uint64_t    l3_per_ccx  = 4 * MiB;
    std::uint32_t    cache_line  = 64;
};
inline constexpr CpuSpec kCpu{};

// ---------------------------------------------------------------- GPU
struct GpuSpec {
    std::string_view name               = "AMD Radeon Custom (RDNA 2)";
    std::uint32_t    compute_units      = 20;
    std::uint32_t    lanes_per_cu       = 64;
    std::uint64_t    clock_hz           = 1'565'000'000ull;   // fecha os 4 TFLOPS
    std::uint64_t    requested_clock_hz = 1'050'000'000ull;   // valor da especificação do projeto
    double           tflops_fp32        = 4.0;
    std::uint32_t    wave_size          = 32;                  // RDNA: wave32 nativo (wave64 opcional)
    bool             hw_raytracing      = true;
    bool             mesh_shaders       = true;
    bool             sampler_feedback   = true;                // base do Sampler Feedback Streaming
    bool             vrs_tier2          = true;
};
inline constexpr GpuSpec kGpu{};
// FLOPs teóricos = CUs × lanes × 2 × clock
inline constexpr double kComputedTflops =
    static_cast<double>(kGpu.compute_units) * kGpu.lanes_per_cu * 2.0 * static_cast<double>(kGpu.clock_hz) / 1e12;

// ---------------------------------------------------------------- Memória unificada
struct MemorySpec {
    std::string_view type           = "GDDR6 unificada";
    std::uint64_t    total          = 10 * GiB;
    std::uint64_t    user_space     = 7 * GiB;    // limite rígido para o título
    std::uint64_t    system_reserved= 3 * GiB;    // SO / kernel / page tables
    std::uint64_t    slow_pool      = 2 * GiB;    // 56 GB/s (dentro da área do sistema)
    double           fast_bw_gbs    = 224.0;
    double           slow_bw_gbs    = 56.0;
    std::uint64_t    page_size      = 4 * KiB;
    std::uint64_t    large_page     = 2 * MiB;
    // Política de pressão (frações do orçamento de usuário)
    double           elevated_ratio = 0.80;       // streaming fica mais agressivo
    double           critical_ratio = 0.95;       // compressão forçada de dados frios
};
inline constexpr MemorySpec kMemory{};
static_assert(kMemory.user_space + kMemory.system_reserved == kMemory.total);

// ---------------------------------------------------------------- SSD / Velocity Architecture
struct SsdSpec {
    std::string_view name              = "Custom NVMe SSD (PCIe 4.0 x2)";
    std::uint64_t    capacity          = 512 * GB;
    std::uint64_t    raw_bytes_s       = 2'400'000'000ull;   // 2,4 GB/s
    std::uint64_t    decompressed_max_bytes_s = 8'000'000'000ull; // teto do descompressor
    std::uint64_t    command_latency_ns = 30'000;            // ~30 µs por comando NVMe
    std::uint32_t    queue_depth        = 64;
    std::uint32_t    queues             = 4;                  // filas de submissão paralelas
    std::uint32_t    block_size         = 64 * 1024;          // bloco de compressão (64 KiB)
};
inline constexpr SsdSpec kSsd{};

// Perfis de comparação (para o benchmark)
struct StorageProfile {
    std::string_view name;
    std::uint64_t    bytes_s;
    std::uint64_t    latency_ns;
    bool             hw_decompression;
};
inline constexpr StorageProfile kHdd     {"HDD 5400 RPM (Xbox One)", 100'000'000ull, 10'000'000ull, false};
inline constexpr StorageProfile kSataSsd {"SSD SATA III",            550'000'000ull,     80'000ull, false};
inline constexpr StorageProfile kSeriesS {"Series S NVMe + HW decomp", kSsd.raw_bytes_s, kSsd.command_latency_ns, true};

// ---------------------------------------------------------------- Vídeo
struct DisplaySpec {
    std::uint32_t render_width  = 2560;   // 1440p nativo
    std::uint32_t render_height = 1440;
    std::uint32_t output_width  = 3840;   // upscaling para 4K
    std::uint32_t output_height = 2160;
    std::uint32_t target_fps    = 60;
};
inline constexpr DisplaySpec kDisplay{};

// ---------------------------------------------------------------- Mapa físico (10 GB)
namespace layout {
    // [0, 2 GB)  pool lento  — sistema
    // [2, 3 GB)  pool rápido — sistema (kernel, page tables, filas de I/O)
    // [3, 10 GB) pool rápido — espaço do título (7 GB)
    inline constexpr std::uint64_t kSystemBase = 0;
    inline constexpr std::uint64_t kSystemSize = kMemory.system_reserved;
    inline constexpr std::uint64_t kUserBase   = kSystemBase + kSystemSize;
    inline constexpr std::uint64_t kUserSize   = kMemory.user_space;
}

// ---------------------------------------------------------------- Mapa virtual do título
namespace vmap {
    inline constexpr std::uint64_t kImageVA    = 0x0000'0000'0040'0000ull;
    inline constexpr std::uint64_t kStackTopVA = 0x0000'0000'7FFF'0000ull;
    inline constexpr std::uint64_t kStackSize  = 1 * MiB;
    inline constexpr std::uint64_t kHeapBaseVA = 0x0000'0010'0000'0000ull;   // 64 GB, alinhado a 2 MB
}

}  // namespace xss::spec
