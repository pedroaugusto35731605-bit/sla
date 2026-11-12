// =============================================================================
//  SeriesS-Emu — bios_uefi.hpp
//  Firmware UEFI simulado + kernel do Xbox OS em HLE.
//
//  UEFI (fases do PI):  SEC → PEI (POST, mapa de memória 7+3) → DXE (GPU, SSD)
//                       → BDS (carga da imagem do título, ExitBootServices)
//
//  Kernel (XboxOsKernel): o console real roda um hypervisor com o "Game OS" e o
//  "Shared OS". Aqui o título chama serviços via SYSCALL (número em EAX,
//  convenção própria do SeriesS-Emu) e o emulador os implementa em C++. Toda
//  memória pedida pelo título sai do User Space e respeita o limite de 7 GB.
// =============================================================================
#pragma once

#include "cpu.hpp"
#include "gpu.hpp"
#include "memory_manager.hpp"
#include "ssd_io.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace xss {

namespace svc {
    inline constexpr std::uint32_t XDbgPrint                 = 0x0001; // rdi=str VA, rsi=len
    inline constexpr std::uint32_t XDbgPrintHex              = 0x0002; // rdi=valor
    inline constexpr std::uint32_t NtAllocateVirtualMemory   = 0x0010; // rdi=tamanho → rax=VA ou 0
    inline constexpr std::uint32_t NtFreeVirtualMemory       = 0x0011; // rdi=VA
    inline constexpr std::uint32_t XMemQueryBudget           = 0x0012; // → rax=bytes livres no User Space
    inline constexpr std::uint32_t KeQueryPerformanceCounter = 0x0020; // → rax=ticks de 10 MHz
    inline constexpr std::uint32_t XGpuSubmitCommandBuffer   = 0x0030; // rdi=VA, rsi=dwords
    inline constexpr std::uint32_t NtTerminateProcess        = 0x00FF; // rdi=código
}
namespace status {
    inline constexpr std::uint64_t Success        = 0x00000000;
    inline constexpr std::uint64_t NotImplemented = 0xC0000002;
    inline constexpr std::uint64_t NoMemory       = 0xC0000017;
}

class XboxOsKernel final : public SystemCalls {
public:
    XboxOsKernel(MemoryManager& mem, GpuDevice& gpu);
    void         dispatch(CpuCore& core) override;
    bool         terminated() const noexcept override { return terminated_.load(); }
    std::int64_t exit_code() const noexcept override { return exit_code_.load(); }

    std::uint64_t failed_allocations() const noexcept { return failed_allocs_.load(); }

private:
    MemoryManager& mem_;
    GpuDevice&     gpu_;
    std::mutex     mu_;
    GuestVA        heap_next_;
    std::map<GuestVA, AllocationId> heap_;      // VA → alocação do título
    std::atomic<bool>          terminated_{false};
    std::atomic<std::int64_t>  exit_code_{0};
    std::atomic<std::uint64_t> failed_allocs_{0};
};

struct BootImage {
    std::string               name;
    std::vector<std::uint8_t> bytes;        // código + dados, carregados em kImageVA
    std::uint64_t             entry_offset = 0;
};

class UefiFirmware {
public:
    UefiFirmware(MemoryManager& mem, CpuCluster& cpu, GpuDevice& gpu, SsdDevice& ssd);

    bool power_on_self_test();                     // SEC + PEI
    void initialize_devices(bool vk_validation);   // DXE
    void boot(const BootImage& image);             // BDS

    static BootImage diagnostic_rom();             // homebrew de diagnóstico embutido
    static BootImage load_flat_binary(const std::filesystem::path& p);

private:
    MemoryManager& mem_;
    CpuCluster&    cpu_;
    GpuDevice&     gpu_;
    SsdDevice&     ssd_;
    AllocationId   image_alloc_ = 0, stack_alloc_ = 0;
};

}  // namespace xss
