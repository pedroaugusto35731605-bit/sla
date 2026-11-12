// =============================================================================
//  XOne-Emu — gpu.cpp
//  Command Processor PM4 + inicialização do contexto Vulkan.
// =============================================================================
#include "gpu.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#if defined(XONE_HAS_VULKAN)
#  include <vulkan/vulkan.h>
#endif

namespace xone {

// ============================================================================
//  Contexto Vulkan
// ============================================================================
struct GpuDevice::VulkanContext {
#if defined(XONE_HAS_VULKAN)
    VkInstance       instance        = VK_NULL_HANDLE;
    VkPhysicalDevice physical        = VK_NULL_HANDLE;
    VkDevice         device          = VK_NULL_HANDLE;
    VkQueue          queue           = VK_NULL_HANDLE;
    std::uint32_t    queue_family    = 0;
    VkCommandPool    cmd_pool        = VK_NULL_HANDLE;
    VkCommandBuffer  cmd             = VK_NULL_HANDLE;
    VkFence          fence           = VK_NULL_HANDLE;
    // Render target offscreen 1920x1080 (o front buffer do Xbox One a 1080p)
    VkImage          color_image     = VK_NULL_HANDLE;
    VkDeviceMemory   color_memory    = VK_NULL_HANDLE;
    bool             image_initialized = false;

    ~VulkanContext() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (color_image)  vkDestroyImage(device, color_image, nullptr);
            if (color_memory) vkFreeMemory(device, color_memory, nullptr);
            if (fence)        vkDestroyFence(device, fence, nullptr);
            if (cmd_pool)     vkDestroyCommandPool(device, cmd_pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
#endif
};

#if defined(XONE_HAS_VULKAN)
namespace {
const char* vk_result_str(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER (nenhum driver/ICD)";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        default: return "VkResult desconhecido";
    }
}

#define VK_CHECK(expr)                                                                 \
    do {                                                                               \
        VkResult _r = (expr);                                                          \
        if (_r != VK_SUCCESS)                                                          \
            throw std::runtime_error(std::string(#expr " -> ") + vk_result_str(_r));   \
    } while (0)

int device_score(VkPhysicalDevice pd) {
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(pd, &p);
    switch (p.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 1;
        default:                                     return 0;
    }
}
}  // namespace
#endif

// ============================================================================
GpuDevice::GpuDevice(MemoryManager& mem) : mem_(mem), context_regs_(0x400, 0) {}
GpuDevice::~GpuDevice() = default;

bool GpuDevice::initialize(bool enable_validation) {
    std::printf("[XONE-GPU ] %s | %u CUs | perfil %.2f TFLOPS (Xbox One real: %.2f)\n",
                std::string(spec::kGpu.name).c_str(), spec::kGpu.compute_units,
                spec::kGpu.tflops_fp32, spec::kRetail.tflops_fp32);
#if defined(XONE_HAS_VULKAN)
    try {
        auto ctx = std::make_unique<VulkanContext>();

        // ---- 1. Instância
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName   = "XOne-Emu";
        app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app.pEngineName        = "XOne-Core";
        app.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
        app.apiVersion         = VK_API_VERSION_1_2;

        const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        if (enable_validation) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers; }
        VK_CHECK(vkCreateInstance(&ici, nullptr, &ctx->instance));

        // ---- 2. Dispositivo físico (prefere GPU discreta)
        std::uint32_t count = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(ctx->instance, &count, nullptr));
        if (count == 0) throw std::runtime_error("nenhum dispositivo Vulkan encontrado");
        std::vector<VkPhysicalDevice> pds(count);
        VK_CHECK(vkEnumeratePhysicalDevices(ctx->instance, &count, pds.data()));
        int best = -1;
        for (auto pd : pds)
            if (int s = device_score(pd); s > best) { best = s; ctx->physical = pd; }

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(ctx->physical, &props);
        device_ = props.deviceName;

        // ---- 3. Família de filas gráficas (GCN: fila "GFX" = graphics + compute)
        std::uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qf_count);
        vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical, &qf_count, qfs.data());
        bool found = false;
        for (std::uint32_t i = 0; i < qf_count; ++i) {
            if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                ctx->queue_family = i; found = true; break;
            }
        }
        if (!found) throw std::runtime_error("nenhuma fila graphics+compute");

        // ---- 4. Dispositivo lógico + fila
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = ctx->queue_family;
        qci.queueCount       = 1;
        qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos    = &qci;
        VK_CHECK(vkCreateDevice(ctx->physical, &dci, nullptr, &ctx->device));
        vkGetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);

        // ---- 5. Command pool / buffer / fence
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = ctx->queue_family;
        VK_CHECK(vkCreateCommandPool(ctx->device, &pci, nullptr, &ctx->cmd_pool));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = ctx->cmd_pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx->device, &cai, &ctx->cmd));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(ctx->device, &fci, nullptr, &ctx->fence));

        // ---- 6. Render target offscreen 1920x1080 RGBA8
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType   = VK_IMAGE_TYPE_2D;
        ii.format      = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent      = {1920, 1080, 1};
        ii.mipLevels   = 1;
        ii.arrayLayers = 1;
        ii.samples     = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
        ii.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(ctx->device, &ii, nullptr, &ctx->color_image));
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(ctx->device, ctx->color_image, &req);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(ctx->physical, &mp);
        std::uint32_t type_index = UINT32_MAX;
        for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type_index = i; break;
            }
        }
        if (type_index == UINT32_MAX) throw std::runtime_error("sem memória DEVICE_LOCAL compatível");
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = type_index;
        VK_CHECK(vkAllocateMemory(ctx->device, &mai, nullptr, &ctx->color_memory));
        VK_CHECK(vkBindImageMemory(ctx->device, ctx->color_image, ctx->color_memory, 0));

        vk_ = std::move(ctx);
        backend_ = "Vulkan 1.2";
        std::printf("[XONE-GPU ] Vulkan inicializado: %s (driver API %u.%u.%u), fila %u\n",
                    device_.c_str(), VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                    VK_VERSION_PATCH(props.apiVersion), vk_->queue_family);
        return true;
    } catch (const std::exception& e) {
        std::printf("[XONE-GPU ] Vulkan indisponível (%s) — usando backend headless\n", e.what());
        vk_.reset();
        return false;
    }
#else
    (void)enable_validation;
    std::printf("[XONE-GPU ] Compilado sem Vulkan — usando backend headless\n");
    return false;
#endif
}

// ============================================================================
//  Command Processor (PM4)
// ============================================================================
void GpuDevice::submit_command_buffer(GuestVA va, std::uint32_t dwords) {
    std::vector<std::uint32_t> buf(dwords);
    mem_.read_virtual(va, buf.data(), static_cast<std::uint64_t>(dwords) * 4);

    std::uint32_t i = 0;
    while (i < dwords) {
        const std::uint32_t hdr  = buf[i];
        const std::uint32_t type = hdr >> 30;
        if (type == 2) { ++i; continue; }  // filler de 1 dword
        if (type != 3) {
            std::printf("[XONE-GPU ] pacote PM4 tipo %u não suportado em +%u — abortando IB\n", type, i);
            ++stats_.unknown_packets;
            return;
        }
        const std::uint32_t count  = ((hdr >> 16) & 0x3FFF) + 1;
        const std::uint8_t  opcode = static_cast<std::uint8_t>((hdr >> 8) & 0xFF);
        if (i + 1 + count > dwords) {
            std::printf("[XONE-GPU ] pacote PM4 truncado em +%u\n", i);
            return;
        }
        execute_packet(opcode, &buf[i + 1], count);
        ++stats_.packets;
        i += 1 + count;
    }
}

void GpuDevice::execute_packet(std::uint8_t opcode, const std::uint32_t* body, std::uint32_t count) {
    switch (opcode) {
        case pm4::NOP: break;
        case pm4::CLEAR_STATE:
            std::fill(context_regs_.begin(), context_regs_.end(), 0u);
            break;
        case pm4::SET_CONTEXT_REG: {
            const std::uint32_t first = body[0];  // offset relativo a 0xA000
            for (std::uint32_t k = 1; k < count && first + k - 1 < context_regs_.size(); ++k)
                context_regs_[first + k - 1] = body[k];
            break;
        }
        case pm4::SET_UCONFIG_REG:
            if (body[0] + pm4::kUconfigRegBase == pm4::VGT_PRIMITIVE_TYPE && count >= 2)
                primitive_ = static_cast<PrimitiveType>(body[1]);
            break;
        case pm4::SET_SH_REG:
            break;  // registradores de shader: consumidos pelo recompilador de shaders (futuro)
        case pm4::NUM_INSTANCES:
            num_instances_ = body[0] ? body[0] : 1;
            break;
        case pm4::DRAW_INDEX_AUTO:
            submit_draw({primitive_, body[0], num_instances_, false, 0});
            break;
        case pm4::DRAW_INDEX_2: {
            // body: max_size, index_base_lo, index_base_hi, index_count, draw_initiator
            if (count < 4) break;
            const GuestVA ib = static_cast<GuestVA>(body[1]) | (static_cast<GuestVA>(body[2]) << 32);
            submit_draw({primitive_, body[3], num_instances_, true, ib});
            break;
        }
        case pm4::EVENT_WRITE_EOP:
            end_frame();  // fim de pipe: apresenta o frame
            break;
        default:
            ++stats_.unknown_packets;
            std::printf("[XONE-GPU ] opcode PM4 0x%02X não implementado\n", opcode);
            break;
    }
}

void GpuDevice::submit_draw(const DrawCall& dc) {
    ++stats_.draws;
    stats_.vertices += static_cast<std::uint64_t>(dc.vertex_count) * dc.instance_count;
    frame_draws_.push_back(dc);
    std::printf("[XONE-GPU ] DrawCall: prim=%u vertices=%u instancias=%u %s\n",
                static_cast<unsigned>(dc.primitive), dc.vertex_count, dc.instance_count,
                dc.indexed ? "(indexado)" : "(auto)");
}

void GpuDevice::end_frame() {
    ++stats_.frames;
    const std::uint32_t clear = context_regs_[kXoneClearColorReg - pm4::kContextRegBase];

#if defined(XONE_HAS_VULKAN)
    if (vk_) {
        auto& v = *vk_;
        VK_CHECK(vkResetCommandBuffer(v.cmd, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(v.cmd, &bi));

        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = v.image_initialized ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = v.color_image;
        barrier.subresourceRange = range;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(v.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        v.image_initialized = true;

        // Clear do render target com a cor programada pelo título (RGBA8 → float)
        VkClearColorValue cc{};
        cc.float32[0] = static_cast<float>((clear >> 24) & 0xFF) / 255.0f;
        cc.float32[1] = static_cast<float>((clear >> 16) & 0xFF) / 255.0f;
        cc.float32[2] = static_cast<float>((clear >> 8)  & 0xFF) / 255.0f;
        cc.float32[3] = static_cast<float>(clear & 0xFF) / 255.0f;
        vkCmdClearColorImage(v.cmd, v.color_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);

        // Os draws GCN entram aqui quando o recompilador de shaders GCN→SPIR-V
        // produzir pipelines: vkCmdBindPipeline + vkCmdDraw(dc.vertex_count, dc.instance_count, 0, 0).

        VK_CHECK(vkEndCommandBuffer(v.cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &v.cmd;
        VK_CHECK(vkQueueSubmit(v.queue, 1, &si, v.fence));
        VK_CHECK(vkWaitForFences(v.device, 1, &v.fence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(v.device, 1, &v.fence));
    }
#endif
    std::printf("[XONE-GPU ] Frame %llu apresentado via %s: %zu draws, clear=#%08X\n",
                static_cast<unsigned long long>(stats_.frames), backend_.c_str(), frame_draws_.size(), clear);
    frame_draws_.clear();
}

}  // namespace xone
