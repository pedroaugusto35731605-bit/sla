// =============================================================================
//  XOne-Emu — bios.hpp
//  Simulação do firmware UEFI do Xbox One.
//
//  Fases (como no UEFI PI):
//    SEC  → vetor de reset, estado inicial dos núcleos
//    PEI  → POST/teste de memória, construção do mapa de memória EFI
//    DXE  → "drivers": GPU (Vulkan), armazenamento NVMe
//    BDS  → seleção do dispositivo de boot e carga da imagem (título/homebrew)
//
//  O sistema operacional real do console (hypervisor + Host OS + Game OS) é
//  substituído por HLE: o BIOS carrega diretamente a imagem do título no
//  espaço virtual e aponta o núcleo 0 para o entry point.
// =============================================================================
#pragma once

#include "cpu.hpp"
#include "gpu.hpp"
#include "memory.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xone {

// EFI_MEMORY_DESCRIPTOR (UEFI spec 2.x, seção 7.2)
struct EfiMemoryDescriptor {
    std::uint32_t type;
    std::uint32_t pad;
    std::uint64_t physical_start;
    std::uint64_t virtual_start;
    std::uint64_t number_of_pages;
    std::uint64_t attribute;
};
static_assert(sizeof(EfiMemoryDescriptor) == 40);

enum EfiMemoryType : std::uint32_t {
    EfiReservedMemoryType = 0, EfiLoaderCode = 1, EfiLoaderData = 2,
    EfiBootServicesCode = 3, EfiBootServicesData = 4, EfiRuntimeServicesCode = 5,
    EfiRuntimeServicesData = 6, EfiConventionalMemory = 7, EfiACPIReclaimMemory = 9,
};

struct BootImage {
    std::string                name;
    std::vector<std::uint8_t>  bytes;        // código + dados, carregados em kBootEntryVA
    std::uint64_t              entry_offset = 0;
};

class Bios {
public:
    Bios(MemoryManager& mem, CpuCluster& cpu, GpuDevice& gpu);

    bool power_on_self_test();                  // PEI
    void build_memory_map();                    // PEI
    void initialize_devices(bool vk_validation);// DXE
    void boot(const BootImage& image);          // BDS

    // Imagens de boot
    static BootImage builtin_test_rom();        // homebrew de diagnóstico embutido
    static BootImage load_flat_binary(const std::filesystem::path& p);  // .xbin (homebrew cru)

    const std::vector<EfiMemoryDescriptor>& memory_map() const noexcept { return map_; }

private:
    MemoryManager& mem_;
    CpuCluster&    cpu_;
    GpuDevice&     gpu_;
    std::vector<EfiMemoryDescriptor> map_;
};

}  // namespace xone
