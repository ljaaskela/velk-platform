#include "vk_backend.h"

#include "vk_command_buffer.h"
#include "vk_gpu_buffer.h"
#include "vk_gpu_pipeline.h"
#include "vk_gpu_texture.h"
#include "vk_render_target_group.h"

#include <velk/api/perf.h>
#include <velk/api/velk.h>

#include <cstdlib>
#include <cstring>

namespace velk::vk {

#ifdef VELK_RENDER_DEBUG
#define RENDER_LOG(...) VELK_LOG(I, __VA_ARGS__)
#else
#define RENDER_LOG(...) ((void)0)
#endif

void VkBackend::cmd_push_label(::VkCommandBuffer cb, const char* name)
{
    if (!vkCmdBeginDebugUtilsLabelEXT) return;
    VkDebugUtilsLabelEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    info.pLabelName = name;
    vkCmdBeginDebugUtilsLabelEXT(cb, &info);
}

void VkBackend::cmd_pop_label(::VkCommandBuffer cb)
{
    if (!vkCmdEndDebugUtilsLabelEXT) return;
    vkCmdEndDebugUtilsLabelEXT(cb);
}

namespace {

VkFormat vk_format_for(PixelFormat f)
{
    switch (f) {
        case PixelFormat::R8:         return VK_FORMAT_R8_UNORM;
        case PixelFormat::RGBA8:      return VK_FORMAT_R8G8B8A8_UNORM;
        case PixelFormat::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
        case PixelFormat::RGBA16F:    return VK_FORMAT_R16G16B16A16_SFLOAT;
        case PixelFormat::RGBA32F:    return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkFormat choose_surface_format(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr);
    vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, formats.data());

    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f.format;
        }
    }
    return formats.empty() ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
}

VkPresentModeKHR choose_present_mode(VkPhysicalDevice device, VkSurfaceKHR surface, UpdateRate rate)
{
    // VSync (default): always FIFO. Always supported, capped to display refresh.
    if (rate == UpdateRate::VSync) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    // Unlimited / Targeted: prefer IMMEDIATE (no vsync, may tear),
    // fall back to MAILBOX (triple-buffered, no tearing), then FIFO.
    // Targeted mode relies on software pacing (Application sleeps between frames).
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, nullptr);
    vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, modes.data());

    for (auto m : modes) {
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            return m;
        }
    }
    for (auto m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
            return m;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

#ifndef NDEBUG
VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* /*user_data*/)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        VELK_LOG(E, "Vulkan: %s", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        VELK_LOG(W, "Vulkan: %s", data->pMessage);
    }
    return VK_FALSE;
}
#endif

} // namespace

VkBackend::~VkBackend()
{
    if (initialized_) {
        VkBackend::shutdown();
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

bool VkBackend::init(void* params)
{
    if (initialized_) {
        return true;
    }

    VELK_PERF_SCOPE("vk.init");

    if (volkInitialize() != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: volk init failed");
        return false;
    }

    if (!create_vk_instance()) {
        return false;
    }
    volkLoadInstance(instance_);

#ifndef NDEBUG
    {
        VkDebugUtilsMessengerCreateInfoEXT ci{};
        ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        ci.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        ci.pfnUserCallback = debug_callback;
        vkCreateDebugUtilsMessengerEXT(instance_, &ci, nullptr, &debug_messenger_);
    }
#endif

    // Create the first surface from init params (needed for device selection)
    auto* vk_params = static_cast<VulkanInitParams*>(params);
    VkSurfaceKHR initial_surface = VK_NULL_HANDLE;
    if (vk_params && vk_params->create_surface) {
        // Retain the callback so recreate_surface can rebuild the platform
        // surface handle from a new native window after suspend/resume.
        surface_create_fn_ = vk_params->create_surface;
        if (!vk_params->create_surface(instance_, &initial_surface, vk_params->user_data)) {
            VELK_LOG(E, "VkBackend: failed to create initial surface");
            return false;
        }
    }

    // Store it temporarily so device selection can check present support
    SurfaceData initial_sd{};
    initial_sd.surface = initial_surface;
    surfaces_[next_surface_id_] = initial_sd;

    if (!select_physical_device()) {
        return false;
    }
    if (!create_device()) {
        return false;
    }
    volkLoadDevice(device_);

    if (!create_allocator()) {
        return false;
    }
    if (!create_command_pool()) {
        return false;
    }
    if (!create_sync_objects()) {
        return false;
    }
    if (!create_bindless_descriptor()) {
        return false;
    }
    if (!create_bound_buffer_descriptor()) {
        return false;
    }
    if (!create_pipeline_layout()) {
        return false;
    }

    initialized_ = true;
    VELK_LOG(I, "VkBackend: initialized (Vulkan 1.3, BDA + bindless + dynamic rendering)");
    return true;
}

void VkBackend::wait_idle()
{
    if (initialized_ && device_) {
        vkDeviceWaitIdle(device_);
    }
}

void VkBackend::shutdown()
{
    if (!initialized_) {
        return;
    }

    vkDeviceWaitIdle(device_);

    // vkDeviceWaitIdle above lets pending VMA destroys run unconditionally.
    {
        std::lock_guard<std::mutex> lock(deferred_gpu_buffers_mutex_);
        for (auto& d : deferred_gpu_buffers_) {
            vmaDestroyBuffer(allocator_, d.buffer, d.allocation);
        }
        deferred_gpu_buffers_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(deferred_gpu_pipelines_mutex_);
        for (auto& d : deferred_gpu_pipelines_) {
            vkDestroyPipeline(device_, d.pipeline, nullptr);
        }
        deferred_gpu_pipelines_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(deferred_gpu_textures_mutex_);
        for (auto& d : deferred_gpu_textures_) {
            if (d.view)             vkDestroyImageView(device_, d.view, nullptr);
            if (d.image)            vmaDestroyImage(allocator_, d.image, d.allocation);
        }
        deferred_gpu_textures_.clear();
    }

    // Destroy surfaces
    for (auto& [id, sd] : surfaces_) {
        destroy_swapchain(sd);
        if (sd.surface) {
            vkDestroySurfaceKHR(instance_, sd.surface, nullptr);
        }
    }
    surfaces_.clear();

    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, bound_buffer_layout_, nullptr);
    vkDestroyDescriptorPool(device_, bound_buffer_pool_, nullptr);
    // linear_sampler_ lives inside sampler_cache_ (primed at init); loop
    // destroys everything in one pass.
    for (auto& [key, sampler] : sampler_cache_) {
        vkDestroySampler(device_, sampler, nullptr);
    }
    sampler_cache_.clear();
    linear_sampler_ = VK_NULL_HANDLE;

    for (uint32_t i = 0; i < kFrameOverlap; ++i) {
        vkDestroyFence(device_, frame_sync_[i].fence, nullptr);
    }
    for (uint32_t i = 0; i < kMaxSwapchainImages; ++i) {
        vkDestroySemaphore(device_, image_available_[i], nullptr);
        vkDestroySemaphore(device_, render_finished_[i], nullptr);
    }
    if (frame_timeline_) {
        vkDestroySemaphore(device_, frame_timeline_, nullptr);
        frame_timeline_ = VK_NULL_HANDLE;
    }
    if (timestamp_pool_) {
        vkDestroyQueryPool(device_, timestamp_pool_, nullptr);
        timestamp_pool_ = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < kFrameOverlap; ++i) {
        // Pool destruction frees all buffers in the pool — the deferred
        // queue would dangle. Clear it explicitly first.
        frame_sync_[i].deferred_persistent_frees.clear();
    }
    if (persistent_secondary_pool_) {
        vkDestroyCommandPool(device_, persistent_secondary_pool_, nullptr);
        persistent_secondary_pool_ = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < kFrameOverlap; ++i) {
        if (frame_sync_[i].command_pool) {
            vkDestroyCommandPool(device_, frame_sync_[i].command_pool, nullptr);
            frame_sync_[i].command_pool = VK_NULL_HANDLE;
        }
    }
    vkDestroyCommandPool(device_, command_pool_, nullptr);

    vmaDestroyAllocator(allocator_);

#ifndef NDEBUG
    if (debug_messenger_) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
    }
#endif

    vkDestroyDevice(device_, nullptr);
    vkDestroyInstance(instance_, nullptr);

    initialized_ = false;
}

// ============================================================================
// Instance / device setup
// ============================================================================

bool VkBackend::create_vk_instance()
{
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "velk-ui";
    app_info.apiVersion = VK_API_VERSION_1_3;

    vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(_WIN32)
        "VK_KHR_win32_surface",
#elif defined(__ANDROID__)
        "VK_KHR_android_surface",
#endif
        // Always enabled: debug-utils labels are free when no debugger
        // is attached and let RenderDoc / Nsight group events by our
        // pass names instead of falling back to confused auto-grouping.
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };

    // Validation on by default in debug builds (both desktop and Android).
    // Android sample APKs bundle libVkLayer_khronos_validation.so under
    // lib/<abi>/ via the gradle downloadValidationLayers task — without that,
    // vkCreateInstance fails with VK_ERROR_LAYER_NOT_PRESENT.
    bool enable_validation = false;
#ifndef NDEBUG
    enable_validation = true;
#endif
    if (const char* v = std::getenv("VELK_VK_VALIDATION")) {
        enable_validation = (v[0] == '1');
    }

    vector<const char*> layers;
    if (enable_validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app_info;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();

    if (VkResult r = vkCreateInstance(&ci, nullptr, &instance_); r != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: vkCreateInstance failed (VkResult=%d)", static_cast<int>(r));
        return false;
    }
    return true;
}

bool VkBackend::select_physical_device()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        VELK_LOG(E, "VkBackend: no Vulkan devices found");
        return false;
    }

    vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    // Prefer discrete GPU
    physical_device_ = devices[0];
    for (auto d : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physical_device_ = d;
            break;
        }
    }

    // Find graphics queue family with present support
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, nullptr);
    vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, families.data());

    // Check against the first surface for present support
    VkSurfaceKHR check_surface = VK_NULL_HANDLE;
    for (auto& [id, sd] : surfaces_) {
        if (sd.surface) {
            check_surface = sd.surface;
            break;
        }
    }

    bool found = false;
    for (uint32_t i = 0; i < family_count; ++i) {
        if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            continue;
        }
        if (check_surface) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, i, check_surface, &present);
            if (!present) {
                continue;
            }
        }
        graphics_family_ = i;
        found = true;
        break;
    }

    if (!found) {
        VELK_LOG(E, "VkBackend: no suitable queue family");
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device_, &props);
    VELK_LOG(I, "VkBackend: using %s", props.deviceName);

    // GPU timing support: timestampComputeAndGraphics guarantees the
    // graphics/compute queues report valid timestamp bits. A zero period
    // means timestamps are unsupported; leave the feature disabled.
    if (props.limits.timestampComputeAndGraphics && props.limits.timestampPeriod > 0.f) {
        timestamp_period_ns_ = props.limits.timestampPeriod;
    }
    return true;
}

bool VkBackend::create_device()
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_ci{};
    queue_ci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_ci.queueFamilyIndex = graphics_family_;
    queue_ci.queueCount = 1;
    queue_ci.pQueuePriorities = &priority;

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    // Vulkan 1.2 features: BDA + descriptor indexing
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.descriptorIndexing = VK_TRUE;
    // Per-block scalar packing (opt-in via `layout(scalar)` on a
    // buffer_reference block). Used by the 3D mesh vertex path so
    // `Vertex3D { vec3 pos; vec3 normal; vec2 uv; }` packs tightly to
    // 32 bytes instead of std430's 48-byte vec3=16-align stride.
    features12.scalarBlockLayout = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
    // The per-frame "global buffer" set (set = 1) fills its STORAGE_BUFFER
    // descriptors during prepare, after the set was bound at begin_frame.
    features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    // Required for vkCmdDrawIndexedIndirectCount / vkCmdDrawIndirectCount —
    // the always-indirect emission path lets the GPU determine the actual
    // draw count later (post-culling) without a CPU readback.
    features12.drawIndirectCount = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
    features12.timelineSemaphore = VK_TRUE;

    // Vulkan 1.3: dynamic rendering. Lets vkCmdBeginRendering bind
    // attachments inline at record time without VkRenderPass /
    // VkFramebuffer objects.
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features12.pNext = &features13;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features12;
    features2.features.shaderInt64 = VK_TRUE;

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext = &features2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &queue_ci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = extensions;

    if (vkCreateDevice(physical_device_, &ci, nullptr, &device_) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create device");
        return false;
    }

    vkGetDeviceQueue(device_, graphics_family_, 0, &graphics_queue_);
    return true;
}

bool VkBackend::create_allocator()
{
    VmaVulkanFunctions vma_funcs{};
    vma_funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vma_funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo ci{};
    ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    ci.physicalDevice = physical_device_;
    ci.device = device_;
    ci.instance = instance_;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.pVulkanFunctions = &vma_funcs;

    if (vmaCreateAllocator(&ci, &allocator_) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create VMA allocator");
        return false;
    }
    return true;
}

bool VkBackend::create_command_pool()
{
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = graphics_family_;

    // command_pool_ stays around for one-shot ops (begin_one_shot_commands).
    // Frame-loop primary cmd buffers come from per-slot pools instead — see
    // FrameSync::command_pool — so begin_frame (main thread) and end_frame
    // (render thread) never touch the same pool concurrently.
    if (vkCreateCommandPool(device_, &ci, nullptr, &command_pool_) != VK_SUCCESS) {
        return false;
    }

    for (uint32_t i = 0; i < kFrameOverlap; ++i) {
        if (vkCreateCommandPool(device_, &ci, nullptr, &frame_sync_[i].command_pool) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = frame_sync_[i].command_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &alloc_info, &frame_sync_[i].command_buffer) != VK_SUCCESS) {
            return false;
        }
    }

    // Long-lived secondary pool: producers' cached cmd buffers
    // (`create_command_buffer`) outlive any one frame, so the pool
    // is not reset between frames. Individual buffers are freed on
    // `IGpuCommandBuffer` destruction.
    VkCommandPoolCreateInfo spci = ci;
    spci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(device_, &spci, nullptr, &persistent_secondary_pool_) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool VkBackend::create_sync_objects()
{
    VkSemaphoreCreateInfo sem_ci{};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < kFrameOverlap; ++i) {
        if (vkCreateFence(device_, &fence_ci, nullptr, &frame_sync_[i].fence) != VK_SUCCESS) {
            return false;
        }
    }

    // Per-image semaphores: acquire and present semaphores indexed by swapchain image
    for (uint32_t i = 0; i < kMaxSwapchainImages; ++i) {
        if (vkCreateSemaphore(device_, &sem_ci, nullptr, &image_available_[i]) != VK_SUCCESS) {
            return false;
        }
        if (vkCreateSemaphore(device_, &sem_ci, nullptr, &render_finished_[i]) != VK_SUCCESS) {
            return false;
        }
    }

    // Timeline semaphore for per-submit GPU completion tracking.
    VkSemaphoreTypeCreateInfo timeline_type{};
    timeline_type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_type.initialValue = 0;
    VkSemaphoreCreateInfo timeline_ci{};
    timeline_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    timeline_ci.pNext = &timeline_type;
    if (vkCreateSemaphore(device_, &timeline_ci, nullptr, &frame_timeline_) != VK_SUCCESS) {
        return false;
    }

    // Timestamp query pool for per-pass GPU timing. One contiguous range
    // per frame slot (2 timestamps per timed region). A failed create or
    // unsupported timestamps just leaves the feature inert.
    if (timestamp_period_ns_ > 0.f) {
        VkQueryPoolCreateInfo qpci{};
        qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = kFrameOverlap * 2 * kMaxTimedPasses;
        if (vkCreateQueryPool(device_, &qpci, nullptr, &timestamp_pool_) != VK_SUCCESS) {
            timestamp_pool_ = VK_NULL_HANDLE;
            timestamp_period_ns_ = 0.f;
        }
    }
    return true;
}

namespace {

VkFilter to_vk_filter(SamplerFilter f)
{
    return f == SamplerFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerMipmapMode to_vk_mipmap_mode(SamplerMipmapMode m)
{
    return m == SamplerMipmapMode::Nearest
        ? VK_SAMPLER_MIPMAP_MODE_NEAREST
        : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

VkSamplerAddressMode to_vk_address(SamplerAddressMode a)
{
    switch (a) {
    case SamplerAddressMode::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case SamplerAddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case SamplerAddressMode::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

} // namespace

size_t VkBackend::SamplerKeyHash::operator()(const SamplerKey& k) const noexcept
{
    // Pack the six 8-bit enums into a single 64-bit hash input. Each enum
    // value fits in 3 bits today; using a full byte each keeps the packing
    // robust to future enum growth.
    uint64_t v = 0;
    v |= static_cast<uint64_t>(k.desc.wrap_s)      << 0;
    v |= static_cast<uint64_t>(k.desc.wrap_t)      << 8;
    v |= static_cast<uint64_t>(k.desc.wrap_r)      << 16;
    v |= static_cast<uint64_t>(k.desc.mag_filter)  << 24;
    v |= static_cast<uint64_t>(k.desc.min_filter)  << 32;
    v |= static_cast<uint64_t>(k.desc.mipmap_mode) << 40;
    return std::hash<uint64_t>{}(v);
}

VkSampler VkBackend::get_or_create_sampler(const SamplerDesc& desc)
{
    SamplerKey key{desc};
    if (auto it = sampler_cache_.find(key); it != sampler_cache_.end()) {
        return it->second;
    }

    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = to_vk_filter(desc.mag_filter);
    ci.minFilter = to_vk_filter(desc.min_filter);
    ci.mipmapMode = to_vk_mipmap_mode(desc.mipmap_mode);
    ci.addressModeU = to_vk_address(desc.wrap_s);
    ci.addressModeV = to_vk_address(desc.wrap_t);
    ci.addressModeW = to_vk_address(desc.wrap_r);
    ci.minLod = 0.f;
    ci.maxLod = VK_LOD_CLAMP_NONE;

    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device_, &ci, nullptr, &sampler) != VK_SUCCESS) {
        return linear_sampler_;
    }
    sampler_cache_.emplace(key, sampler);
    return sampler;
}

bool VkBackend::create_bindless_descriptor()
{
    // Prime the cache with the default (Repeat + Linear) sampler and keep
    // a named reference so call sites that want "the default" don't re-hash.
    // Shutdown destroys every cached sampler, including this one.
    linear_sampler_ = get_or_create_sampler(SamplerDesc{});
    if (!linear_sampler_) {
        return false;
    }

    // Descriptor set layout: four bindings
    //   0: variable-length sampler array (combined image+sampler) for sampled reads
    //   1: storage-image array for compute imageStore writes (rgba8-format)
    //   2: storage-image array for compute imageStore writes (rgba32f-format)
    //   3: storage-image array for compute imageStore writes (rgba16f-format)
    //
    // Per-view FrameGlobals are NOT a descriptor — shaders dereference a
    // GPU address pushed via push constants ([0..8) of the per-stage push
    // range). That keeps the descriptor set spec-compliant when bindings
    // 0..3 are flagged UPDATE_AFTER_BIND (which is required for the
    // transient-pool path where bindless descriptors are written
    // mid-command-buffer).
    VkDescriptorSetLayoutBinding bindings[4]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = kMaxBindlessTextures;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = kMaxBindlessTextures;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = kMaxBindlessTextures;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[3].descriptorCount = kMaxBindlessTextures;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorBindingFlags binding_flags[4] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
    };

    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci{};
    flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_ci.bindingCount = 4;
    flags_ci.pBindingFlags = binding_flags;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.pNext = &flags_ci;
    layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_ci.bindingCount = 4;
    layout_ci.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &descriptor_layout_) != VK_SUCCESS) {
        return false;
    }

    // Pool
    VkDescriptorPoolSize pool_sizes[4]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = kMaxBindlessTextures;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[1].descriptorCount = kMaxBindlessTextures;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[2].descriptorCount = kMaxBindlessTextures;
    pool_sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[3].descriptorCount = kMaxBindlessTextures;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 4;
    pool_ci.pPoolSizes = pool_sizes;

    if (vkCreateDescriptorPool(device_, &pool_ci, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &descriptor_layout_;

    return vkAllocateDescriptorSets(device_, &alloc_info, &descriptor_set_) == VK_SUCCESS;
}

bool VkBackend::create_bound_buffer_descriptor()
{
    // set = 1: a small fixed set of bound storage buffers that compute
    // shaders read by index (BVH nodes/shapes; GpuArena/GpuHive pages
    // later). A SINGLE frame-invariant set, bound by both the primary and
    // every simultaneous-use secondary. Per-frame variance lives in the
    // buffer contents (IGpuArena refreshes them in place); the descriptor
    // is rewritten only when a buffer's address changes (first bind + the
    // rare growth realloc), so steady state never disturbs in-flight
    // reads. UPDATE_AFTER_BIND because the arena writes the descriptor
    // during prepare, after the set was bound. PARTIALLY_BOUND lets no-BVH
    // frames leave the slots unbound (shaders gate on bvh_node_count == 0).
    VkDescriptorSetLayoutBinding bindings[IRenderBackend::kGlobalBufferSlotCount]{};
    VkDescriptorBindingFlags binding_flags[IRenderBackend::kGlobalBufferSlotCount]{};
    for (uint32_t i = 0; i < IRenderBackend::kGlobalBufferSlotCount; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        binding_flags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                           VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci{};
    flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_ci.bindingCount = IRenderBackend::kGlobalBufferSlotCount;
    flags_ci.pBindingFlags = binding_flags;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.pNext = &flags_ci;
    layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_ci.bindingCount = IRenderBackend::kGlobalBufferSlotCount;
    layout_ci.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &bound_buffer_layout_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = IRenderBackend::kGlobalBufferSlotCount;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;

    if (vkCreateDescriptorPool(device_, &pool_ci, nullptr, &bound_buffer_pool_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = bound_buffer_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &bound_buffer_layout_;

    return vkAllocateDescriptorSets(device_, &alloc_info, &bound_buffer_set_) == VK_SUCCESS;
}

bool VkBackend::create_pipeline_layout()
{
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_ALL;
    push_range.offset = 0;
    push_range.size = kMaxRootConstantsSize;

    // set 0 = bindless arrays (shared), set 1 = per-frame bound buffers.
    VkDescriptorSetLayout set_layouts[2] = { descriptor_layout_, bound_buffer_layout_ };

    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = 2;
    ci.pSetLayouts = set_layouts;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_, &ci, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create pipeline layout");
        return false;
    }
    return true;
}

// ============================================================================
// Surfaces
// ============================================================================

uint64_t VkBackend::create_surface(const SurfaceDesc& desc)
{
    // Check if we already have an initial surface from init()
    for (auto& [id, sd] : surfaces_) {
        if (sd.surface && sd.swapchain == VK_NULL_HANDLE) {
            sd.width = desc.width;
            sd.height = desc.height;
            sd.update_rate = desc.update_rate;
            sd.color_format = desc.color_format;
            if (!create_swapchain(sd)) {
                return 0;
            }
            create_surface_composite(id, sd);
            return id;
        }
    }

    // Otherwise create a new one (would need a surface creation callback)
    VELK_LOG(E, "VkBackend: additional surface creation not yet supported");
    return 0;
}

void VkBackend::destroy_surface(uint64_t surface_id)
{
    auto it = surfaces_.find(surface_id);
    if (it == surfaces_.end()) {
        return;
    }

    vkDeviceWaitIdle(device_);
    destroy_swapchain(it->second);
    if (it->second.surface) {
        vkDestroySurfaceKHR(instance_, it->second.surface, nullptr);
    }
    surfaces_.erase(it);
}

void VkBackend::resize_surface(uint64_t surface_id, int width, int height)
{
    auto it = surfaces_.find(surface_id);
    if (it == surfaces_.end()) {
        return;
    }

    vkDeviceWaitIdle(device_);
    it->second.width = width;
    it->second.height = height;
    destroy_swapchain(it->second);
    create_swapchain(it->second);
    create_surface_composite(surface_id, it->second);
}

void VkBackend::recreate_surface(uint64_t surface_id, void* native_handle,
                                 int width, int height)
{
    auto it = surfaces_.find(surface_id);
    if (it == surfaces_.end() || !surface_create_fn_) {
        return;
    }
    auto& sd = it->second;

    // Caller (Android resume) guarantees no frame is in flight; wait idle
    // before tearing the old swapchain + (now-invalid) surface handle down.
    vkDeviceWaitIdle(device_);
    destroy_swapchain(sd);
    if (sd.surface) {
        vkDestroySurfaceKHR(instance_, sd.surface, nullptr);
        sd.surface = VK_NULL_HANDLE;
    }

    VkSurfaceKHR new_surface = VK_NULL_HANDLE;
    if (!surface_create_fn_(instance_, &new_surface, native_handle)) {
        VELK_LOG(E, "VkBackend::recreate_surface: surface-create callback failed");
        return;
    }
    sd.surface = new_surface;
    sd.width = width;
    sd.height = height;
    if (!create_swapchain(sd)) {
        VELK_LOG(E, "VkBackend::recreate_surface: create_swapchain failed");
        return;
    }
    create_surface_composite(surface_id, sd);
}

IRenderTarget::Ptr VkBackend::acquire_swapchain_texture(uint64_t surface_id)
{
    auto it = surfaces_.find(surface_id);
    if (it == surfaces_.end()) return {};
    auto& sd = it->second;
    if (!sd.composite) return {};

    // First acquire this frame: emit the per-frame clear here (after
    // any pending resize_surface has run during the renderer's build
    // phase, but before producers record their secondaries that target
    // the composite). Multi-camera-per-surface: subsequent acquires
    // skip the clear; record_begin_rendering on the composite is
    // overridden to LOAD so subsequent views stack on top.
    if (!sd.composite_acquired_this_frame) {
        sd.composite_acquired_this_frame = true;
        auto* surf_tex = interface_cast<IVkGpuTexture>(sd.composite.get());
        if (surf_tex && surf_tex->vk_image() != VK_NULL_HANDLE) {
            auto& sync = frame_sync_[recording_slot_];
            cmd_push_label(sync.command_buffer, "Composite Clear");
            const VkImage img = surf_tex->vk_image();
            const VkImageLayout old_layout = surf_tex->vk_current_layout();

            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = 1;
            range.layerCount = 1;

            VkImageMemoryBarrier to_dst{};
            to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_dst.oldLayout = old_layout;
            to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_dst.image = img;
            to_dst.subresourceRange = range;
            to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(sync.command_buffer,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                     | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &to_dst);

            VkClearColorValue clear{};
            clear.float32[0] = 0.f; clear.float32[1] = 0.f;
            clear.float32[2] = 0.f; clear.float32[3] = 0.f;
            vkCmdClearColorImage(sync.command_buffer, img,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &clear, 1, &range);

            // Leave composite in SHADER_READ_ONLY post-clear so cached
            // record_begin_rendering barriers (baked as SHADER_READ_ONLY
            // → COLOR_ATTACHMENT_OPTIMAL) replay correctly.
            VkImageMemoryBarrier to_read{};
            to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_read.image = img;
            to_read.subresourceRange = range;
            to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(sync.command_buffer,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                     | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &to_read);
            surf_tex->set_vk_current_layout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            cmd_pop_label(sync.command_buffer);
        }
    }

    // Up-cast IGpuTexture::Ptr → IRenderTarget::Ptr via the wrapper
    // chain (VkSurfaceTexture IS-A IRenderTarget IS-A IGpuTexture).
    return interface_pointer_cast<IRenderTarget>(sd.composite);
}

bool VkBackend::create_swapchain(SurfaceData& sd)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, sd.surface, &caps);

    sd.image_format = choose_surface_format(physical_device_, sd.surface);
    VkPresentModeKHR present_mode = choose_present_mode(physical_device_, sd.surface, sd.update_rate);

    VkExtent2D extent;
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width = static_cast<uint32_t>(sd.width);
        extent.height = static_cast<uint32_t>(sd.height);
    }
    sd.width = static_cast<int>(extent.width);
    sd.height = static_cast<int>(extent.height);

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = sd.surface;
    ci.minImageCount = image_count;
    ci.imageFormat = sd.image_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    // TRANSFER_DST_BIT allows compute-RT views to blit into the swapchain
    // image via vkCmdBlitImage (the blit_to_surface path).
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // Use IDENTITY when supported so the compositor handles device rotation.
    // The alternative (preTransform = currentTransform) requires the app to
    // pre-rotate its content in the projection matrix; velk's camera path
    // doesn't do that yet, so rotated displays would show portrait-oriented
    // content. Fall back to currentTransform if IDENTITY isn't supported.
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    } else {
        ci.preTransform = caps.currentTransform;
    }
    // Pick a supported compositeAlpha. Android surfaces commonly only support
    // INHERIT (not OPAQUE), so the previous hardcoded OPAQUE caused
    // vkCreateSwapchainKHR to fail there. Prefer OPAQUE when available, fall
    // through to the other defined bits.
    constexpr VkCompositeAlphaFlagBitsKHR kCompositePrefs[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (auto bit : kCompositePrefs) {
        if (caps.supportedCompositeAlpha & bit) {
            ci.compositeAlpha = bit;
            break;
        }
    }
    ci.presentMode = present_mode;
    ci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &ci, nullptr, &sd.swapchain) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create swapchain");
        return false;
    }

    // Get images
    uint32_t img_count = 0;
    vkGetSwapchainImagesKHR(device_, sd.swapchain, &img_count, nullptr);
    if (img_count > kMaxSwapchainImages) {
        VELK_LOG(E, "VkBackend: swapchain returned %u images, exceeds kMaxSwapchainImages=%u",
                 img_count, kMaxSwapchainImages);
        return false;
    }
    sd.images.resize(img_count);
    vkGetSwapchainImagesKHR(device_, sd.swapchain, &img_count, sd.images.data());

    // Image views
    sd.image_views.resize(img_count);
    for (uint32_t i = 0; i < img_count; ++i) {
        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = sd.images[i];
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = sd.image_format;
        view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_ci.subresourceRange.levelCount = 1;
        view_ci.subresourceRange.layerCount = 1;

        vkCreateImageView(device_, &view_ci, nullptr, &sd.image_views[i]);
    }

    // Surfaces don't need their own render pass / framebuffer / depth
    // attachment. Producers render to the per-surface composite
    // (allocated separately in create_surface_composite); the backend
    // blits composite → swap image at end_frame. The swap images
    // themselves are only used as transfer destinations.
    return true;
}

bool VkBackend::create_surface_composite(uint64_t surface_id, SurfaceData& sd)
{
    if (sd.width <= 0 || sd.height <= 0) return false;

    // Composite format follows sd.color_format. Usage spread:
    //  - COLOR_ATTACHMENT for raster passes via record_begin_rendering.
    //  - SAMPLED so subsequent passes can sample (rare, but possible).
    //  - TRANSFER_SRC for the end_frame composite-to-swap blit.
    //  - TRANSFER_DST for record_blit_to_texture from compute / RT outputs
    //    into the composite (debug overlays, deferred lighting blit).
    //
    // Note: STORAGE is intentionally absent. Including it disables AFBC/UBWC
    // framebuffer compression on tile-based GPUs (Mali/Adreno). Tonemap and
    // other compute writers target a separate post_output image and blit into
    // the composite; nothing imageStores the composite directly.
    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    PixelFormat composite_format = PixelFormat::RGBA8_SRGB;
    switch (sd.color_format) {
    case SurfaceColorFormat::RGBA8_SRGB:
        vk_format = VK_FORMAT_R8G8B8A8_SRGB;
        composite_format = PixelFormat::RGBA8_SRGB;
        break;
    case SurfaceColorFormat::RGBA16F:
        vk_format = VK_FORMAT_R16G16B16A16_SFLOAT;
        composite_format = PixelFormat::RGBA16F;
        break;
    }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vk_format;
    ici.extent = { static_cast<uint32_t>(sd.width),
                   static_cast<uint32_t>(sd.height), 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
              | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
              | VK_IMAGE_USAGE_TRANSFER_DST_BIT
              | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateImage(allocator_, &ici, &aci, &image, &allocation, nullptr) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to allocate surface composite image");
        return false;
    }

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = vk_format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device_, &vci, nullptr, &view) != VK_SUCCESS) {
        vmaDestroyImage(allocator_, image, allocation);
        return false;
    }

    auto composite = ::velk::instance().create<IGpuTexture>(ClassId::VkSurfaceTexture);
    auto* surf_tex = interface_cast<IVkSurfaceTexture>(composite.get());
    if (!surf_tex) {
        vkDestroyImageView(device_, view, nullptr);
        vmaDestroyImage(allocator_, image, allocation);
        return false;
    }
    const uvec2 dims{ static_cast<uint32_t>(sd.width),
                      static_cast<uint32_t>(sd.height) };
    surf_tex->init(this, surface_id, image, view, allocation, dims,
                   composite_format);
    sd.composite = std::move(composite);
    return true;
}

void VkBackend::destroy_surface_composite(SurfaceData& sd)
{
    if (!sd.composite) return;
    auto* surf_tex = interface_cast<IVkSurfaceTexture>(sd.composite.get());
    if (surf_tex) {
        surf_tex->release(device_, allocator_);
    }
    sd.composite.reset();
}

void VkBackend::destroy_swapchain(SurfaceData& sd)
{
    destroy_surface_composite(sd);
    for (auto iv : sd.image_views) {
        vkDestroyImageView(device_, iv, nullptr);
    }
    if (sd.swapchain) {
        vkDestroySwapchainKHR(device_, sd.swapchain, nullptr);
    }
    sd.image_views.clear();
    sd.images.clear();
    sd.swapchain = VK_NULL_HANDLE;
}

// ============================================================================
// GPU Memory
// ============================================================================

IGpuBuffer::Ptr VkBackend::create_gpu_buffer(const GpuBufferDesc& desc)
{
    VkBufferCreateInfo buf_ci{};
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size = desc.size;
    buf_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                   VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (desc.index_buffer) {
        buf_ci.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }

    VmaAllocationCreateInfo alloc_ci{};
    if (desc.cpu_writable) {
        alloc_ci.flags =
            VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    } else {
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }

    ::VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator_, &buf_ci, &alloc_ci, &buffer, &allocation, &info) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create gpu buffer (%zu bytes)", desc.size);
        return {};
    }

    VkBufferDeviceAddressInfo addr_info{};
    addr_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addr_info.buffer = buffer;
    uint64_t address = vkGetBufferDeviceAddress(device_, &addr_info);

    auto gb = ::velk::instance().create<::velk::IGpuBuffer>(
        ::velk::ClassId::VkGpuBuffer);
    auto* vk_gb = interface_cast<IVkGpuBuffer>(gb.get());
    if (!vk_gb) {
        vmaDestroyBuffer(allocator_, buffer, allocation);
        return {};
    }
    vk_gb->init(this, buffer, allocation, info.pMappedData, desc.size, address);
    return gb;
}

void VkBackend::record_buffer_update(IGpuBuffer& target, size_t offset,
                                     size_t size, const void* data)
{
    if (size == 0 || !data) return;
    auto* vk_gb = interface_cast<IVkGpuBuffer>(&target);
    if (!vk_gb) return;
    ::VkBuffer buffer = vk_gb->vk_buffer();
    if (buffer == VK_NULL_HANDLE) return;
    auto& sync = frame_sync_[recording_slot_];
    vkCmdUpdateBuffer(sync.command_buffer, buffer, offset, size, data);
    sync.pending_buffer_update_barrier = true;
}

void VkBackend::set_global_buffer(uint32_t binding, IGpuBuffer* buffer)
{
    if (binding >= IRenderBackend::kGlobalBufferSlotCount) return;

    // The caller may pass a raw IGpuBuffer or a CPU-shadow IBuffer
    // wrapper whose backend handle lives on its attached storage.
    auto* vk_gb = interface_cast<IVkGpuBuffer>(buffer);
    IGpuBuffer::Ptr storage;  // keeps resolved storage alive across the cast
    if (!vk_gb) {
        if (auto* owner = interface_cast<IGpuBufferStorageOwner>(buffer)) {
            storage = owner->attached_gpu_buffer();
            vk_gb = interface_cast<IVkGpuBuffer>(storage.get());
        }
    }

    ::VkBuffer vk_buffer = vk_gb ? vk_gb->vk_buffer() : VK_NULL_HANDLE;
    // Null / unresolved: leave the slot as-is. PARTIALLY_BOUND keeps the
    // descriptor valid as long as the shader does not access it (no-BVH
    // frames gate on bvh_node_count == 0).
    if (vk_buffer == VK_NULL_HANDLE) return;

    VkDescriptorBufferInfo info{};
    info.buffer = vk_buffer;
    info.offset = 0;
    info.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = bound_buffer_set_;
    write.dstBinding = binding;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void VkBackend::defer_destroy_gpu_buffer(IGpuBuffer* gb, uint64_t completion_marker)
{
    if (!gb) return;
    if (completion_marker == kDefaultCompletionMarker) {
        completion_marker = frame_completion_marker();
    }
    auto* vk_gb = interface_cast<IVkGpuBuffer>(gb);
    if (!vk_gb) return;
    ::VkBuffer buffer = vk_gb->vk_buffer();
    VmaAllocation allocation = vk_gb->vk_allocation();
    if (buffer == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(deferred_gpu_buffers_mutex_);
    deferred_gpu_buffers_.push_back({buffer, allocation, completion_marker});
}

void VkBackend::drain_deferred_buffers()
{
    std::lock_guard<std::mutex> lock(deferred_gpu_buffers_mutex_);
    for (auto it = deferred_gpu_buffers_.begin(); it != deferred_gpu_buffers_.end();) {
        if (is_frame_complete(it->completion_marker)) {
            vmaDestroyBuffer(allocator_, it->buffer, it->allocation);
            it = deferred_gpu_buffers_.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================================
// Textures
// ============================================================================

IGpuTexture::Ptr VkBackend::create_texture(const TextureDesc& desc)
{
    VkFormat vk_format = vk_format_for(desc.format);
    const bool is_color_attachment = (desc.usage == TextureUsage::ColorAttachment);
    const bool is_render_target = (desc.usage == TextureUsage::RenderTarget);
    const bool is_renderable = is_render_target || is_color_attachment;

    uint32_t mip_levels = desc.mip_levels > 0 ? static_cast<uint32_t>(desc.mip_levels) : 1u;

    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = vk_format;
    img_ci.extent = {static_cast<uint32_t>(desc.width), static_cast<uint32_t>(desc.height), 1};
    img_ci.mipLevels = mip_levels;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (mip_levels > 1) {
        img_ci.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (is_renderable) {
        img_ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (desc.usage == TextureUsage::Storage) {
        img_ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateImage(allocator_, &img_ci, &alloc_ci, &image, &allocation, nullptr) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create texture image");
        return {};
    }

    VkImageView view = VK_NULL_HANDLE;
    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = vk_format;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.levelCount = mip_levels;
    view_ci.subresourceRange.layerCount = 1;
    vkCreateImageView(device_, &view_ci, nullptr, &view);

    VkImageLayout initial_layout = (desc.usage == TextureUsage::Storage)
        ? VK_IMAGE_LAYOUT_GENERAL
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    auto cb = begin_one_shot_commands();
    transition_image_layout(cb, image, VK_IMAGE_LAYOUT_UNDEFINED, initial_layout, mip_levels);
    end_one_shot_commands(cb);

    uint32_t bindless_index;
    if (!free_bindless_indices_.empty()) {
        bindless_index = free_bindless_indices_.back();
        free_bindless_indices_.pop_back();
    } else {
        bindless_index = next_bindless_index_++;
    }

    VkDescriptorImageInfo sampler_info{};
    sampler_info.sampler = get_or_create_sampler(desc.sampler);
    sampler_info.imageView = view;
    sampler_info.imageLayout = initial_layout;

    VkWriteDescriptorSet sampler_write{};
    sampler_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    sampler_write.dstSet = descriptor_set_;
    sampler_write.dstBinding = 0;
    sampler_write.dstArrayElement = bindless_index;
    sampler_write.descriptorCount = 1;
    sampler_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler_write.pImageInfo = &sampler_info;
    vkUpdateDescriptorSets(device_, 1, &sampler_write, 0, nullptr);

    if (desc.usage == TextureUsage::Storage) {
        uint32_t storage_binding = 1;
        if (desc.format == PixelFormat::RGBA32F) storage_binding = 2;
        else if (desc.format == PixelFormat::RGBA16F) storage_binding = 3;

        VkDescriptorImageInfo storage_info{};
        storage_info.imageView = view;
        storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet storage_write{};
        storage_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        storage_write.dstSet = descriptor_set_;
        storage_write.dstBinding = storage_binding;
        storage_write.dstArrayElement = bindless_index;
        storage_write.descriptorCount = 1;
        storage_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        storage_write.pImageInfo = &storage_info;
        vkUpdateDescriptorSets(device_, 1, &storage_write, 0, nullptr);
    }

    const uvec2 dims{static_cast<uint32_t>(desc.width), static_cast<uint32_t>(desc.height)};

    if (is_renderable) {
        auto rt = ::velk::instance().create<IGpuTexture>(ClassId::VkRenderTexture);
        if (!rt) return {};
        auto* vk_rt = interface_cast<IVkGpuTexture>(rt.get());
        if (!vk_rt) return {};
        vk_rt->init_render_target(this, image, view, allocation, bindless_index,
                                  mip_levels, initial_layout, dims, desc.format,
                                  desc.sampler);
        if (auto* rt_target = interface_cast<IRenderTarget>(rt.get())) {
            rt_target->set_size(static_cast<uint32_t>(desc.width),
                                static_cast<uint32_t>(desc.height));
            rt_target->set_format(desc.format);
        }
        live_render_targets_.push_back(vk_rt);
        return rt;
    }

    auto t = ::velk::instance().create<IGpuTexture>(ClassId::VkGpuTexture);
    if (!t) return {};
    auto* vk_t = interface_cast<IVkGpuTexture>(t.get());
    if (!vk_t) return {};
    vk_t->init_sampled(this, image, view, allocation, bindless_index, mip_levels,
                       initial_layout, dims, desc.format, desc.sampler);
    return t;
}

void VkBackend::upload_texture(IGpuTexture& texture, const uint8_t* pixels, int width, int height)
{
    auto* vk_t = interface_cast<IVkGpuTexture>(&texture);
    if (!vk_t || vk_t->vk_image() == VK_NULL_HANDLE) return;

    PixelFormat fmt = texture.format();
    size_t bpp = 4;
    if (fmt == PixelFormat::R8) bpp = 1;
    else if (fmt == PixelFormat::RGBA16F) bpp = 8;
    else if (fmt == PixelFormat::RGBA32F) bpp = 16;
    size_t data_size = static_cast<size_t>(width) * height * bpp;

    // Create staging buffer
    VkBufferCreateInfo buf_ci{};
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size = data_size;
    buf_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.flags =
        VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;

    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation staging_alloc = VK_NULL_HANDLE;
    VmaAllocationInfo staging_info{};

    if (vmaCreateBuffer(allocator_, &buf_ci, &alloc_ci, &staging, &staging_alloc, &staging_info) !=
        VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create texture staging buffer");
        return;
    }

    std::memcpy(staging_info.pMappedData, pixels, data_size);

    auto cb = begin_one_shot_commands();

    const uint32_t mip_levels = vk_t->vk_mip_levels();
    const VkImage image = vk_t->vk_image();

    // Transition all mip levels to TRANSFER_DST for the initial upload.
    transition_image_layout(
        cb, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_levels);

    // Upload the source pixels to mip 0.
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    vkCmdCopyBufferToImage(cb, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    if (mip_levels > 1) {
        // Generate the rest of the chain by progressively blitting from
        // level i (TRANSFER_SRC) into level i+1 (TRANSFER_DST). Each
        // level is transitioned to SHADER_READ_ONLY once we're done
        // reading from it, so we end with every mip ready for sampling.
        int32_t mip_w = width;
        int32_t mip_h = height;
        for (uint32_t i = 1; i < mip_levels; ++i) {
            // Source mip (i-1): TRANSFER_DST -> TRANSFER_SRC.
            VkImageMemoryBarrier bar{};
            bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image = image;
            bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bar.subresourceRange.baseMipLevel = i - 1;
            bar.subresourceRange.levelCount = 1;
            bar.subresourceRange.layerCount = 1;
            bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &bar);

            int32_t next_w = mip_w > 1 ? mip_w / 2 : 1;
            int32_t next_h = mip_h > 1 ? mip_h / 2 : 1;
            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = i - 1;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[1] = {mip_w, mip_h, 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = i;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[1] = {next_w, next_h, 1};
            vkCmdBlitImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);

            // Source mip now fully written + read; move to SHADER_READ_ONLY.
            bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &bar);

            mip_w = next_w;
            mip_h = next_h;
        }

        // Final level (mip_levels - 1) is still TRANSFER_DST; flip to
        // SHADER_READ_ONLY to match the rest.
        VkImageMemoryBarrier tail{};
        tail.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        tail.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        tail.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        tail.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        tail.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        tail.image = image;
        tail.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        tail.subresourceRange.baseMipLevel = mip_levels - 1;
        tail.subresourceRange.levelCount = 1;
        tail.subresourceRange.layerCount = 1;
        tail.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        tail.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &tail);
    } else {
        // Single-level image: flip mip 0 back to sampling layout.
        transition_image_layout(
            cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
    }

    end_one_shot_commands(cb);

    vmaDestroyBuffer(allocator_, staging, staging_alloc);
}

bool VkBackend::read_texture(IGpuTexture& texture, vector<uint8_t>& out_pixels,
                             PixelFormat& out_format, uvec2& out_dims)
{
    auto* vk_t = interface_cast<IVkGpuTexture>(&texture);
    if (!vk_t || vk_t->vk_image() == VK_NULL_HANDLE) return false;

    const PixelFormat fmt = texture.format();
    const uvec2 dims = texture.get_dimensions();
    size_t bpp = 4;
    if (fmt == PixelFormat::R8) bpp = 1;
    else if (fmt == PixelFormat::RGBA16F) bpp = 8;
    else if (fmt == PixelFormat::RGBA32F) bpp = 16;
    const size_t data_size = static_cast<size_t>(dims.x) * dims.y * bpp;

    VkBufferCreateInfo buf_ci{};
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size = data_size;
    buf_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.flags =
        VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;

    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation staging_alloc = VK_NULL_HANDLE;
    VmaAllocationInfo staging_info{};
    if (vmaCreateBuffer(allocator_, &buf_ci, &alloc_ci, &staging, &staging_alloc, &staging_info) !=
        VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create texture readback staging buffer");
        return false;
    }

    // Drain prior GPU work so any outstanding writes to this texture are
    // visible to the upcoming transfer. Cheap in a debug-dump context.
    vkQueueWaitIdle(graphics_queue_);

    auto cb = begin_one_shot_commands();

    const VkImage image = vk_t->vk_image();
    const VkImageLayout original_layout = vk_t->vk_current_layout();
    const uint32_t mip_levels = vk_t->vk_mip_levels();

    // Use a permissive image barrier rather than the generic
    // transition_image_layout helper, which doesn't cover the
    // GENERAL <-> TRANSFER_SRC pairs the readback path needs.
    auto image_barrier = [&](VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = mip_levels;
        b.subresourceRange.layerCount = 1;
        b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    image_barrier(original_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {dims.x, dims.y, 1};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);

    image_barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, original_layout);

    end_one_shot_commands(cb);

    out_pixels.resize(data_size);
    std::memcpy(out_pixels.data(), staging_info.pMappedData, data_size);
    out_format = fmt;
    out_dims = dims;

    vmaDestroyBuffer(allocator_, staging, staging_alloc);
    return true;
}

// ============================================================================
// Pipelines
// ============================================================================

IGpuPipeline::Ptr VkBackend::create_pipeline_dynamic(
    const PipelineDesc& desc,
    array_view<const PixelFormat> color_formats,
    DepthFormat depth_format)
{
    VELK_PERF_SCOPE("vk.create_pipeline_dynamic");

    // Resolve color formats and depth format up front. Mirrors
    // create_pipeline's path but without a VkRenderPass — pipelines
    // declare their attachment formats via VkPipelineRenderingCreateInfo
    // pNext'd into VkGraphicsPipelineCreateInfo.
    constexpr size_t kMaxColors = 8;
    if (color_formats.size() > kMaxColors) {
        VELK_LOG(E, "VkBackend: create_pipeline_dynamic: too many color attachments");
        return {};
    }
    VkFormat vk_color_formats[kMaxColors]{};
    for (size_t i = 0; i < color_formats.size(); ++i) {
        vk_color_formats[i] = vk_format_for(color_formats[i]);
        if (vk_color_formats[i] == VK_FORMAT_UNDEFINED) {
            VELK_LOG(E, "VkBackend: create_pipeline_dynamic: unsupported color format");
            return {};
        }
    }
    const VkFormat vk_depth_format = (depth_format == DepthFormat::Default)
                                         ? VK_FORMAT_D32_SFLOAT
                                         : VK_FORMAT_UNDEFINED;
    const uint32_t color_attachment_count = static_cast<uint32_t>(color_formats.size());
    const bool target_has_depth = (vk_depth_format != VK_FORMAT_UNDEFINED);

    // Shader modules
    VkShaderModuleCreateInfo vert_ci{};
    vert_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vert_ci.codeSize = desc.get_vertex_size();
    vert_ci.pCode = desc.get_vertex_data().begin();

    VkShaderModule vert_module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &vert_ci, nullptr, &vert_module) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create vertex shader module");
        return {};
    }

    VkShaderModuleCreateInfo frag_ci{};
    frag_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    frag_ci.codeSize = desc.get_fragment_size();
    frag_ci.pCode = desc.get_fragment_data().begin();

    VkShaderModule frag_module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &frag_ci, nullptr, &frag_module) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        VELK_LOG(E, "VkBackend: failed to create fragment shader module");
        return {};
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_module;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = (desc.options.topology == Topology::TriangleStrip)
        ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    switch (desc.options.cull_mode) {
    case CullMode::None:  rasterizer.cullMode = VK_CULL_MODE_NONE;     break;
    case CullMode::Back:  rasterizer.cullMode = VK_CULL_MODE_BACK_BIT; break;
    case CullMode::Front: rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT; break;
    }
    rasterizer.frontFace = (desc.options.front_face == FrontFace::Clockwise)
        ? VK_FRONT_FACE_CLOCKWISE
        : VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Blend: same policy as legacy create_pipeline — opaque on MRT
    // (color_attachment_count > 1), alpha-honoring on single-color.
    const bool blend_enabled = (color_attachment_count == 1)
        && (desc.options.blend_mode == BlendMode::Alpha);

    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.blendEnable = blend_enabled ? VK_TRUE : VK_FALSE;
    blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.colorBlendOp = VK_BLEND_OP_ADD;
    blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    vector<VkPipelineColorBlendAttachmentState> blend_atts(color_attachment_count, blend_att);

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = color_attachment_count;
    blend.pAttachments = blend_atts.data();

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    if (target_has_depth) {
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = (desc.options.depth_test != CompareOp::Disabled) ? VK_TRUE : VK_FALSE;
        depth_stencil.depthWriteEnable = desc.options.depth_write ? VK_TRUE : VK_FALSE;
        switch (desc.options.depth_test) {
        case CompareOp::Never:        depth_stencil.depthCompareOp = VK_COMPARE_OP_NEVER;            break;
        case CompareOp::Less:         depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;             break;
        case CompareOp::Equal:        depth_stencil.depthCompareOp = VK_COMPARE_OP_EQUAL;            break;
        case CompareOp::LessEqual:    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;    break;
        case CompareOp::Greater:      depth_stencil.depthCompareOp = VK_COMPARE_OP_GREATER;          break;
        case CompareOp::NotEqual:     depth_stencil.depthCompareOp = VK_COMPARE_OP_NOT_EQUAL;        break;
        case CompareOp::GreaterEqual: depth_stencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; break;
        case CompareOp::Always:       depth_stencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;           break;
        case CompareOp::Disabled:     depth_stencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;           break;
        }
        depth_stencil.minDepthBounds = 0.0f;
        depth_stencil.maxDepthBounds = 1.0f;
    }

    // Dynamic-rendering hookup: attachment formats ride in via pNext.
    VkPipelineRenderingCreateInfo rendering_ci{};
    rendering_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_ci.colorAttachmentCount = color_attachment_count;
    rendering_ci.pColorAttachmentFormats = (color_attachment_count > 0) ? vk_color_formats : nullptr;
    rendering_ci.depthAttachmentFormat = vk_depth_format;
    rendering_ci.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext = &rendering_ci;
    pipeline_ci.stageCount = 2;
    pipeline_ci.pStages = stages;
    pipeline_ci.pVertexInputState = &vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.pViewportState = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterizer;
    pipeline_ci.pMultisampleState = &multisample;
    pipeline_ci.pDepthStencilState = target_has_depth ? &depth_stencil : nullptr;
    pipeline_ci.pColorBlendState = &blend;
    pipeline_ci.pDynamicState = &dynamic;
    pipeline_ci.layout = pipeline_layout_;
    pipeline_ci.renderPass = VK_NULL_HANDLE;
    pipeline_ci.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult pipeline_result;
    {
        VELK_PERF_SCOPE("vk.vkCreateGraphicsPipelines");
        pipeline_result = vkCreateGraphicsPipelines(
            device_, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline);
    }
    if (pipeline_result != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create dynamic-rendering pipeline");
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return {};
    }

    vkDestroyShaderModule(device_, vert_module, nullptr);
    vkDestroyShaderModule(device_, frag_module, nullptr);


    auto gp = ::velk::instance().create<::velk::IGpuPipeline>(
        ::velk::ClassId::VkGpuPipeline);
    auto* vk_gp = interface_cast<IVkGpuPipeline>(gp.get());
    if (!vk_gp) {
        vkDestroyPipeline(device_, pipeline, nullptr);
        return {};
    }
    vk_gp->init(this, pipeline, VK_PIPELINE_BIND_POINT_GRAPHICS);
    return gp;
}

IGpuPipeline::Ptr VkBackend::create_compute_pipeline(const ComputePipelineDesc& desc)
{
    if (!desc.compute) {
        VELK_LOG(E, "VkBackend: create_compute_pipeline requires a compute shader");
        return {};
    }
    VELK_PERF_SCOPE("vk.create_compute_pipeline");

    auto code = desc.compute->get_data();
    if (code.empty()) {
        VELK_LOG(E, "VkBackend: create_compute_pipeline got empty SPIR-V");
        return {};
    }

    VkShaderModuleCreateInfo sm_ci{};
    sm_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm_ci.codeSize = desc.compute->get_data_size();
    sm_ci.pCode = code.begin();

    VkShaderModule cs_module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &sm_ci, nullptr, &cs_module) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create compute shader module");
        return {};
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = cs_module;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_ci.stage = stage;
    pipeline_ci.layout = pipeline_layout_;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult pipeline_result;
    {
        VELK_PERF_SCOPE("vk.vkCreateComputePipelines");
        pipeline_result = vkCreateComputePipelines(
            device_, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline);
    }

    vkDestroyShaderModule(device_, cs_module, nullptr);

    if (pipeline_result != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create compute pipeline");
        return {};
    }


    auto gp = ::velk::instance().create<::velk::IGpuPipeline>(
        ::velk::ClassId::VkGpuPipeline);
    auto* vk_gp = interface_cast<IVkGpuPipeline>(gp.get());
    if (!vk_gp) {
        vkDestroyPipeline(device_, pipeline, nullptr);
        return {};
    }
    vk_gp->init(this, pipeline, VK_PIPELINE_BIND_POINT_COMPUTE);
    return gp;
}

void VkBackend::defer_destroy_gpu_pipeline(IGpuPipeline* pipeline)
{
    if (!pipeline) return;
    auto* vk_gp = interface_cast<IVkGpuPipeline>(pipeline);
    if (!vk_gp) return;
    ::VkPipeline vk_pipe = vk_gp->vk_pipeline();
    if (vk_pipe == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(deferred_gpu_pipelines_mutex_);
    deferred_gpu_pipelines_.push_back({vk_pipe, pending_frame_completion_marker()});
}

void VkBackend::drain_deferred_pipelines()
{
    std::lock_guard<std::mutex> lock(deferred_gpu_pipelines_mutex_);
    for (auto it = deferred_gpu_pipelines_.begin(); it != deferred_gpu_pipelines_.end();) {
        if (is_frame_complete(it->completion_marker)) {
            vkDestroyPipeline(device_, it->pipeline, nullptr);
            it = deferred_gpu_pipelines_.erase(it);
        } else {
            ++it;
        }
    }
}

void VkBackend::defer_destroy_gpu_texture(IGpuTexture* texture,
                                          uint64_t completion_marker)
{
    if (!texture) return;
    auto* vk_t = interface_cast<IVkGpuTexture>(texture);
    if (!vk_t) return;
    if (vk_t->vk_image() == VK_NULL_HANDLE) return;
    if (completion_marker == kDefaultCompletionMarker) {
        completion_marker = pending_frame_completion_marker();
    }
    for (auto it = live_render_targets_.begin(); it != live_render_targets_.end(); ++it) {
        if (*it == vk_t) { live_render_targets_.erase(it); break; }
    }
    DeferredGpuTextureDestroy entry{
        vk_t->vk_image(),
        vk_t->vk_view(),
        vk_t->vk_allocation(),
        vk_t->vk_bindless_index(),
        completion_marker,
    };
    std::lock_guard<std::mutex> lock(deferred_gpu_textures_mutex_);
    deferred_gpu_textures_.push_back(entry);
}

IGpuTexture::Ptr VkBackend::create_depth_attachment_texture(
    int width, int height, DepthFormat format)
{
    if (width <= 0 || height <= 0) return {};
    const VkFormat vk_depth_format = (format == DepthFormat::Default)
                                         ? VK_FORMAT_D32_SFLOAT
                                         : VK_FORMAT_UNDEFINED;
    if (vk_depth_format == VK_FORMAT_UNDEFINED) return {};

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vk_depth_format;
    ici.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
              | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateImage(allocator_, &ici, &aci, &image, &allocation, nullptr) != VK_SUCCESS) {
        return {};
    }

    VkImageView view = VK_NULL_HANDLE;
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = vk_depth_format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &vci, nullptr, &view) != VK_SUCCESS) {
        vmaDestroyImage(allocator_, image, allocation);
        return {};
    }

    auto tex = ::velk::instance().create<IGpuTexture>(ClassId::VkGpuTexture);
    auto* vk_t = interface_cast<IVkGpuTexture>(tex.get());
    if (!vk_t) {
        vkDestroyImageView(device_, view, nullptr);
        vmaDestroyImage(allocator_, image, allocation);
        return {};
    }
    // Sentinel bindless index: depth attachments aren't sampled through
    // the bindless heap. Initial layout UNDEFINED; transitioned by
    // record_begin_rendering on first use.
    constexpr uint32_t kNoBindless = UINT32_MAX;
    const uvec2 dims{ static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    vk_t->init_sampled(this, image, view, allocation,
                       kNoBindless, /*mip_levels=*/1,
                       VK_IMAGE_LAYOUT_UNDEFINED, dims,
                       /*format=*/PixelFormat::RGBA8 /*placeholder*/,
                       SamplerDesc{});
    return tex;
}

void VkBackend::drain_deferred_textures()
{
    std::lock_guard<std::mutex> lock(deferred_gpu_textures_mutex_);
    for (auto it = deferred_gpu_textures_.begin(); it != deferred_gpu_textures_.end();) {
        if (is_frame_complete(it->completion_marker)) {
            if (it->view)             vkDestroyImageView(device_, it->view, nullptr);
            if (it->image)            vmaDestroyImage(allocator_, it->image, it->allocation);
            // Recycle the bindless slot now that the GPU is done with it.
            // UINT32_MAX / 0 sentinels (depth attachments, "no texture")
            // never occupied a slot.
            if (it->bindless_index != UINT32_MAX && it->bindless_index != 0) {
                free_bindless_indices_.push_back(it->bindless_index);
            }
            it = deferred_gpu_textures_.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================================
// Frame rendering
// ============================================================================

void VkBackend::begin_frame()
{
    // Advance the recording slot first so prepare(N+1) records into a
    // different slot than submit(N) is consuming on the render thread.
    // The fence wait below gates reuse when the slot is still GPU-busy.
    recording_slot_ = (recording_slot_ + 1) % kFrameOverlap;
    auto& sync = frame_sync_[recording_slot_];
    vkWaitForFences(device_, 1, &sync.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &sync.fence);
    vkResetCommandBuffer(sync.command_buffer, 0);

    // The slot's fence has fired, so the timestamps it wrote last time it
    // recorded are now readable. Resolve them before the pool range is
    // reset + reused below.
    resolve_gpu_timings(recording_slot_);

    // Reset per-slot state captured during recording (formerly VkBackend
    // fields; moved into FrameSync so concurrent submits stay isolated).
    sync.present_surface_id = 0;
    sync.present_acquire_sem_idx = 0;
    sync.pending_buffer_update_barrier = false;

    RENDER_LOG("vk.begin_frame slot=%u primary=%p deferred_frees=%zu",
               recording_slot_, (void*)sync.command_buffer,
               sync.deferred_persistent_frees.size());

    // Drain persistent-pool secondaries deferred when this slot last
    // ran. The slot's fence above guarantees their last submission has
    // completed.
    if (!sync.deferred_persistent_frees.empty()) {
        vkFreeCommandBuffers(device_, persistent_secondary_pool_,
                             static_cast<uint32_t>(sync.deferred_persistent_frees.size()),
                             sync.deferred_persistent_frees.data());
        sync.deferred_persistent_frees.clear();
    }

    drain_deferred_buffers();
    drain_deferred_pipelines();
    drain_deferred_textures();

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(sync.command_buffer, &begin_info);

    // Reset this slot's timestamp range on the primary before any pass
    // records into it, and start a fresh region count for the frame.
    if (gpu_timing_enabled_ && timestamp_pool_) {
        vkCmdResetQueryPool(sync.command_buffer, timestamp_pool_,
                            recording_slot_ * 2 * kMaxTimedPasses,
                            2 * kMaxTimedPasses);
    }
    timer_slots_[recording_slot_].count = 0;
    timer_region_open_ = false;

    cmd_push_label(sync.command_buffer, "Frame");

    // Compute descriptor set: bound here on the primary because
    // `dispatch` records `vkCmd*` inline on the primary (compute is
    // outside any renderpass, so SECONDARY contents doesn't apply).
    // Graphics rebinds happen inside each secondary command buffer
    // (Vulkan spec: secondaries don't inherit descriptor bindings).
    vkCmdBindDescriptorSets(sync.command_buffer,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_,
                            0, 1, &descriptor_set_,
                            0, nullptr);
    // set 1: the frame-invariant bound-buffer set (BVH nodes/shapes).
    // Bound here for compute dispatches recorded inline on the primary;
    // simultaneous-use secondaries bind it themselves. The arena fills its
    // descriptors during prepare; raster never references set 1.
    vkCmdBindDescriptorSets(sync.command_buffer,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_,
                            1, 1, &bound_buffer_set_,
                            0, nullptr);

    frame_open_ = true;
    for (auto* rt : live_render_targets_) rt->mark_cleared_this_frame(false);
    for (auto* g  : live_render_target_groups_) g->mark_cleared_this_frame(false);

    // Bridge to end_frame on the render thread.
    {
        std::lock_guard<std::mutex> lk(in_flight_mutex_);
        in_flight_slots_.push_back(recording_slot_);
    }

    // Surface composites need a fresh "canvas" per frame, but the
    // actual clear is deferred to the first acquire_swapchain_texture
    // call this frame, by which point any pending resize_surface
    // (which recreates the composite) has fired during the renderer's
    // build phase. Clearing in begin_frame would record vkCmd* on the
    // primary referencing a composite VkImage that resize might
    // destroy mid-frame, invalidating the primary cmd buffer.
    for (auto& [surface_id, sd] : surfaces_) {
        sd.composite_acquired_this_frame = false;
    }
}

void VkBackend::record_draw_loop(::VkCommandBuffer cb,
                                 array_view<const DrawCall> calls)
{
    RENDER_LOG("vk.record_draw_loop cb=%p calls=%zu",
               (void*)cb, calls.size());
    for (size_t i = 0; i < calls.size(); ++i) {
        const auto& call = calls[i];

        auto* pipe = interface_cast<IVkGpuPipeline>(call.pipeline);
        if (!pipe) {
            RENDER_LOG("vk.record_draw_loop[%zu] pipeline=%p MISSING",
                       i, (void*)call.pipeline);
            continue;
        }

        RENDER_LOG("vk.record_draw_loop[%zu] vkpipe=%p indexed=%d args=%p+%llu count=%p+%llu rc_size=%u",
                   i, (void*)pipe->vk_pipeline(),
                   call.indexed ? 1 : 0,
                   (void*)call.args_buffer, (unsigned long long)call.args_buffer_offset,
                   (void*)call.count_buffer, (unsigned long long)call.count_buffer_offset,
                   call.root_constants_size);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->vk_pipeline());

        if (call.root_constants_size > 0) {
            // Per-draw push at offset 0: the 8-byte DrawData address
            // (root pointer). The DrawData header carries
            // `globals_addr` so shaders synthesise globals via a
            // buffer-reference cast on `root.globals_addr` — no
            // separate FrameGlobals push needed for graphics.
            vkCmdPushConstants(cb,
                               pipeline_layout_,
                               VK_SHADER_STAGE_ALL,
                               0,
                               call.root_constants_size,
                               call.root_constants);
        }

        auto* args_gb = interface_cast<IVkGpuBuffer>(call.args_buffer);
        auto* count_gb = interface_cast<IVkGpuBuffer>(call.count_buffer);
        if (!args_gb || !count_gb) continue;
        VkBuffer args_vk = args_gb->vk_buffer();
        VkBuffer count_vk = count_gb->vk_buffer();

        if (call.indexed) {
            auto* ibo_gb = interface_cast<IVkGpuBuffer>(call.index_buffer);
            if (!ibo_gb) continue;
            VkBuffer ibo_vk = ibo_gb->vk_buffer();
            vkCmdBindIndexBuffer(cb, ibo_vk,
                                 call.index_buffer_offset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirectCount(cb,
                                          args_vk, call.args_buffer_offset,
                                          count_vk, call.count_buffer_offset,
                                          call.max_draw_count, call.args_stride);
        } else {
            vkCmdDrawIndirectCount(cb,
                                   args_vk, call.args_buffer_offset,
                                   count_vk, call.count_buffer_offset,
                                   call.max_draw_count, call.args_stride);
        }
    }
}

void VkBackend::defer_free_persistent_secondary(::VkCommandBuffer cb)
{
    if (cb == VK_NULL_HANDLE) return;
    // Queue at the slot that was last used (one before current in
    // round-robin). Its next drain happens kFrameOverlap-1 frames from
    // now, by which point every frame that referenced this secondary
    // has been submitted AND its fence signaled. Queueing at the
    // current slot would drain at the upcoming begin_frame, whose
    // fence only guarantees work from kFrameOverlap frames ago — a
    // recently-submitted frame's GPU work referencing the secondary
    // could still be in flight when vkFreeCommandBuffers ran.
    uint32_t target_slot =
        (recording_slot_ + kFrameOverlap - 1) % kFrameOverlap;
    frame_sync_[target_slot].deferred_persistent_frees.push_back(cb);
    RENDER_LOG("vk.defer_free cb=%p current_slot=%u queue_slot=%u",
               (void*)cb, recording_slot_, target_slot);
}

void VkBackend::record_dispatch_call(::VkCommandBuffer cb, const DispatchCall& call)
{
    auto* pipe = interface_cast<IVkGpuPipeline>(call.pipeline);
    if (!pipe) return;
    if (pipe->vk_bind_point() != VK_PIPELINE_BIND_POINT_COMPUTE) return;

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->vk_pipeline());

    if (call.root_constants_size > 0) {
        vkCmdPushConstants(cb,
                           pipeline_layout_,
                           VK_SHADER_STAGE_ALL,
                           0,
                           call.root_constants_size,
                           call.root_constants);
    }

    vkCmdDispatch(cb, call.groups_x, call.groups_y, call.groups_z);
}

void VkBackend::record_blit_to_texture(::VkCommandBuffer cb, IGpuTexture& source,
                                       IGpuTexture& dest, rect dst_rect)
{
    auto* src_vk = interface_cast<IVkGpuTexture>(&source);
    auto* dst_vk = interface_cast<IVkGpuTexture>(&dest);
    if (!src_vk || src_vk->vk_image() == VK_NULL_HANDLE) return;
    if (!dst_vk || dst_vk->vk_image() == VK_NULL_HANDLE) return;

    const VkImage src_image = src_vk->vk_image();
    VkImage dst_image = dst_vk->vk_image();
    int dst_w = 0, dst_h = 0;
    {
        auto d = dest.get_dimensions();
        dst_w = static_cast<int>(d.x);
        dst_h = static_cast<int>(d.y);
    }
    VkImageLayout dst_old_layout = dst_vk->vk_current_layout();
    VkImageLayout dst_final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAccessFlags dst_old_access = (dst_old_layout == VK_IMAGE_LAYOUT_GENERAL)
                                       ? VK_ACCESS_SHADER_WRITE_BIT
                                       : VK_ACCESS_SHADER_READ_BIT;
    dst_vk->set_vk_current_layout(dst_final_layout);

    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = dst_old_layout;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = dst_image;
    to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_dst.subresourceRange.levelCount = 1;
    to_dst.subresourceRange.layerCount = 1;
    to_dst.srcAccessMask = dst_old_access;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    VkImageLayout src_canonical_layout = src_vk->vk_current_layout();
    int src_w = 0, src_h = 0;
    {
        auto d = source.get_dimensions();
        src_w = static_cast<int>(d.x);
        src_h = static_cast<int>(d.y);
    }
    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.oldLayout = src_canonical_layout;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = src_image;
    to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_src.subresourceRange.levelCount = 1;
    to_src.subresourceRange.layerCount = 1;
    to_src.srcAccessMask = (src_canonical_layout == VK_IMAGE_LAYOUT_GENERAL)
                               ? VK_ACCESS_SHADER_WRITE_BIT
                               : VK_ACCESS_SHADER_READ_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkImageMemoryBarrier pre[2] = {to_dst, to_src};
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, pre);

    int32_t dx0 = static_cast<int32_t>(dst_rect.x);
    int32_t dy0 = static_cast<int32_t>(dst_rect.y);
    int32_t dx1 = static_cast<int32_t>(dst_rect.x + dst_rect.width);
    int32_t dy1 = static_cast<int32_t>(dst_rect.y + dst_rect.height);
    if (dst_rect.width <= 0 || dst_rect.height <= 0) {
        dx0 = 0; dy0 = 0; dx1 = dst_w; dy1 = dst_h;
    }

    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1] = {src_w, src_h, 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = {dx0, dy0, 0};
    blit.dstOffsets[1] = {dx1, dy1, 1};

    vkCmdBlitImage(cb,
                   src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_LINEAR);

    VkImageMemoryBarrier to_final{};
    to_final.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_final.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_final.newLayout = dst_final_layout;
    to_final.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_final.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_final.image = dst_image;
    to_final.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_final.subresourceRange.levelCount = 1;
    to_final.subresourceRange.layerCount = 1;
    to_final.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_final.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkImageMemoryBarrier to_general{};
    to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_general.newLayout = src_canonical_layout;
    to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.image = src_image;
    to_general.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_general.subresourceRange.levelCount = 1;
    to_general.subresourceRange.layerCount = 1;
    to_general.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_general.dstAccessMask = 0;

    VkImageMemoryBarrier post[2] = {to_final, to_general};
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                             | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, post);
}

void VkBackend::blit_to_texture(IGpuTexture& source, IGpuTexture& dest, rect dst_rect)
{
    VELK_PERF_SCOPE("vk.blit_to_texture");
    auto& sync = frame_sync_[recording_slot_];
    record_blit_to_texture(sync.command_buffer, source, dest, dst_rect);
}


static VkPipelineStageFlags to_vk_stage(PipelineStage stage)
{
    switch (stage) {
    case PipelineStage::ColorOutput:    return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case PipelineStage::FragmentShader: return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    case PipelineStage::ComputeShader:  return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case PipelineStage::Transfer:       return VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
}

static VkAccessFlags to_vk_access(PipelineStage stage)
{
    switch (stage) {
    case PipelineStage::ColorOutput:    return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case PipelineStage::FragmentShader: return VK_ACCESS_SHADER_READ_BIT;
    case PipelineStage::ComputeShader:  return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    case PipelineStage::Transfer:       return VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
}

void VkBackend::barrier(PipelineStage src, PipelineStage dst)
{
    auto& sync = frame_sync_[recording_slot_];

    VkMemoryBarrier mem_barrier{};
    mem_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mem_barrier.srcAccessMask = to_vk_access(src);
    mem_barrier.dstAccessMask = to_vk_access(dst);

    vkCmdPipelineBarrier(
        sync.command_buffer,
        to_vk_stage(src), to_vk_stage(dst),
        0,
        1, &mem_barrier,
        0, nullptr,
        0, nullptr);
}

::velk::IGpuCommandBuffer::Ptr VkBackend::create_command_buffer()
{
    auto cmd = ::velk::instance().create<::velk::IGpuCommandBuffer>(
        ::velk::ClassId::VkCommandBuffer);
    if (auto* impl = static_cast<VkCommandBuffer*>(cmd.get())) {
        impl->init(this);
    }
    RENDER_LOG("vk.create_command_buffer ptr=%p", (void*)cmd.get());
    return cmd;
}

void VkBackend::execute(const ::velk::IGpuCommandBuffer::Ptr& cmd)
{
    if (!cmd) return;
    auto* impl = static_cast<VkCommandBuffer*>(cmd.get());
    ::VkCommandBuffer secondary = impl->handle();
    if (!secondary) return;
    auto& sync = frame_sync_[recording_slot_];

    // Flush any pending vkCmdUpdateBuffer writes before the secondary
    // reads them (view globals etc).
    if (sync.pending_buffer_update_barrier) {
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(sync.command_buffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT
                                 | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
        sync.pending_buffer_update_barrier = false;
    }

    RENDER_LOG("vk.execute primary=%p secondary=%p",
               (void*)sync.command_buffer, (void*)secondary);
    vkCmdExecuteCommands(sync.command_buffer, 1, &secondary);
}

void VkBackend::begin_gpu_timer(const char* label)
{
    if (!gpu_timing_enabled_ || !timestamp_pool_) return;
    auto& slot = timer_slots_[recording_slot_];
    if (slot.count >= kMaxTimedPasses) {
        // Region budget exhausted this frame; drop the pair so the
        // matching end_gpu_timer is a no-op too.
        timer_region_open_ = false;
        return;
    }
    slot.labels[slot.count] = label ? label : "";
    uint32_t base = recording_slot_ * 2 * kMaxTimedPasses + 2 * slot.count;
    vkCmdWriteTimestamp(frame_sync_[recording_slot_].command_buffer,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        timestamp_pool_, base);
    timer_region_open_ = true;
}

void VkBackend::end_gpu_timer()
{
    if (!gpu_timing_enabled_ || !timestamp_pool_ || !timer_region_open_) return;
    auto& slot = timer_slots_[recording_slot_];
    uint32_t base = recording_slot_ * 2 * kMaxTimedPasses + 2 * slot.count + 1;
    vkCmdWriteTimestamp(frame_sync_[recording_slot_].command_buffer,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        timestamp_pool_, base);
    ++slot.count;
    timer_region_open_ = false;
}

void VkBackend::resolve_gpu_timings(uint32_t slot)
{
    if (!gpu_timing_enabled_ || !timestamp_pool_ || timestamp_period_ns_ <= 0.f) return;
    const TimerSlot& ts = timer_slots_[slot];
    if (ts.count == 0) return;

    uint64_t stamps[2 * kMaxTimedPasses];
    uint32_t n = ts.count;
    VkResult r = vkGetQueryPoolResults(
        device_, timestamp_pool_, slot * 2 * kMaxTimedPasses, 2 * n,
        sizeof(uint64_t) * 2 * n, stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) return; // VK_NOT_READY should not happen post-fence.

    last_timings_.clear();
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t begin = stamps[2 * i];
        uint64_t end = stamps[2 * i + 1];
        // Guard against wraparound on the (unlikely) decreasing pair.
        float ms = end > begin
            ? static_cast<float>(static_cast<double>(end - begin) * timestamp_period_ns_ * 1e-6)
            : 0.f;
        const char* lbl = ts.labels[i] ? ts.labels[i] : "";
        bool merged = false;
        for (auto& t : last_timings_) {
            if (std::strcmp(t.label, lbl) == 0) { t.ms += ms; merged = true; break; }
        }
        if (!merged) last_timings_.push_back({lbl, ms});
    }
}

bool VkBackend::record_present_blit(FrameSync& sync)
{
    if (sync.present_swapchain == VK_NULL_HANDLE
        || sync.present_composite_image == VK_NULL_HANDLE) {
        return false;
    }

    // Acquire on the render thread so acquire + present share one thread; the
    // swapchain is therefore never used from two threads concurrently.
    sync.present_acquire_sem_idx = acquire_semaphore_index_;
    VkSemaphore acquire_sem = image_available_[acquire_semaphore_index_];
    acquire_semaphore_index_ = (acquire_semaphore_index_ + 1) % kMaxSwapchainImages;
    VkResult r = vkAcquireNextImageKHR(
        device_, sync.present_swapchain, UINT64_MAX, acquire_sem, VK_NULL_HANDLE,
        &sync.present_image_index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || sync.present_image_index >= sync.present_image_count) {
        return false;
    }

    const VkImage src_image = sync.present_composite_image;
    const VkImageLayout src_canonical_layout = sync.present_composite_layout;
    const VkImage dst_image = sync.present_images[sync.present_image_index];

    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = dst_image;
    to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_dst.subresourceRange.levelCount = 1;
    to_dst.subresourceRange.layerCount = 1;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.oldLayout = src_canonical_layout;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = src_image;
    to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_src.subresourceRange.levelCount = 1;
    to_src.subresourceRange.layerCount = 1;
    to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_SHADER_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkImageMemoryBarrier pre[2] = {to_dst, to_src};
    vkCmdPipelineBarrier(sync.command_buffer,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                             | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, pre);

    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1] = {sync.present_width, sync.present_height, 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[1] = {sync.present_width, sync.present_height, 1};
    vkCmdBlitImage(sync.command_buffer,
                   src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_LINEAR);

    // Swap → PRESENT_SRC; composite → SHADER_READ_ONLY for next-frame
    // sampling (not strictly needed today; avoid surprising consumers).
    VkImageMemoryBarrier to_present{};
    to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = dst_image;
    to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_present.subresourceRange.levelCount = 1;
    to_present.subresourceRange.layerCount = 1;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    VkImageMemoryBarrier to_sampled{};
    to_sampled.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_sampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_sampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_sampled.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_sampled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_sampled.image = src_image;
    to_sampled.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_sampled.subresourceRange.levelCount = 1;
    to_sampled.subresourceRange.layerCount = 1;
    to_sampled.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_sampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkImageMemoryBarrier post[2] = {to_present, to_sampled};
    vkCmdPipelineBarrier(sync.command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                             | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, post);
    // Composite's tracked layout was already advanced to SHADER_READ_ONLY by
    // close_frame (main thread); the blit is layout-neutral so nothing to
    // write back here.
    return true;
}

void VkBackend::close_frame()
{
    // End of recording on the prepare / main thread. The scene is recorded into
    // the primary; the composite-to-swap present blit is NOT recorded here.
    // Instead we snapshot the present target (composite source + swapchain +
    // dims) into the slot, and submit_frame (render thread) does the acquire +
    // blit + present. That keeps swapchain acquire and present on one thread
    // (the swapchain must not be used from two threads) and keeps the blit's
    // composite-layout read off the live value that the next prepare mutates.
    auto& sync = frame_sync_[recording_slot_];

    sync.present_surface_id = 0;
    sync.present_swapchain = VK_NULL_HANDLE;
    sync.present_composite_image = VK_NULL_HANDLE;
    sync.present_image_count = 0;

    // Single-surface present (matches prior behavior): pick the first surface
    // rendered this frame.
    for (auto& [surface_id, sd] : surfaces_) {
        if (!sd.composite_acquired_this_frame || !sd.swapchain) continue;
        auto* surf_tex = interface_cast<IVkGpuTexture>(sd.composite.get());
        if (!surf_tex || surf_tex->vk_image() == VK_NULL_HANDLE) continue;

        sync.present_surface_id = surface_id;
        sync.present_swapchain = sd.swapchain;
        sync.present_composite_image = surf_tex->vk_image();
        sync.present_composite_layout = surf_tex->vk_current_layout();
        sync.present_width = sd.width;
        sync.present_height = sd.height;
        const uint32_t n = (sd.images.size() < kMaxSwapchainImages)
                               ? static_cast<uint32_t>(sd.images.size())
                               : kMaxSwapchainImages;
        for (uint32_t i = 0; i < n; ++i) sync.present_images[i] = sd.images[i];
        sync.present_image_count = n;

        // The present blit is layout-neutral (composite returns to
        // SHADER_READ_ONLY); advance the tracked layout here on the main thread
        // so the render thread never writes the shared composite state.
        surf_tex->set_vk_current_layout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        break;
    }

    // Primary is left open; submit_frame appends the present blit then ends it.
    frame_open_ = false;
}

void VkBackend::submit_frame()
{
    // Pop the FIFO slot pushed by begin_frame. Runs on the render thread; it
    // appends the present blit (acquire + composite->swap), finalizes the
    // primary, then submits + presents. Acquire and present both happen here so
    // the swapchain is never touched from two threads. All present inputs come
    // from the per-slot snapshot taken at close_frame, not shared SurfaceData.
    uint32_t slot;
    {
        std::lock_guard<std::mutex> lk(in_flight_mutex_);
        if (in_flight_slots_.empty()) {
            return;  // unmatched submit_frame — shouldn't happen in normal flow
        }
        slot = in_flight_slots_.front();
        in_flight_slots_.pop_front();
    }
    auto& sync = frame_sync_[slot];

    bool present_ok = false;
    if (sync.present_swapchain != VK_NULL_HANDLE) {
        cmd_push_label(sync.command_buffer, "Composite -> Swap");
        present_ok = record_present_blit(sync);
        cmd_pop_label(sync.command_buffer);
    }

    cmd_pop_label(sync.command_buffer);  // "Frame"
    vkEndCommandBuffer(sync.command_buffer);

    // Allocate this submit's timeline value. The signal value rides
    // along on every queue submit below; renderer pulls it via
    // frame_completion_marker() right after this returns.
    const uint64_t timeline_value = next_frame_value_.fetch_add(1);

    if (present_ok) {
        // Surface was used: submit with swapchain synchronization, reading
        // only the per-slot snapshot taken at close_frame.
        VkSemaphore wait_sem = image_available_[sync.present_acquire_sem_idx];
        VkSemaphore signal_sems[2] = { render_finished_[sync.present_image_index], frame_timeline_ };
        // Binary semaphore values are ignored; index 1 is the timeline value.
        uint64_t signal_values[2] = { 0, timeline_value };

        VkTimelineSemaphoreSubmitInfo timeline_info{};
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline_info.signalSemaphoreValueCount = 2;
        timeline_info.pSignalSemaphoreValues = signal_values;

        // Cover both the raster path (COLOR_ATTACHMENT_OUTPUT) and the
        // RT blit path (TRANSFER) on acquire-semaphore wait.
        VkPipelineStageFlags wait_stage =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.pNext = &timeline_info;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &wait_sem;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &sync.command_buffer;
        submit_info.signalSemaphoreCount = 2;
        submit_info.pSignalSemaphores = signal_sems;

        vkQueueSubmit(graphics_queue_, 1, &submit_info, sync.fence);

        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &signal_sems[0];
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &sync.present_swapchain;
        present_info.pImageIndices = &sync.present_image_index;

        vkQueuePresentKHR(graphics_queue_, &present_info);
    } else {
        // Headless: submit without swapchain synchronization
        uint64_t signal_value = timeline_value;
        VkTimelineSemaphoreSubmitInfo timeline_info{};
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline_info.signalSemaphoreValueCount = 1;
        timeline_info.pSignalSemaphoreValues = &signal_value;

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.pNext = &timeline_info;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &sync.command_buffer;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &frame_timeline_;

        vkQueueSubmit(graphics_queue_, 1, &submit_info, sync.fence);
    }

    last_frame_value_.store(timeline_value);
    // No recording_slot_ advance here; begin_frame advances on the main thread.
}

uint64_t VkBackend::frame_completion_marker() const
{
    return last_frame_value_.load();
}

void VkBackend::wait_for_frame_completion(uint64_t marker)
{
    if (marker == 0 || !device_ || !frame_timeline_) return;

    VkSemaphoreWaitInfo wait_info{};
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &frame_timeline_;
    wait_info.pValues = &marker;

    vkWaitSemaphores(device_, &wait_info, UINT64_MAX);
}

uint64_t VkBackend::pending_frame_completion_marker() const
{
    return next_frame_value_.load();
}

bool VkBackend::is_frame_complete(uint64_t marker) const
{
    if (marker == 0 || !device_ || !frame_timeline_) return true;
    uint64_t value = 0;
    if (vkGetSemaphoreCounterValue(device_, frame_timeline_, &value) != VK_SUCCESS) {
        return false;
    }
    return value >= marker;
}

// ============================================================================
// Utility
// ============================================================================

::VkCommandBuffer VkBackend::begin_one_shot_commands()
{
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    ::VkCommandBuffer cb;
    vkAllocateCommandBuffers(device_, &alloc_info, &cb);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin_info);

    return cb;
}

void VkBackend::end_one_shot_commands(::VkCommandBuffer cb)
{
    vkEndCommandBuffer(cb);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;

    vkQueueSubmit(graphics_queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);

    vkFreeCommandBuffers(device_, command_pool_, 1, &cb);
}

void VkBackend::transition_image_layout(::VkCommandBuffer cb, VkImage image, VkImageLayout old_layout,
                                        VkImageLayout new_layout, uint32_t mip_levels)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = mip_levels;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrier.srcAccessMask = 0;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }

    if (new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

IRenderTextureGroup::Ptr VkBackend::create_render_target_group(const TextureGroupDesc& desc)
{
    const auto& formats = desc.formats;
    int width = desc.width;
    int height = desc.height;
    DepthFormat depth = desc.depth;

    if (formats.size() == 0 || width <= 0 || height <= 0) {
        return {};
    }

    const VkFormat depth_vk_format = (depth == DepthFormat::Default)
                                         ? VK_FORMAT_D32_SFLOAT
                                         : VK_FORMAT_UNDEFINED;
    vector<IGpuTexture::Ptr> attachments;
    attachments.reserve(formats.size());

    // Create each attachment as a renderable+sampleable texture using
    // ColorAttachment, which preserves the declared format.
    for (auto f : formats) {
        TextureDesc td{};
        td.width = width;
        td.height = height;
        td.format = f;
        td.usage = TextureUsage::ColorAttachment;
        auto t = create_texture(td);
        if (!t) {
            VELK_LOG(E, "VkBackend: create_render_target_group: attachment create failed");
            return {};
        }
        attachments.push_back(std::move(t));
    }

    const bool has_depth = depth_vk_format != VK_FORMAT_UNDEFINED;
    VkImage depth_image = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VmaAllocation depth_allocation = VK_NULL_HANDLE;
    if (has_depth) {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = depth_vk_format;
        ici.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        vmaCreateImage(allocator_, &ici, &aci, &depth_image, &depth_allocation, nullptr);

        VkImageViewCreateInfo dv_ci{};
        dv_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        dv_ci.image = depth_image;
        dv_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        dv_ci.format = depth_vk_format;
        dv_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        dv_ci.subresourceRange.levelCount = 1;
        dv_ci.subresourceRange.layerCount = 1;
        vkCreateImageView(device_, &dv_ci, nullptr, &depth_view);
    }

    // Rendering goes through dynamic rendering (record_begin_rendering),
    // so the group needs no VkRenderPass / VkFramebuffer. Attachment
    // formats already live on the individual attachment textures.
    auto cleanup_on_fail = [&]() {
        if (depth_view)  vkDestroyImageView(device_, depth_view, nullptr);
        if (depth_image) vmaDestroyImage(allocator_, depth_image, depth_allocation);
    };

    auto group = ::velk::instance().create<IRenderTextureGroup>(ClassId::VkRenderTargetGroup);
    if (!group) {
        cleanup_on_fail();
        return {};
    }
    auto* vk_group = interface_cast<IVkRenderTargetGroup>(group.get());
    if (!vk_group) {
        cleanup_on_fail();
        return {};
    }
    const uvec2 dims{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    // Wrap the depth resources as a real IGpuTexture::Ptr so the group
    // can expose `depth_attachment()` and the depth lifecycle flows
    // through the standard texture observer chain when the group's
    // last Ptr drops.
    IGpuTexture::Ptr depth_tex{};
    if (has_depth) {
        depth_tex = ::velk::instance().create<IGpuTexture>(ClassId::VkGpuTexture);
        if (auto* vk_depth = interface_cast<IVkGpuTexture>(depth_tex.get())) {
            // Sentinel bindless index: depth attachments aren't sampled
            // through the bindless heap (no descriptor write); reserved
            // index value flags "not in heap". Initial layout UNDEFINED;
            // record_begin_rendering / legacy begin_pass transition on
            // first use.
            constexpr uint32_t kNoBindless = UINT32_MAX;
            vk_depth->init_sampled(this, depth_image, depth_view, depth_allocation,
                                   kNoBindless, /*mip_levels=*/1,
                                   VK_IMAGE_LAYOUT_UNDEFINED, dims,
                                   /*format=*/PixelFormat::RGBA8 /*placeholder*/,
                                   SamplerDesc{});
            // Hand-off succeeded — backend doesn't track these handles
            // separately anymore; the texture wrapper owns them.
            depth_image = VK_NULL_HANDLE;
            depth_view = VK_NULL_HANDLE;
            depth_allocation = VK_NULL_HANDLE;
        } else {
            cleanup_on_fail();
            return {};
        }
    }

    vk_group->init(this, std::move(attachments), std::move(depth_tex),
                   depth_vk_format, dims, formats[0], depth);
    live_render_target_groups_.push_back(vk_group);
    return group;
}

void VkBackend::defer_destroy_gpu_render_target_group(IRenderTextureGroup* group,
                                                      uint64_t /*completion_marker*/)
{
    // The group owns no GPU resources of its own — color/depth attachments
    // are IGpuTexture::Ptrs that defer their own destruction, and rendering
    // uses dynamic rendering (no render pass / framebuffer). So there is
    // nothing to fence; this only unregisters the group from the
    // begin_frame cleared-flag walk.
    if (!group) return;
    auto* vk_group = interface_cast<IVkRenderTargetGroup>(group);
    if (!vk_group) return;
    for (auto it = live_render_target_groups_.begin();
         it != live_render_target_groups_.end(); ++it) {
        if (*it == vk_group) { live_render_target_groups_.erase(it); break; }
    }
}

} // namespace velk::vk
