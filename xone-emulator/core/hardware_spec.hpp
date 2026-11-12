// =============================================================================
//  XOne-Emu — hardware_spec.hpp
//  Constantes de hardware que todo o núcleo do emulador deve respeitar.
//
//  NOTA DE FIDELIDADE: os valores pedidos na especificação do projeto
//  (2.3 GHz, 12 TFLOPS, SSD NVMe) misturam gerações. Para referência:
//    - Xbox One (2013):   Jaguar 8c @ 1.75 GHz, GCN 1.31 TFLOPS, 8 GB DDR3 + 32 MB ESRAM, HDD
//    - Xbox One X (2017): Jaguar 8c @ 2.30 GHz, GCN 6.0  TFLOPS, 12 GB GDDR5, HDD
//    - Series X (2020):   Zen 2  8c @ 3.8 GHz,  RDNA2 12.15 TFLOPS, 16 GB GDDR6, SSD NVMe
//  Mantemos os valores da especificação como "perfil alvo" do emulador e
//  documentamos os valores reais do Xbox One original em `kRetail`.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace xone::spec {

// ---------------------------------------------------------------- unidades
inline constexpr std::uint64_t KiB = 1024ull;
inline constexpr std::uint64_t MiB = 1024ull * KiB;
inline constexpr std::uint64_t GiB = 1024ull * MiB;

// ---------------------------------------------------------------- CPU
struct CpuSpec {
    std::string_view name          = "AMD Jaguar Custom (x86-64)";
    std::uint32_t    cores         = 8;
    std::uint32_t    threads       = 8;           // sem SMT no Jaguar
    std::uint64_t    clock_hz      = 2'300'000'000ull;
    std::uint32_t    modules       = 2;           // 2 módulos de 4 núcleos
    std::uint64_t    l1i_per_core  = 32 * KiB;
    std::uint64_t    l1d_per_core  = 32 * KiB;
    std::uint64_t    l2_per_module = 2 * MiB;
    std::uint32_t    cache_line    = 64;
};
inline constexpr CpuSpec kCpu{};

// ---------------------------------------------------------------- GPU
struct GpuSpec {
    std::string_view name            = "AMD Radeon Custom (GCN 3rd Gen)";
    std::uint32_t    compute_units   = 40;
    std::uint32_t    shaders_per_cu  = 64;        // 4 SIMD16 por CU
    std::uint64_t    clock_hz        = 1'172'000'000ull;
    double           tflops_fp32     = 12.0;      // perfil alvo da especificação
    std::uint32_t    wavefront_size  = 64;
    std::uint32_t    max_render_targets = 8;
};
inline constexpr GpuSpec kGpu{};

// ---------------------------------------------------------------- Memória (SoC unificado)
struct MemorySpec {
    std::string_view type            = "DDR3-2133 (unificada CPU/GPU)";
    std::uint64_t    system_ram      = 8 * GiB;
    std::uint64_t    esram           = 32 * MiB;  // scratchpad embarcado da GPU
    std::uint64_t    page_size       = 4 * KiB;   // página x86-64 padrão
    std::uint64_t    large_page_size = 2 * MiB;
    std::uint32_t    bus_width_bits  = 256;
    double           bandwidth_gbs   = 68.3;
    std::uint32_t    va_bits         = 48;        // endereçamento canônico x86-64
};
inline constexpr MemorySpec kMemory{};

// ---------------------------------------------------------------- Armazenamento
struct StorageSpec {
    std::string_view name               = "Custom NVMe SSD (simulado)";
    std::uint64_t    read_latency_ns    = 20'000;     // ~20 µs por comando
    std::uint64_t    write_latency_ns   = 30'000;
    std::uint64_t    seq_read_bytes_s   = 2'400 * MiB;
    std::uint32_t    sector_size        = 2048;       // setor lógico de disco óptico/XDVDFS
};
inline constexpr StorageSpec kStorage{};

// ---------------------------------------------------------------- Valores reais (referência)
struct RetailXboxOne {
    std::uint64_t cpu_clock_hz = 1'750'000'000ull;
    std::uint32_t gpu_cus      = 12;
    std::uint64_t gpu_clock_hz = 853'000'000ull;
    double        tflops_fp32  = 1.31;
};
inline constexpr RetailXboxOne kRetail{};

// ---------------------------------------------------------------- Mapa físico do emulador
//  Layout do espaço físico de 8 GB usado pelo BIOS e pelo gerenciador de memória.
namespace layout {
    inline constexpr std::uint64_t kFirmwareBase   = 0x0000'0000ull;   // tabelas UEFI / vetores
    inline constexpr std::uint64_t kFirmwareSize   = 1 * MiB;
    inline constexpr std::uint64_t kPageTableBase  = 0x0010'0000ull;   // pool de page tables
    inline constexpr std::uint64_t kPageTableSize  = 15 * MiB;
    inline constexpr std::uint64_t kKernelBase     = 0x0100'0000ull;   // imagem do kernel/boot
    inline constexpr std::uint64_t kKernelSize     = 16 * MiB;
    inline constexpr std::uint64_t kGpuRingBase    = 0x0200'0000ull;   // ring buffer de comandos da GPU
    inline constexpr std::uint64_t kGpuRingSize    = 4 * MiB;
    inline constexpr std::uint64_t kGeneralBase    = 0x0300'0000ull;   // RAM geral (títulos)
}

// ---------------------------------------------------------------- Mapa virtual do título
namespace vmap {
    inline constexpr std::uint64_t kBootEntryVA  = 0x0000'0000'0040'0000ull; // entry point do título
    inline constexpr std::uint64_t kStackTopVA   = 0x0000'0000'7FFF'0000ull;
    inline constexpr std::uint64_t kStackSize    = 1 * MiB;
    inline constexpr std::uint64_t kHeapBaseVA   = 0x0000'0001'0000'0000ull;
    inline constexpr std::uint64_t kGpuRingVA    = 0x0000'0000'6000'0000ull;
}

}  // namespace xone::spec
