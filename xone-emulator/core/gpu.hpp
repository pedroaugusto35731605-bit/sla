// =============================================================================
//  XOne-Emu — gpu.hpp
//  GPU AMD GCN (Xbox One) → backend Vulkan do host.
//
//  Fluxo:
//    título escreve pacotes PM4 (formato do Command Processor da AMD) num ring
//    buffer na RAM unificada → SYSCALL XGpuSubmitCommandBuffer →
//    GpuDevice::submit_command_buffer() decodifica os pacotes → DrawCall →
//    backend Vulkan grava/submete um VkCommandBuffer por frame.
// =============================================================================
#pragma once

#include "hardware_spec.hpp"
#include "memory.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xone {

// Opcodes PM4 tipo 3 (AMD GCN Command Processor)
namespace pm4 {
    inline constexpr std::uint8_t NOP              = 0x10;
    inline constexpr std::uint8_t CLEAR_STATE      = 0x12;
    inline constexpr std::uint8_t DRAW_INDEX_2     = 0x27;
    inline constexpr std::uint8_t DRAW_INDEX_AUTO  = 0x2D;
    inline constexpr std::uint8_t NUM_INSTANCES    = 0x2F;
    inline constexpr std::uint8_t EVENT_WRITE_EOP  = 0x47;
    inline constexpr std::uint8_t SET_CONTEXT_REG  = 0x69;
    inline constexpr std::uint8_t SET_SH_REG       = 0x76;
    inline constexpr std::uint8_t SET_UCONFIG_REG  = 0x79;

    inline constexpr std::uint32_t kContextRegBase = 0xA000;
    inline constexpr std::uint32_t kShRegBase      = 0x2C00;
    inline constexpr std::uint32_t kUconfigRegBase = 0xC000;
    inline constexpr std::uint32_t VGT_PRIMITIVE_TYPE = 0xC242;  // uconfig

    constexpr std::uint32_t type3(std::uint8_t opcode, std::uint32_t body_dwords) {
        return (3u << 30) | (((body_dwords - 1) & 0x3FFF) << 16) | (static_cast<std::uint32_t>(opcode) << 8);
    }
}

// Registrador de contexto do emulador para cor de clear do render target 0
// (fora da faixa usada pelo hardware real; usado pelo homebrew de teste).
inline constexpr std::uint32_t kXoneClearColorReg = 0xA3F0;

enum class PrimitiveType : std::uint32_t {
    PointList = 1, LineList = 2, LineStrip = 3, TriList = 4, TriFan = 5, TriStrip = 6, RectList = 0x11
};

struct DrawCall {
    PrimitiveType primitive      = PrimitiveType::TriList;
    std::uint32_t vertex_count   = 0;
    std::uint32_t instance_count = 1;
    bool          indexed        = false;
    GuestVA       index_buffer   = 0;
};

class GpuDevice {
public:
    explicit GpuDevice(MemoryManager& mem);
    ~GpuDevice();

    // Cria instância/dispositivo Vulkan. Se não houver driver, cai para o
    // backend "headless" (só contabiliza comandos) e devolve false.
    bool initialize(bool enable_validation = false);

    const std::string& backend_name() const noexcept { return backend_; }
    const std::string& device_name()  const noexcept { return device_; }

    void submit_command_buffer(GuestVA va, std::uint32_t dwords);
    void submit_draw(const DrawCall& dc);
    void end_frame();

    struct Stats {
        std::uint64_t packets = 0, draws = 0, vertices = 0, frames = 0, unknown_packets = 0;
    };
    Stats stats() const noexcept { return stats_; }

private:
    void execute_packet(std::uint8_t opcode, const std::uint32_t* body, std::uint32_t count);

    MemoryManager& mem_;
    std::string    backend_ = "headless";
    std::string    device_  = "nenhum";
    Stats          stats_{};

    // Estado do Command Processor
    std::vector<std::uint32_t> context_regs_;   // 0xA000..0xA3FF
    std::uint32_t              num_instances_ = 1;
    PrimitiveType              primitive_     = PrimitiveType::TriList;
    std::vector<DrawCall>      frame_draws_;

    struct VulkanContext;                        // definido em gpu.cpp (pimpl)
    std::unique_ptr<VulkanContext> vk_;
};

}  // namespace xone
