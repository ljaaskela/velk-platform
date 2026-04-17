#include "vk_backend.h"

#include <velk/api/perf.h>
#include <velk/api/velk.h>

#include <cstring>

namespace velk::vk {

namespace {

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
    if (!create_pipeline_layout()) {
        return false;
    }

    // Create a default render pass from the initial surface format.
    // This is needed so pipelines can be created before a swapchain exists.
    if (initial_surface) {
        default_surface_format_ = choose_surface_format(physical_device_, initial_surface);
        VkAttachmentDescription color_att{};
        color_att.format = default_surface_format_;
        color_att.samples = VK_SAMPLE_COUNT_1_BIT;
        color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color_att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference color_ref{};
        color_ref.attachment = 0;
        color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rp_ci{};
        rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp_ci.attachmentCount = 1;
        rp_ci.pAttachments = &color_att;
        rp_ci.subpassCount = 1;
        rp_ci.pSubpasses = &subpass;
        rp_ci.dependencyCount = 1;
        rp_ci.pDependencies = &dep;

        if (vkCreateRenderPass(device_, &rp_ci, nullptr, &default_render_pass_) != VK_SUCCESS) {
            VELK_LOG(E, "VkBackend: failed to create default render pass");
            return false;
        }
    }

    initialized_ = true;
    VELK_LOG(I, "VkBackend: initialized (Vulkan 1.2, BDA + bindless)");
    return true;
}

void VkBackend::shutdown()
{
    if (!initialized_) {
        return;
    }

    vkDeviceWaitIdle(device_);

    // Destroy pipelines
    for (auto& [id, pd] : pipelines_) {
        vkDestroyPipeline(device_, pd.pipeline, nullptr);
    }
    pipelines_.clear();

    // Destroy textures
    for (auto& [id, td] : textures_) {
        if (td.is_renderable) {
            vkDestroyFramebuffer(device_, td.framebuffer, nullptr);
            vkDestroyRenderPass(device_, td.render_pass, nullptr);
            vkDestroyRenderPass(device_, td.load_render_pass, nullptr);
        }
        vkDestroyImageView(device_, td.view, nullptr);
        vmaDestroyImage(allocator_, td.image, td.allocation);
    }
    textures_.clear();

    // Destroy buffers
    for (auto& [id, bd] : buffers_) {
        vmaDestroyBuffer(allocator_, bd.buffer, bd.allocation);
    }
    buffers_.clear();

    // Destroy surfaces
    for (auto& [id, sd] : surfaces_) {
        destroy_swapchain(sd);
        if (sd.surface) {
            vkDestroySurfaceKHR(instance_, sd.surface, nullptr);
        }
    }
    surfaces_.clear();

    if (default_render_pass_) {
        vkDestroyRenderPass(device_, default_render_pass_, nullptr);
    }
    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    vkDestroySampler(device_, linear_sampler_, nullptr);

    for (uint32_t i = 0; i < kFrameOverlap; ++i) {
        vkDestroyFence(device_, frame_sync_[i].fence, nullptr);
    }
    for (uint32_t i = 0; i < kMaxSwapchainImages; ++i) {
        vkDestroySemaphore(device_, image_available_[i], nullptr);
        vkDestroySemaphore(device_, render_finished_[i], nullptr);
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
    app_info.apiVersion = VK_API_VERSION_1_2;

    vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef _WIN32
        "VK_KHR_win32_surface",
#endif
    };

    vector<const char*> layers;
#ifndef NDEBUG
    layers.push_back("VK_LAYER_KHRONOS_validation");
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app_info;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();

    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create VkInstance");
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
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;

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
    ci.vulkanApiVersion = VK_API_VERSION_1_2;
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

    if (vkCreateCommandPool(device_, &ci, nullptr, &command_pool_) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = kFrameOverlap;

    VkCommandBuffer cbs[kFrameOverlap];
    if (vkAllocateCommandBuffers(device_, &alloc_info, cbs) != VK_SUCCESS) {
        return false;
    }
    for (uint32_t i = 0; i < kFrameOverlap; ++i) {
        frame_sync_[i].command_buffer = cbs[i];
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
    return true;
}

bool VkBackend::create_bindless_descriptor()
{
    // Sampler
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(device_, &sampler_ci, nullptr, &linear_sampler_) != VK_SUCCESS) {
        return false;
    }

    // Descriptor set layout: two bindings
    //   0: variable-length sampler array (combined image+sampler) for sampled reads
    //   1: variable-length storage-image array for compute imageStore writes
    // Only the LAST binding may use VARIABLE_DESCRIPTOR_COUNT, so binding 1
    // carries that flag; binding 0 uses a fixed count.
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = kMaxBindlessTextures;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = kMaxBindlessTextures;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorBindingFlags binding_flags[2] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
    };

    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci{};
    flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_ci.bindingCount = 2;
    flags_ci.pBindingFlags = binding_flags;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.pNext = &flags_ci;
    layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_ci.bindingCount = 2;
    layout_ci.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &descriptor_layout_) != VK_SUCCESS) {
        return false;
    }

    // Pool
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = kMaxBindlessTextures;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[1].descriptorCount = kMaxBindlessTextures;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 2;
    pool_ci.pPoolSizes = pool_sizes;

    if (vkCreateDescriptorPool(device_, &pool_ci, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        return false;
    }

    // Allocate set
    uint32_t variable_count = kMaxBindlessTextures;
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable_ci{};
    variable_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    variable_ci.descriptorSetCount = 1;
    variable_ci.pDescriptorCounts = &variable_count;

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.pNext = &variable_ci;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &descriptor_layout_;

    return vkAllocateDescriptorSets(device_, &alloc_info, &descriptor_set_) == VK_SUCCESS;
}

bool VkBackend::create_pipeline_layout()
{
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_ALL;
    push_range.offset = 0;
    push_range.size = kMaxRootConstantsSize;

    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &descriptor_layout_;
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
            if (!create_swapchain(sd)) {
                return 0;
            }
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
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = present_mode;
    ci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &ci, nullptr, &sd.swapchain) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create swapchain");
        return false;
    }

    // Get images
    uint32_t img_count = 0;
    vkGetSwapchainImagesKHR(device_, sd.swapchain, &img_count, nullptr);
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

    // Render pass
    VkAttachmentDescription color_att{};
    color_att.format = sd.image_format;
    color_att.samples = VK_SAMPLE_COUNT_1_BIT;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_ci{};
    rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = 1;
    rp_ci.pAttachments = &color_att;
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &subpass;
    rp_ci.dependencyCount = 1;
    rp_ci.pDependencies = &dep;

    vkCreateRenderPass(device_, &rp_ci, nullptr, &sd.render_pass);

    // Load render pass (for subsequent passes on the same surface within a frame)
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color_att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCreateRenderPass(device_, &rp_ci, nullptr, &sd.load_render_pass);

    // Framebuffers
    sd.framebuffers.resize(img_count);
    for (uint32_t i = 0; i < img_count; ++i) {
        VkFramebufferCreateInfo fb_ci{};
        fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = sd.render_pass;
        fb_ci.attachmentCount = 1;
        fb_ci.pAttachments = &sd.image_views[i];
        fb_ci.width = extent.width;
        fb_ci.height = extent.height;
        fb_ci.layers = 1;

        vkCreateFramebuffer(device_, &fb_ci, nullptr, &sd.framebuffers[i]);
    }

    return true;
}

void VkBackend::destroy_swapchain(SurfaceData& sd)
{
    for (auto fb : sd.framebuffers) {
        vkDestroyFramebuffer(device_, fb, nullptr);
    }
    for (auto iv : sd.image_views) {
        vkDestroyImageView(device_, iv, nullptr);
    }
    if (sd.render_pass) {
        vkDestroyRenderPass(device_, sd.render_pass, nullptr);
    }
    if (sd.load_render_pass) {
        vkDestroyRenderPass(device_, sd.load_render_pass, nullptr);
    }
    if (sd.swapchain) {
        vkDestroySwapchainKHR(device_, sd.swapchain, nullptr);
    }

    sd.framebuffers.clear();
    sd.image_views.clear();
    sd.images.clear();
    sd.render_pass = VK_NULL_HANDLE;
    sd.load_render_pass = VK_NULL_HANDLE;
    sd.swapchain = VK_NULL_HANDLE;
}

// ============================================================================
// GPU Memory
// ============================================================================

GpuBuffer VkBackend::create_buffer(const GpuBufferDesc& desc)
{
    VkBufferCreateInfo buf_ci{};
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size = desc.size;
    buf_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo alloc_ci{};
    if (desc.cpu_writable) {
        alloc_ci.flags =
            VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    } else {
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }

    BufferData bd{};
    bd.size = desc.size;

    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator_, &buf_ci, &alloc_ci, &bd.buffer, &bd.allocation, &info) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create buffer (%zu bytes)", desc.size);
        return 0;
    }

    bd.mapped = info.pMappedData;

    GpuBuffer id = next_buffer_id_++;
    buffers_[id] = bd;
    return id;
}

void VkBackend::destroy_buffer(GpuBuffer buffer)
{
    auto it = buffers_.find(buffer);
    if (it == buffers_.end()) {
        return;
    }

    vmaDestroyBuffer(allocator_, it->second.buffer, it->second.allocation);
    buffers_.erase(it);
}

void* VkBackend::map(GpuBuffer buffer)
{
    auto it = buffers_.find(buffer);
    if (it == buffers_.end()) {
        return nullptr;
    }
    return it->second.mapped;
}

uint64_t VkBackend::gpu_address(GpuBuffer buffer)
{
    auto it = buffers_.find(buffer);
    if (it == buffers_.end()) {
        return 0;
    }

    VkBufferDeviceAddressInfo addr_info{};
    addr_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addr_info.buffer = it->second.buffer;

    return vkGetBufferDeviceAddress(device_, &addr_info);
}

// ============================================================================
// Textures
// ============================================================================

TextureId VkBackend::create_texture(const TextureDesc& desc)
{
    TextureData td{};
    td.width = desc.width;
    td.height = desc.height;
    td.format = desc.format;
    td.bindless_index = next_bindless_index_++;

    VkFormat vk_format = VK_FORMAT_R8G8B8A8_UNORM;
    if (desc.usage == TextureUsage::RenderTarget && default_surface_format_ != VK_FORMAT_UNDEFINED) {
        // Render targets must use the same format as the surface so pipelines are compatible
        vk_format = default_surface_format_;
    } else {
        switch (desc.format) {
            case PixelFormat::R8:         vk_format = VK_FORMAT_R8_UNORM; break;
            case PixelFormat::RGBA8:      vk_format = VK_FORMAT_R8G8B8A8_UNORM; break;
            case PixelFormat::RGBA8_SRGB: vk_format = VK_FORMAT_R8G8B8A8_SRGB; break;
            case PixelFormat::RGBA16F:    vk_format = VK_FORMAT_R16G16B16A16_SFLOAT; break;
        }
    }

    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = vk_format;
    img_ci.extent = {static_cast<uint32_t>(desc.width), static_cast<uint32_t>(desc.height), 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc.usage == TextureUsage::RenderTarget) {
        img_ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        td.is_renderable = true;
    }
    if (desc.usage == TextureUsage::Storage) {
        // TRANSFER_SRC_BIT allows this texture to be blitted onto a surface
        // by blit_to_surface (the compute-RT output path).
        img_ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocator_, &img_ci, &alloc_ci, &td.image, &td.allocation, nullptr) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create texture image");
        return 0;
    }

    // Image view
    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = td.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = vk_format;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.levelCount = 1;
    view_ci.subresourceRange.layerCount = 1;

    vkCreateImageView(device_, &view_ci, nullptr, &td.view);

    // Initial layout: GENERAL for storage textures (compute writes them in
    // that layout and the bindless sampler can still read them there);
    // SHADER_READ_ONLY_OPTIMAL for everything else.
    VkImageLayout initial_layout = (desc.usage == TextureUsage::Storage)
        ? VK_IMAGE_LAYOUT_GENERAL
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    auto cb = begin_one_shot_commands();
    transition_image_layout(cb, td.image, VK_IMAGE_LAYOUT_UNDEFINED, initial_layout);
    end_one_shot_commands(cb);

    // Update bindless sampler descriptor (binding 0)
    VkDescriptorImageInfo sampler_info{};
    sampler_info.sampler = linear_sampler_;
    sampler_info.imageView = td.view;
    sampler_info.imageLayout = initial_layout;

    VkWriteDescriptorSet sampler_write{};
    sampler_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    sampler_write.dstSet = descriptor_set_;
    sampler_write.dstBinding = 0;
    sampler_write.dstArrayElement = td.bindless_index;
    sampler_write.descriptorCount = 1;
    sampler_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler_write.pImageInfo = &sampler_info;

    vkUpdateDescriptorSets(device_, 1, &sampler_write, 0, nullptr);

    // For storage textures, also register as a storage image (binding 1) so
    // compute shaders can imageStore via the same bindless_index.
    if (desc.usage == TextureUsage::Storage) {
        VkDescriptorImageInfo storage_info{};
        storage_info.imageView = td.view;
        storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet storage_write{};
        storage_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        storage_write.dstSet = descriptor_set_;
        storage_write.dstBinding = 1;
        storage_write.dstArrayElement = td.bindless_index;
        storage_write.descriptorCount = 1;
        storage_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        storage_write.pImageInfo = &storage_info;

        vkUpdateDescriptorSets(device_, 1, &storage_write, 0, nullptr);
    }

    // Create render pass and framebuffer for render target textures
    if (td.is_renderable) {
        VkAttachmentDescription color_att{};
        color_att.format = vk_format;
        color_att.samples = VK_SAMPLE_COUNT_1_BIT;
        color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color_att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference color_ref{};
        color_ref.attachment = 0;
        color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rp_ci{};
        rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp_ci.attachmentCount = 1;
        rp_ci.pAttachments = &color_att;
        rp_ci.subpassCount = 1;
        rp_ci.pSubpasses = &subpass;
        rp_ci.dependencyCount = 1;
        rp_ci.pDependencies = &dep;

        vkCreateRenderPass(device_, &rp_ci, nullptr, &td.render_pass);

        // Load render pass (subsequent passes on the same texture within a frame)
        color_att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color_att.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCreateRenderPass(device_, &rp_ci, nullptr, &td.load_render_pass);

        // Framebuffer
        VkFramebufferCreateInfo fb_ci{};
        fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = td.render_pass;
        fb_ci.attachmentCount = 1;
        fb_ci.pAttachments = &td.view;
        fb_ci.width = static_cast<uint32_t>(desc.width);
        fb_ci.height = static_cast<uint32_t>(desc.height);
        fb_ci.layers = 1;

        vkCreateFramebuffer(device_, &fb_ci, nullptr, &td.framebuffer);
    }

    TextureId id = td.bindless_index;
    textures_[id] = td;
    return id;
}

void VkBackend::destroy_texture(TextureId texture)
{
    auto it = textures_.find(texture);
    if (it == textures_.end()) {
        return;
    }

    auto& td = it->second;
    if (td.is_renderable) {
        vkDestroyFramebuffer(device_, td.framebuffer, nullptr);
        vkDestroyRenderPass(device_, td.render_pass, nullptr);
        vkDestroyRenderPass(device_, td.load_render_pass, nullptr);
    }
    vkDestroyImageView(device_, td.view, nullptr);
    vmaDestroyImage(allocator_, td.image, td.allocation);
    textures_.erase(it);
}

void VkBackend::upload_texture(TextureId texture, const uint8_t* pixels, int width, int height)
{
    auto it = textures_.find(texture);
    if (it == textures_.end()) {
        return;
    }

    auto& td = it->second;
    size_t bpp = 4;
    if (td.format == PixelFormat::R8) bpp = 1;
    else if (td.format == PixelFormat::RGBA16F) bpp = 8;
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

    // Transition to transfer dst
    transition_image_layout(
        cb, td.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // Copy
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};

    vkCmdCopyBufferToImage(cb, staging, td.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition back to shader read
    transition_image_layout(
        cb, td.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    end_one_shot_commands(cb);

    vmaDestroyBuffer(allocator_, staging, staging_alloc);
}

// ============================================================================
// Pipelines
// ============================================================================

PipelineId VkBackend::create_pipeline(const PipelineDesc& desc)
{
    if (!default_render_pass_) {
        VELK_LOG(E, "VkBackend: cannot create pipeline without a render pass");
        return 0;
    }
    VELK_PERF_SCOPE("vk.create_pipeline");
    VkRenderPass render_pass = default_render_pass_;

    // Shader modules
    VkShaderModuleCreateInfo vert_ci{};
    vert_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vert_ci.codeSize = desc.get_vertex_size();
    vert_ci.pCode = desc.get_vertex_data().begin();

    VkShaderModule vert_module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &vert_ci, nullptr, &vert_module) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create vertex shader module");
        return 0;
    }

    VkShaderModuleCreateInfo frag_ci{};
    frag_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    frag_ci.codeSize = desc.get_fragment_size();
    frag_ci.pCode = desc.get_fragment_data().begin();

    VkShaderModule frag_module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &frag_ci, nullptr, &frag_module) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        VELK_LOG(E, "VkBackend: failed to create fragment shader module");
        return 0;
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

    // Empty vertex input (vertex pulling via BDA)
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = (desc.topology == Topology::TriangleStrip)
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
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.blendEnable = VK_TRUE;
    blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.colorBlendOp = VK_BLEND_OP_ADD;
    blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_att;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.stageCount = 2;
    pipeline_ci.pStages = stages;
    pipeline_ci.pVertexInputState = &vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.pViewportState = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterizer;
    pipeline_ci.pMultisampleState = &multisample;
    pipeline_ci.pColorBlendState = &blend;
    pipeline_ci.pDynamicState = &dynamic;
    pipeline_ci.layout = pipeline_layout_;
    pipeline_ci.renderPass = render_pass;
    pipeline_ci.subpass = 0;

    PipelineEntry pe{};
    VkResult pipeline_result;
    {
        VELK_PERF_SCOPE("vk.vkCreateGraphicsPipelines");
        pipeline_result = vkCreateGraphicsPipelines(
            device_, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pe.pipeline);
    }
    if (pipeline_result != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create graphics pipeline");
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return 0;
    }

    // Shader modules can be destroyed after pipeline creation
    vkDestroyShaderModule(device_, vert_module, nullptr);
    vkDestroyShaderModule(device_, frag_module, nullptr);

    PipelineId id = next_pipeline_id_++;
    pipelines_[id] = pe;
    return id;
}

PipelineId VkBackend::create_compute_pipeline(const ComputePipelineDesc& desc)
{
    if (!desc.compute) {
        VELK_LOG(E, "VkBackend: create_compute_pipeline requires a compute shader");
        return 0;
    }
    VELK_PERF_SCOPE("vk.create_compute_pipeline");

    auto code = desc.compute->get_data();
    if (code.empty()) {
        VELK_LOG(E, "VkBackend: create_compute_pipeline got empty SPIR-V");
        return 0;
    }

    VkShaderModuleCreateInfo sm_ci{};
    sm_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm_ci.codeSize = desc.compute->get_data_size();
    sm_ci.pCode = code.begin();

    VkShaderModule cs_module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &sm_ci, nullptr, &cs_module) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create compute shader module");
        return 0;
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

    PipelineEntry pe{};
    pe.bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
    VkResult pipeline_result;
    {
        VELK_PERF_SCOPE("vk.vkCreateComputePipelines");
        pipeline_result = vkCreateComputePipelines(
            device_, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pe.pipeline);
    }

    vkDestroyShaderModule(device_, cs_module, nullptr);

    if (pipeline_result != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create compute pipeline");
        return 0;
    }

    PipelineId id = next_pipeline_id_++;
    pipelines_[id] = pe;
    return id;
}

void VkBackend::destroy_pipeline(PipelineId pipeline)
{
    auto it = pipelines_.find(pipeline);
    if (it == pipelines_.end()) {
        return;
    }

    vkDestroyPipeline(device_, it->second.pipeline, nullptr);
    pipelines_.erase(it);
}

// ============================================================================
// Frame rendering
// ============================================================================

void VkBackend::begin_frame()
{
    auto& sync = frame_sync_[frame_sync_index_];
    vkWaitForFences(device_, 1, &sync.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &sync.fence);
    vkResetCommandBuffer(sync.command_buffer, 0);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(sync.command_buffer, &begin_info);

    frame_open_ = true;
    present_surface_id_ = 0;
    surface_has_clear_ = false;
    cleared_textures_.clear();
}

void VkBackend::begin_pass(uint64_t target_id)
{
    auto& sync = frame_sync_[frame_sync_index_];
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;

    // Try surface
    auto sit = surfaces_.find(target_id);
    if (sit != surfaces_.end()) {
        auto& sd = sit->second;
        current_surface_ = target_id;

        // Only acquire the swapchain image once per frame per surface
        if (present_surface_id_ != target_id) {
            present_surface_id_ = target_id;

            present_acquire_sem_idx_ = acquire_semaphore_index_;
            VkSemaphore acquire_sem = image_available_[acquire_semaphore_index_];
            acquire_semaphore_index_ = (acquire_semaphore_index_ + 1) % kMaxSwapchainImages;

            VkResult result = vkAcquireNextImageKHR(
                device_, sd.swapchain, UINT64_MAX, acquire_sem, VK_NULL_HANDLE, &sd.image_index);
            if (result == VK_ERROR_OUT_OF_DATE_KHR) {
                current_surface_ = 0;
                return;
            }
        }

        rp = surface_has_clear_ ? sd.load_render_pass : sd.render_pass;
        fb = sd.framebuffers[sd.image_index];
        current_target_width_ = sd.width;
        current_target_height_ = sd.height;
        surface_has_clear_ = true;
    } else {
        // Try texture render target
        auto tit = textures_.find(static_cast<TextureId>(target_id));
        if (tit == textures_.end() || !tit->second.is_renderable) {
            return;
        }
        auto& td = tit->second;
        current_surface_ = 0;

        bool already_cleared = false;
        for (auto id : cleared_textures_) {
            if (id == static_cast<TextureId>(target_id)) {
                already_cleared = true;
                break;
            }
        }

        rp = already_cleared ? td.load_render_pass : td.render_pass;
        fb = td.framebuffer;
        current_target_width_ = td.width;
        current_target_height_ = td.height;

        if (!already_cleared) {
            cleared_textures_.push_back(static_cast<TextureId>(target_id));
        }
    }

    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = rp;
    rp_begin.framebuffer = fb;
    rp_begin.renderArea.extent = {static_cast<uint32_t>(current_target_width_),
                                  static_cast<uint32_t>(current_target_height_)};

    VkClearValue clear_value{};
    clear_value.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    rp_begin.clearValueCount = 1;
    rp_begin.pClearValues = &clear_value;

    vkCmdBeginRenderPass(sync.command_buffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    // Bind bindless descriptor set
    vkCmdBindDescriptorSets(sync.command_buffer,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout_,
                            0,
                            1,
                            &descriptor_set_,
                            0,
                            nullptr);
}

void VkBackend::submit(array_view<const DrawCall> calls, rect vp)
{
    auto& sync = frame_sync_[frame_sync_index_];

    {
        float vp_w = (vp.width > 0) ? vp.width : static_cast<float>(current_target_width_);
        float vp_h = (vp.height > 0) ? vp.height : static_cast<float>(current_target_height_);

        VkViewport viewport{};
        viewport.x = vp.x;
        viewport.y = vp.y;
        viewport.width = vp_w;
        viewport.height = vp_h;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(sync.command_buffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {static_cast<int32_t>(vp.x), static_cast<int32_t>(vp.y)};
        scissor.extent = {static_cast<uint32_t>(vp_w), static_cast<uint32_t>(vp_h)};
        vkCmdSetScissor(sync.command_buffer, 0, 1, &scissor);
    }
    for (size_t i = 0; i < calls.size(); ++i) {
        const auto& call = calls[i];

        auto pit = pipelines_.find(call.pipeline);
        if (pit == pipelines_.end()) {
            continue;
        }

        vkCmdBindPipeline(sync.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pit->second.pipeline);

        if (call.root_constants_size > 0) {
            vkCmdPushConstants(sync.command_buffer,
                               pipeline_layout_,
                               VK_SHADER_STAGE_ALL,
                               0,
                               call.root_constants_size,
                               call.root_constants);
        }

        vkCmdDraw(sync.command_buffer, call.vertex_count, call.instance_count, 0, 0);
    }
}

void VkBackend::end_pass()
{
    auto& sync = frame_sync_[frame_sync_index_];
    vkCmdEndRenderPass(sync.command_buffer);
    current_surface_ = 0;
}

void VkBackend::dispatch(array_view<const DispatchCall> calls)
{
    if (calls.empty()) {
        return;
    }
    auto& sync = frame_sync_[frame_sync_index_];

    // Bind the bindless descriptor set for the compute bind point so the
    // shader can sample textures / read the global texture array.
    vkCmdBindDescriptorSets(sync.command_buffer,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_,
                            0, 1, &descriptor_set_,
                            0, nullptr);

    for (size_t i = 0; i < calls.size(); ++i) {
        const auto& call = calls[i];

        auto pit = pipelines_.find(call.pipeline);
        if (pit == pipelines_.end()) {
            continue;
        }
        if (pit->second.bind_point != VK_PIPELINE_BIND_POINT_COMPUTE) {
            continue;
        }

        vkCmdBindPipeline(sync.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pit->second.pipeline);

        if (call.root_constants_size > 0) {
            vkCmdPushConstants(sync.command_buffer,
                               pipeline_layout_,
                               VK_SHADER_STAGE_ALL,
                               0,
                               call.root_constants_size,
                               call.root_constants);
        }

        vkCmdDispatch(sync.command_buffer, call.groups_x, call.groups_y, call.groups_z);
    }

    // Ensure storage-image writes from compute are visible to subsequent
    // sampled reads in graphics passes. Over-synchronizes if callers want
    // to chain multiple dispatch batches, but safe as a default.
    barrier(PipelineStage::ComputeShader, PipelineStage::FragmentShader);
}

void VkBackend::blit_to_surface(TextureId source, uint64_t surface_id, rect dst_rect)
{
    auto& sync = frame_sync_[frame_sync_index_];

    auto sit = surfaces_.find(surface_id);
    if (sit == surfaces_.end()) {
        return;
    }
    auto& sd = sit->second;

    auto tit = textures_.find(source);
    if (tit == textures_.end()) {
        return;
    }
    auto& td = tit->second;

    // Acquire swapchain image once per frame per surface (mirrors begin_pass).
    if (present_surface_id_ != surface_id) {
        present_surface_id_ = surface_id;
        present_acquire_sem_idx_ = acquire_semaphore_index_;
        VkSemaphore acquire_sem = image_available_[acquire_semaphore_index_];
        acquire_semaphore_index_ = (acquire_semaphore_index_ + 1) % kMaxSwapchainImages;

        VkResult result = vkAcquireNextImageKHR(
            device_, sd.swapchain, UINT64_MAX, acquire_sem, VK_NULL_HANDLE, &sd.image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            present_surface_id_ = 0;
            return;
        }
    }

    VkImage swap_image = sd.images[sd.image_index];

    // Swapchain: UNDEFINED -> TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = swap_image;
    to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_dst.subresourceRange.levelCount = 1;
    to_dst.subresourceRange.layerCount = 1;
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    // Storage texture: GENERAL -> TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = td.image;
    to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_src.subresourceRange.levelCount = 1;
    to_src.subresourceRange.layerCount = 1;
    to_src.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkImageMemoryBarrier pre[2] = {to_dst, to_src};
    vkCmdPipelineBarrier(sync.command_buffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, pre);

    // Resolve destination rect; default (zero size) means full surface.
    int32_t dx0 = static_cast<int32_t>(dst_rect.x);
    int32_t dy0 = static_cast<int32_t>(dst_rect.y);
    int32_t dx1 = static_cast<int32_t>(dst_rect.x + dst_rect.width);
    int32_t dy1 = static_cast<int32_t>(dst_rect.y + dst_rect.height);
    if (dst_rect.width <= 0 || dst_rect.height <= 0) {
        dx0 = 0;
        dy0 = 0;
        dx1 = sd.width;
        dy1 = sd.height;
    }

    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1] = {td.width, td.height, 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = {dx0, dy0, 0};
    blit.dstOffsets[1] = {dx1, dy1, 1};

    vkCmdBlitImage(sync.command_buffer,
                   td.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swap_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_LINEAR);

    // Swapchain: TRANSFER_DST_OPTIMAL -> PRESENT_SRC_KHR
    VkImageMemoryBarrier to_present{};
    to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = swap_image;
    to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_present.subresourceRange.levelCount = 1;
    to_present.subresourceRange.layerCount = 1;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;

    // Storage texture: TRANSFER_SRC_OPTIMAL -> GENERAL (ready for next compute)
    VkImageMemoryBarrier to_general{};
    to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.image = td.image;
    to_general.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_general.subresourceRange.levelCount = 1;
    to_general.subresourceRange.layerCount = 1;
    to_general.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_general.dstAccessMask = 0;

    VkImageMemoryBarrier post[2] = {to_present, to_general};
    vkCmdPipelineBarrier(sync.command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 2, post);

    // The blit fully populates the swap image. Mark the surface as
    // "already cleared" so any subsequent begin_pass (e.g. a performance
    // overlay view rendered on top) uses the load render pass and
    // preserves our pixels instead of clearing over them.
    surface_has_clear_ = true;
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
    auto& sync = frame_sync_[frame_sync_index_];

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

void VkBackend::end_frame()
{
    auto& sync = frame_sync_[frame_sync_index_];
    vkEndCommandBuffer(sync.command_buffer);

    if (present_surface_id_ != 0) {
        // Surface was used: submit with swapchain synchronization
        auto it = surfaces_.find(present_surface_id_);
        if (it != surfaces_.end()) {
            auto& sd = it->second;

            VkSemaphore wait_sem = image_available_[present_acquire_sem_idx_];
            VkSemaphore signal_sem = render_finished_[sd.image_index];

            // Cover both the raster path (COLOR_ATTACHMENT_OUTPUT) and the
            // RT blit path (TRANSFER) on acquire-semaphore wait.
            VkPipelineStageFlags wait_stage =
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;

            VkSubmitInfo submit_info{};
            submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit_info.waitSemaphoreCount = 1;
            submit_info.pWaitSemaphores = &wait_sem;
            submit_info.pWaitDstStageMask = &wait_stage;
            submit_info.commandBufferCount = 1;
            submit_info.pCommandBuffers = &sync.command_buffer;
            submit_info.signalSemaphoreCount = 1;
            submit_info.pSignalSemaphores = &signal_sem;

            vkQueueSubmit(graphics_queue_, 1, &submit_info, sync.fence);

            VkPresentInfoKHR present_info{};
            present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present_info.waitSemaphoreCount = 1;
            present_info.pWaitSemaphores = &signal_sem;
            present_info.swapchainCount = 1;
            present_info.pSwapchains = &sd.swapchain;
            present_info.pImageIndices = &sd.image_index;

            vkQueuePresentKHR(graphics_queue_, &present_info);
        }
    } else {
        // Headless: submit without swapchain synchronization
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &sync.command_buffer;

        vkQueueSubmit(graphics_queue_, 1, &submit_info, sync.fence);
    }

    frame_sync_index_ = (frame_sync_index_ + 1) % kFrameOverlap;
    frame_open_ = false;
    present_surface_id_ = 0;
}

// ============================================================================
// Utility
// ============================================================================

VkCommandBuffer VkBackend::begin_one_shot_commands()
{
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cb;
    vkAllocateCommandBuffers(device_, &alloc_info, &cb);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin_info);

    return cb;
}

void VkBackend::end_one_shot_commands(VkCommandBuffer cb)
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

void VkBackend::transition_image_layout(VkCommandBuffer cb, VkImage image, VkImageLayout old_layout,
                                        VkImageLayout new_layout)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
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

} // namespace velk::vk
