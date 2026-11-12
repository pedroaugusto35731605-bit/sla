// =============================================================================
//  SeriesS-Emu — gpu.cpp
//  Command Processor PM4 + contexto Vulkan + upscaling 1440p → 4K.
// =============================================================================
#include "gpu.hpp"

#include "log.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#if defined(XSS_HAS_VULKAN)
#  include <vulkan/vulkan.h>
#endif

namespace xss {

// ============================================================================
//  Contexto Vulkan
// ============================================================================
struct GpuDevice::VulkanContext {
#if defined(XSS_HAS_VULKAN)
    VkInstance       instance     = VK_NULL_HANDLE;
    VkPhysicalDevice physical     = VK_NULL_HANDLE;
    VkDevice         device       = VK_NULL_HANDLE;
    VkQueue          queue        = VK_NULL_HANDLE;
    std::uint32_t    queue_family = 0;
    VkCommandPool    cmd_pool     = VK_NULL_HANDLE;
    VkCommandBuffer  cmd          = VK_NULL_HANDLE;
    VkFence          fence        = VK_NULL_HANDLE;

    struct Image { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; bool used = false; };
    Image render;   // 2560x1440
    Image output;   // 3840x2160
    VkFilter blit_filter = VK_FILTER_LINEAR;

    ~VulkanContext() {
        if (device) {
            vkDeviceWaitIdle(device);
            for (Image* i : {&render, &output}) {
                if (i->img) vkDestroyImage(device, i->img, nullptr);
                if (i->mem) vkFreeMemory(device, i->mem, nullptr);
            }
            if (fence)    vkDestroyFence(device, fence, nullptr);
            if (cmd_pool) vkDestroyCommandPool(device, cmd_pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
#endif
};

#if defined(XSS_HAS_VULKAN)
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

#define VK_CHECK(expr)                                                               \
    do {                                                                             \
        VkResult _r = (expr);                                                        \
        if (_r != VK_SUCCESS)                                                        \
            throw std::runtime_error(std::string(#expr " -> ") + vk_result_str(_r)); \
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

void create_image(VkPhysicalDevice pd, VkDevice dev, std::uint32_t w, std::uint32_t h,
                  VkImage& img, VkDeviceMemory& mem) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.format        = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent        = {w, h, 1};
    ii.mipLevels     = 1;
    ii.arrayLayers   = 1;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ii, nullptr, &img));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, img, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i; break;
        }
    if (type == UINT32_MAX) throw std::runtime_error("sem memória DEVICE_LOCAL compatível");
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &mem));
    VK_CHECK(vkBindImageMemory(dev, img, mem, 0));
}

void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
             VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}
}  // namespace
#endif

// ============================================================================
GpuDevice::GpuDevice(MemoryManager& mem) : mem_(mem), context_regs_(0x400, 0) {}

GpuDevice::~GpuDevice() {
    vk_.reset();
    for (AllocationId id : {rt_1440p_, rt_4k_})
        if (id) try { mem_.free(id); } catch (...) {}
}

bool GpuDevice::initialize(bool enable_validation) {
    const auto& d = spec::kDisplay;
    log_info("GPU", "{} | {} CUs @ {:.3f} GHz | {:.2f} TFLOPS | wave{} | RT={} | Mesh={} | SFS={}",
             spec::kGpu.name, spec::kGpu.compute_units, static_cast<double>(spec::kGpu.clock_hz) / 1e9,
             spec::kComputedTflops, spec::kGpu.wave_size, spec::kGpu.hw_raytracing, spec::kGpu.mesh_shaders,
             spec::kGpu.sampler_feedback);

    // Render targets na memória unificada (contam nos 7 GB do título)
    rt_1440p_ = mem_.allocate(Partition::User, std::uint64_t{d.render_width} * d.render_height * 4,
                              "GPU: render target 1440p RGBA8", alloc_flags::kPinned);
    rt_4k_    = mem_.allocate(Partition::User, std::uint64_t{d.output_width} * d.output_height * 4,
                              "GPU: front buffer 4K RGBA8", alloc_flags::kPinned);

#if defined(XSS_HAS_VULKAN)
    try {
        auto ctx = std::make_unique<VulkanContext>();

        // 1. Instância
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "SeriesS-Emu";
        app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app.pEngineName = "XSS-Core";
        app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        app.apiVersion = VK_API_VERSION_1_2;
        const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        if (enable_validation) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers; }
        VK_CHECK(vkCreateInstance(&ici, nullptr, &ctx->instance));

        // 2. Dispositivo físico (prefere GPU discreta)
        std::uint32_t count = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(ctx->instance, &count, nullptr));
        if (count == 0) throw std::runtime_error("nenhum dispositivo Vulkan");
        std::vector<VkPhysicalDevice> pds(count);
        VK_CHECK(vkEnumeratePhysicalDevices(ctx->instance, &count, pds.data()));
        int best = -1;
        for (auto pd : pds) if (int s = device_score(pd); s > best) { best = s; ctx->physical = pd; }
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(ctx->physical, &props);
        device_ = props.deviceName;

        // 3. Fila graphics + compute
        std::uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical, &qn, qfs.data());
        bool found = false;
        for (std::uint32_t i = 0; i < qn && !found; ++i)
            if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                ctx->queue_family = i; found = true;
            }
        if (!found) throw std::runtime_error("nenhuma fila graphics+compute");

        // 4. Dispositivo lógico
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = ctx->queue_family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        VK_CHECK(vkCreateDevice(ctx->physical, &dci, nullptr, &ctx->device));
        vkGetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);

        // 5. Comandos
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

        // 6. Render target 1440p + saída 4K
        create_image(ctx->physical, ctx->device, d.render_width, d.render_height, ctx->render.img, ctx->render.mem);
        create_image(ctx->physical, ctx->device, d.output_width, d.output_height, ctx->output.img, ctx->output.mem);
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(ctx->physical, VK_FORMAT_R8G8B8A8_UNORM, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
            !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
            throw std::runtime_error("RGBA8 sem suporte a blit");
        ctx->blit_filter = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                               ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

        vk_ = std::move(ctx);
        backend_ = "Vulkan 1.2";
        upscale_ = true;
        log_info("GPU", "Vulkan inicializado: {} (API {}.{}.{}) | render {}x{} → saída {}x{} (upscaling {})",
                 device_, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                 VK_VERSION_PATCH(props.apiVersion), d.render_width, d.render_height, d.output_width,
                 d.output_height, vk_->blit_filter == VK_FILTER_LINEAR ? "bilinear" : "nearest");
        return true;
    } catch (const std::exception& e) {
        log_warn("GPU", "Vulkan indisponível ({}) — usando backend headless", e.what());
        vk_.reset();
        return false;
    }
#else
    (void)enable_validation;
    log_warn("GPU", "compilado sem Vulkan — usando backend headless");
    return false;
#endif
}

// ============================================================================
//  Command Processor (PM4)
// ============================================================================
void GpuDevice::submit_command_buffer(GuestVA va, std::uint32_t dwords) {
    std::vector<std::uint32_t> buf(dwords);
    mem_.read_virtual(va, buf.data(), std::uint64_t{dwords} * 4);
    std::uint32_t i = 0;
    while (i < dwords) {
        const std::uint32_t hdr = buf[i];
        const std::uint32_t type = hdr >> 30;
        if (type == 2) { ++i; continue; }
        if (type != 3) {
            log_warn("GPU", "pacote PM4 tipo {} não suportado em +{} — abortando IB", type, i);
            ++stats_.unknown_packets;
            return;
        }
        const std::uint32_t count = ((hdr >> 16) & 0x3FFF) + 1;
        const auto opcode = static_cast<std::uint8_t>((hdr >> 8) & 0xFF);
        if (i + 1 + count > dwords) { log_warn("GPU", "pacote PM4 truncado em +{}", i); return; }
        execute_packet(opcode, &buf[i + 1], count);
        ++stats_.packets;
        i += 1 + count;
    }
}

void GpuDevice::execute_packet(std::uint8_t opcode, const std::uint32_t* body, std::uint32_t count) {
    switch (opcode) {
        case pm4::NOP: case pm4::SET_SH_REG: break;
        case pm4::CLEAR_STATE: std::fill(context_regs_.begin(), context_regs_.end(), 0u); break;
        case pm4::SET_CONTEXT_REG:
            for (std::uint32_t k = 1; k < count && body[0] + k - 1 < context_regs_.size(); ++k)
                context_regs_[body[0] + k - 1] = body[k];
            break;
        case pm4::SET_UCONFIG_REG:
            if (count >= 2 && body[0] + pm4::kUconfigRegBase == pm4::VGT_PRIMITIVE_TYPE)
                primitive_ = static_cast<PrimitiveType>(body[1]);
            break;
        case pm4::NUM_INSTANCES: num_instances_ = body[0] ? body[0] : 1; break;
        case pm4::DRAW_INDEX_AUTO: submit_draw({primitive_, body[0], num_instances_, false, 0}); break;
        case pm4::DRAW_INDEX_2:
            if (count >= 4)
                submit_draw({primitive_, body[3], num_instances_, true,
                             static_cast<GuestVA>(body[1]) | (static_cast<GuestVA>(body[2]) << 32)});
            break;
        case pm4::DISPATCH_DIRECT:
            ++stats_.dispatches;
            log_info("GPU", "Dispatch compute {}x{}x{} (wave{})", body[0], count > 1 ? body[1] : 1,
                     count > 2 ? body[2] : 1, spec::kGpu.wave_size);
            break;
        case pm4::EVENT_WRITE_EOP: end_frame(); break;
        default:
            ++stats_.unknown_packets;
            log_warn("GPU", "opcode PM4 0x{:02X} não implementado", opcode);
    }
}

void GpuDevice::submit_draw(const DrawCall& dc) {
    ++stats_.draws;
    stats_.vertices += std::uint64_t{dc.vertex_count} * dc.instance_count;
    frame_draws_.push_back(dc);
    log_info("GPU", "DrawCall: prim={} vértices={} instâncias={} {}", static_cast<unsigned>(dc.primitive),
             dc.vertex_count, dc.instance_count, dc.indexed ? "(indexado)" : "(auto)");
}

void GpuDevice::end_frame() {
    ++stats_.frames;
    const std::uint32_t clear = context_regs_[kXssClearColorReg - pm4::kContextRegBase];
#if defined(XSS_HAS_VULKAN)
    if (vk_) {
        auto& v = *vk_;
        const auto& d = spec::kDisplay;
        VK_CHECK(vkResetCommandBuffer(v.cmd, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(v.cmd, &bi));

        // (a) renderiza em 1440p — hoje só o clear; os draws entram com o recompilador RDNA→SPIR-V
        barrier(v.cmd, v.render.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue cc{};
        for (int c = 0; c < 4; ++c) cc.float32[c] = static_cast<float>((clear >> (24 - 8 * c)) & 0xFF) / 255.0f;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(v.cmd, v.render.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);

        // (b) upscaling 2560x1440 → 3840x2160
        barrier(v.cmd, v.render.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        barrier(v.cmd, v.output.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<std::int32_t>(d.render_width), static_cast<std::int32_t>(d.render_height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {static_cast<std::int32_t>(d.output_width), static_cast<std::int32_t>(d.output_height), 1};
        vkCmdBlitImage(v.cmd, v.render.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, v.output.img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, v.blit_filter);

        VK_CHECK(vkEndCommandBuffer(v.cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &v.cmd;
        VK_CHECK(vkQueueSubmit(v.queue, 1, &si, v.fence));
        VK_CHECK(vkWaitForFences(v.device, 1, &v.fence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(v.device, 1, &v.fence));
    }
#endif
    log_info("GPU", "Frame {} via {}: {} draws, clear=#{:08X}{}", stats_.frames, backend_, frame_draws_.size(), clear,
             upscale_ ? ", 1440p → 4K" : "");
    frame_draws_.clear();
}

}  // namespace xss
