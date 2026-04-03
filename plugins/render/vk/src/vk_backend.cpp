#include "vk_backend.h"

#include <cstring>

namespace velk_ui {

namespace {

VkFormat choose_surface_format(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr);
    velk::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, formats.data());

    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f.format;
        }
    }
    return formats.empty() ? VK_FORMAT_B8G8R8A8_SRGB : formats[0].format;
}

VkPresentModeKHR choose_present_mode(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, nullptr);
    velk::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, modes.data());

    for (auto m : modes) {
        if (m == VK_PRESENT_MODE_FIFO_KHR) return m;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

uint32_t attrib_byte_size(VertexAttribType type)
{
    switch (type) {
    case VertexAttribType::Float:  return 4;
    case VertexAttribType::Float2: return 8;
    case VertexAttribType::Float3: return 12;
    case VertexAttribType::Float4: return 16;
    }
    return 16;
}

VkFormat attrib_vk_format(VertexAttribType type)
{
    switch (type) {
    case VertexAttribType::Float:  return VK_FORMAT_R32_SFLOAT;
    case VertexAttribType::Float2: return VK_FORMAT_R32G32_SFLOAT;
    case VertexAttribType::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexAttribType::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_R32G32B32A32_SFLOAT;
}

} // namespace

VkBackend::~VkBackend()
{
    if (initialized_) {
        VkBackend::shutdown();
    }
}

bool VkBackend::create_instance()
{
    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "velk-ui";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "velk";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    const char* instance_extensions[] = {
        "VK_KHR_surface",
#ifdef _WIN32
        "VK_KHR_win32_surface",
#elif defined(__linux__)
        "VK_KHR_xcb_surface",
#endif
    };
    uint32_t ext_count = sizeof(instance_extensions) / sizeof(instance_extensions[0]);

    VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = ext_count;
    create_info.ppEnabledExtensionNames = instance_extensions;

#ifdef _DEBUG
    const char* validation_layer = "VK_LAYER_KHRONOS_validation";
    create_info.enabledLayerCount = 1;
    create_info.ppEnabledLayerNames = &validation_layer;
#endif

    if (vkCreateInstance(&create_info, nullptr, &instance_) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create VkInstance");
        return false;
    }

    volkLoadInstance(instance_);
    return true;
}

bool VkBackend::select_physical_device()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        VELK_LOG(E, "VkBackend: no Vulkan-capable GPU found");
        return false;
    }

    velk::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    // Pick the first discrete GPU, or fall back to the first device
    physical_device_ = devices[0];
    for (auto d : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physical_device_ = d;
            VELK_LOG(I, "VkBackend: using GPU: %s", props.deviceName);
            break;
        }
    }

    // Find a graphics queue family that also supports present
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qf_count, nullptr);
    velk::vector<VkQueueFamilyProperties> qf_props(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qf_count, qf_props.data());

    for (uint32_t i = 0; i < qf_count; ++i) {
        if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, i, surface_khr_, &present_support);
            if (present_support) {
                queue_family_ = i;
                return true;
            }
        }
    }

    VELK_LOG(E, "VkBackend: no suitable queue family found");
    return false;
}

bool VkBackend::create_device()
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    // Enable Vulkan 1.2 features for descriptor indexing (bindless)
    VkPhysicalDeviceVulkan12Features vk12_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vk12_features.descriptorIndexing = VK_TRUE;
    vk12_features.descriptorBindingPartiallyBound = VK_TRUE;
    vk12_features.descriptorBindingVariableDescriptorCount = VK_TRUE;
    vk12_features.runtimeDescriptorArray = VK_TRUE;
    vk12_features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &vk12_features;

    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features2;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;

    if (vkCreateDevice(physical_device_, &device_info, nullptr, &device_) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create VkDevice");
        return false;
    }

    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &graphics_queue_);
    return true;
}

bool VkBackend::create_allocator()
{
    VmaVulkanFunctions vma_funcs{};
    vma_funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vma_funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo alloc_info{};
    alloc_info.vulkanApiVersion = VK_API_VERSION_1_2;
    alloc_info.instance = instance_;
    alloc_info.physicalDevice = physical_device_;
    alloc_info.device = device_;
    alloc_info.pVulkanFunctions = &vma_funcs;

    if (vmaCreateAllocator(&alloc_info, &allocator_) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create VMA allocator");
        return false;
    }
    return true;
}

bool VkBackend::create_command_pool()
{
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_;

    if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo alloc_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    return vkAllocateCommandBuffers(device_, &alloc_info, &command_buffer_) == VK_SUCCESS;
}

bool VkBackend::create_sync_objects()
{
    VkSemaphoreCreateInfo sem_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    return vkCreateSemaphore(device_, &sem_info, nullptr, &image_available_semaphore_) == VK_SUCCESS
        && vkCreateSemaphore(device_, &sem_info, nullptr, &render_finished_semaphore_) == VK_SUCCESS
        && vkCreateFence(device_, &fence_info, nullptr, &in_flight_fence_) == VK_SUCCESS;
}

bool VkBackend::create_bindless_descriptor()
{
    // Create sampler
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler_info, nullptr, &default_sampler_) != VK_SUCCESS) {
        return false;
    }

    // Descriptor set layout with variable-length sampler array
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = kMaxBindlessTextures;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorBindingFlags binding_flags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags_info.bindingCount = 1;
    flags_info.pBindingFlags = &binding_flags;

    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.pNext = &flags_info;
    layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_set_layout_) != VK_SUCCESS) {
        return false;
    }

    // Descriptor pool
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = kMaxBindlessTextures;

    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;

    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        return false;
    }

    // Allocate the single descriptor set
    uint32_t variable_count = kMaxBindlessTextures;
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    variable_info.descriptorSetCount = 1;
    variable_info.pDescriptorCounts = &variable_count;

    VkDescriptorSetAllocateInfo alloc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc_info.pNext = &variable_info;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &descriptor_set_layout_;

    return vkAllocateDescriptorSets(device_, &alloc_info, &descriptor_set_) == VK_SUCCESS;
}

bool VkBackend::create_staging_buffer(size_t size)
{
    VkBufferCreateInfo buf_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buf_info.size = size;
    buf_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;

    if (vmaCreateBuffer(allocator_, &buf_info, &alloc_info,
                        &staging_buffer_, &staging_allocation_, nullptr) != VK_SUCCESS) {
        return false;
    }
    staging_buffer_size_ = size;
    return true;
}

void VkBackend::ensure_staging_buffer(size_t required_size)
{
    if (required_size <= staging_buffer_size_) return;

    if (staging_buffer_) {
        vmaDestroyBuffer(allocator_, staging_buffer_, staging_allocation_);
        staging_buffer_ = VK_NULL_HANDLE;
        staging_allocation_ = VK_NULL_HANDLE;
    }

    size_t new_size = staging_buffer_size_ * 2;
    while (new_size < required_size) new_size *= 2;
    create_staging_buffer(new_size);
}

bool VkBackend::init(void* params)
{
    if (!params) {
        VELK_LOG(E, "VkBackend::init: VulkanInitParams required");
        return false;
    }

    auto* vk_params = static_cast<VulkanInitParams*>(params);

    if (volkInitialize() != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend::init: failed to initialize volk");
        return false;
    }

    if (!create_instance()) return false;

    if (!vk_params->create_surface || !vk_params->create_surface(instance_, &surface_khr_, vk_params->user_data)) {
        VELK_LOG(E, "VkBackend::init: failed to create window surface");
        return false;
    }

    if (!select_physical_device()) return false;
    if (!create_device()) return false;
    if (!create_allocator()) return false;
    if (!create_command_pool()) return false;
    if (!create_sync_objects()) return false;
    if (!create_bindless_descriptor()) return false;
    if (!create_staging_buffer(kInitialStagingSize)) return false;

    initialized_ = true;
    VELK_LOG(I, "VkBackend initialized (Vulkan 1.2, bindless)");
    return true;
}

void VkBackend::shutdown()
{
    if (!initialized_) return;

    vkDeviceWaitIdle(device_);

    // Destroy textures
    for (auto& [key, tex] : textures_) {
        if (tex.view) vkDestroyImageView(device_, tex.view, nullptr);
        if (tex.image) vmaDestroyImage(allocator_, tex.image, tex.allocation);
    }
    textures_.clear();

    // Destroy pipelines
    for (auto& [key, entry] : pipelines_) {
        if (entry.pipeline) vkDestroyPipeline(device_, entry.pipeline, nullptr);
        if (entry.layout) vkDestroyPipelineLayout(device_, entry.layout, nullptr);
        if (entry.vert_module) vkDestroyShaderModule(device_, entry.vert_module, nullptr);
        if (entry.frag_module) vkDestroyShaderModule(device_, entry.frag_module, nullptr);
    }
    pipelines_.clear();

    // Destroy surfaces (swapchains)
    for (auto& [id, s] : surfaces_) {
        destroy_swapchain(s);
    }
    surfaces_.clear();

    if (staging_buffer_) vmaDestroyBuffer(allocator_, staging_buffer_, staging_allocation_);
    if (default_sampler_) vkDestroySampler(device_, default_sampler_, nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (descriptor_set_layout_) vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
    if (in_flight_fence_) vkDestroyFence(device_, in_flight_fence_, nullptr);
    if (render_finished_semaphore_) vkDestroySemaphore(device_, render_finished_semaphore_, nullptr);
    if (image_available_semaphore_) vkDestroySemaphore(device_, image_available_semaphore_, nullptr);
    if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
    if (allocator_) vmaDestroyAllocator(allocator_);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (surface_khr_) vkDestroySurfaceKHR(instance_, surface_khr_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);

    initialized_ = false;
}

bool VkBackend::create_swapchain(SurfaceData& surface)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_khr_, &caps);

    VkExtent2D extent = {static_cast<uint32_t>(surface.width), static_cast<uint32_t>(surface.height)};
    extent.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, extent.width));
    extent.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, extent.height));

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    surface.image_format = choose_surface_format(physical_device_, surface_khr_);

    VkSwapchainCreateInfoKHR swap_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swap_info.surface = surface_khr_;
    swap_info.minImageCount = image_count;
    swap_info.imageFormat = surface.image_format;
    swap_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swap_info.imageExtent = extent;
    swap_info.imageArrayLayers = 1;
    swap_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swap_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swap_info.preTransform = caps.currentTransform;
    swap_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swap_info.presentMode = choose_present_mode(physical_device_, surface_khr_);
    swap_info.clipped = VK_TRUE;

    VkResult swap_result = vkCreateSwapchainKHR(device_, &swap_info, nullptr, &surface.swapchain);
    if (swap_result != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create swapchain (VkResult %d, extent=%ux%u, format=%d)",
                 static_cast<int>(swap_result), extent.width, extent.height, surface.image_format);
        return false;
    }

    // Get swapchain images
    uint32_t actual_count = 0;
    vkGetSwapchainImagesKHR(device_, surface.swapchain, &actual_count, nullptr);
    surface.images.resize(actual_count);
    vkGetSwapchainImagesKHR(device_, surface.swapchain, &actual_count, surface.images.data());

    // Create image views
    surface.image_views.resize(actual_count);
    for (uint32_t i = 0; i < actual_count; ++i) {
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = surface.images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = surface.image_format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        vkCreateImageView(device_, &view_info, nullptr, &surface.image_views[i]);
    }

    // Render pass
    VkAttachmentDescription color_attachment{};
    color_attachment.format = surface.image_format;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &color_attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;

    if (vkCreateRenderPass(device_, &rp_info, nullptr, &surface.render_pass) != VK_SUCCESS) {
        return false;
    }

    // Framebuffers
    surface.framebuffers.resize(actual_count);
    for (uint32_t i = 0; i < actual_count; ++i) {
        VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb_info.renderPass = surface.render_pass;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments = &surface.image_views[i];
        fb_info.width = extent.width;
        fb_info.height = extent.height;
        fb_info.layers = 1;
        vkCreateFramebuffer(device_, &fb_info, nullptr, &surface.framebuffers[i]);
    }

    return true;
}

void VkBackend::destroy_swapchain(SurfaceData& surface)
{
    for (auto fb : surface.framebuffers) {
        if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    }
    surface.framebuffers.clear();

    if (surface.render_pass) {
        vkDestroyRenderPass(device_, surface.render_pass, nullptr);
        surface.render_pass = VK_NULL_HANDLE;
    }

    for (auto iv : surface.image_views) {
        if (iv) vkDestroyImageView(device_, iv, nullptr);
    }
    surface.image_views.clear();
    surface.images.clear();

    if (surface.swapchain) {
        vkDestroySwapchainKHR(device_, surface.swapchain, nullptr);
        surface.swapchain = VK_NULL_HANDLE;
    }
}

bool VkBackend::create_surface(uint64_t surface_id, const SurfaceDesc& desc)
{
    SurfaceData surface;
    surface.width = desc.width;
    surface.height = desc.height;

    if (!create_swapchain(surface)) {
        return false;
    }

    surfaces_[surface_id] = std::move(surface);
    return true;
}

void VkBackend::destroy_surface(uint64_t surface_id)
{
    auto it = surfaces_.find(surface_id);
    if (it != surfaces_.end()) {
        vkDeviceWaitIdle(device_);
        destroy_swapchain(it->second);
        surfaces_.erase(it);
    }
}

void VkBackend::update_surface(uint64_t surface_id, const SurfaceDesc& desc)
{
    auto it = surfaces_.find(surface_id);
    if (it == surfaces_.end()) return;

    vkDeviceWaitIdle(device_);
    destroy_swapchain(it->second);
    it->second.width = desc.width;
    it->second.height = desc.height;
    create_swapchain(it->second);
}

bool VkBackend::register_pipeline(uint64_t pipeline_key, const PipelineDesc& desc)
{
    if (pipelines_.count(pipeline_key)) return true;

    // Need at least one surface's render pass to create the pipeline
    VkRenderPass render_pass = VK_NULL_HANDLE;
    for (auto& [id, s] : surfaces_) {
        if (s.render_pass) {
            render_pass = s.render_pass;
            break;
        }
    }
    if (!render_pass) {
        VELK_LOG(E, "VkBackend::register_pipeline: no render pass available (create a surface first)");
        return false;
    }

    PipelineEntry entry;
    entry.instance_stride = desc.vertex_input.stride;

    // Create shader modules
    VkShaderModuleCreateInfo vert_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    vert_info.codeSize = desc.vertex_spirv_size * sizeof(uint32_t);
    vert_info.pCode = desc.vertex_spirv;
    if (vkCreateShaderModule(device_, &vert_info, nullptr, &entry.vert_module) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create vertex shader module");
        return false;
    }

    VkShaderModuleCreateInfo frag_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    frag_info.codeSize = desc.fragment_spirv_size * sizeof(uint32_t);
    frag_info.pCode = desc.fragment_spirv;
    if (vkCreateShaderModule(device_, &frag_info, nullptr, &entry.frag_module) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, entry.vert_module, nullptr);
        VELK_LOG(E, "VkBackend: failed to create fragment shader module");
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = entry.vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = entry.frag_module;
    stages[1].pName = "main";

    // Vertex input from VertexInputDesc (instance-rate binding)
    VkVertexInputBindingDescription binding_desc{};
    binding_desc.binding = 0;
    binding_desc.stride = desc.vertex_input.stride;
    binding_desc.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    velk::vector<VkVertexInputAttributeDescription> attrib_descs;
    for (auto& attr : desc.vertex_input.attributes) {
        VkVertexInputAttributeDescription vk_attr{};
        vk_attr.location = attr.location;
        vk_attr.binding = 0;
        vk_attr.format = attrib_vk_format(attr.type);
        vk_attr.offset = attr.offset;
        attrib_descs.push_back(vk_attr);
    }

    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding_desc;
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrib_descs.size());
    vertex_input.pVertexAttributeDescriptions = attrib_descs.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    // Push constants: 128 bytes for projection + rect + texture_index + material uniforms
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = kMaxPushConstantSize;

    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &descriptor_set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &entry.layout) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create pipeline layout");
        return false;
    }

    VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = entry.layout;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &entry.pipeline) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create graphics pipeline");
        return false;
    }

    entry.uniforms = desc.uniforms;
    pipelines_[pipeline_key] = std::move(entry);
    return true;
}

velk::vector<UniformInfo> VkBackend::get_pipeline_uniforms(uint64_t pipeline_key) const
{
    auto it = pipelines_.find(pipeline_key);
    if (it != pipelines_.end()) {
        return it->second.uniforms;
    }
    return {};
}

void VkBackend::upload_texture(uint64_t texture_key,
                               const uint8_t* pixels, int width, int height)
{
    auto it = textures_.find(texture_key);
    if (it != textures_.end()) {
        // Destroy old texture
        vkDeviceWaitIdle(device_);
        if (it->second.view) vkDestroyImageView(device_, it->second.view, nullptr);
        if (it->second.image) vmaDestroyImage(allocator_, it->second.image, it->second.allocation);
        textures_.erase(it);
    }

    // Create image
    VkImageCreateInfo img_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = VK_FORMAT_R8_UNORM;
    img_info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

    TextureData tex;
    if (vmaCreateImage(allocator_, &img_info, &alloc_info,
                       &tex.image, &tex.allocation, nullptr) != VK_SUCCESS) {
        VELK_LOG(E, "VkBackend: failed to create texture image");
        return;
    }

    // Upload via staging buffer
    size_t data_size = static_cast<size_t>(width) * height;

    VkBuffer upload_buf;
    VmaAllocation upload_alloc;
    VkBufferCreateInfo buf_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buf_info.size = data_size;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo upload_alloc_info{};
    upload_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    upload_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                              VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo mapped_info;
    vmaCreateBuffer(allocator_, &buf_info, &upload_alloc_info,
                    &upload_buf, &upload_alloc, &mapped_info);
    std::memcpy(mapped_info.pMappedData, pixels, data_size);

    // Record transfer commands
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cmd_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_alloc.commandPool = command_pool_;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cmd_alloc, &cmd);

    vkBeginCommandBuffer(cmd, &begin_info);

    // Transition to TRANSFER_DST
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = tex.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    vkCmdCopyBufferToImage(cmd, upload_buf, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition to SHADER_READ
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(graphics_queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);

    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    vmaDestroyBuffer(allocator_, upload_buf, upload_alloc);

    // Create image view
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = tex.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    vkCreateImageView(device_, &view_info, nullptr, &tex.view);

    // Assign bindless slot
    tex.bindless_index = next_bindless_index_++;

    VkDescriptorImageInfo desc_image{};
    desc_image.sampler = default_sampler_;
    desc_image.imageView = tex.view;
    desc_image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor_set_;
    write.dstBinding = 0;
    write.dstArrayElement = tex.bindless_index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &desc_image;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    textures_[texture_key] = tex;
}

void VkBackend::begin_frame(uint64_t surface_id)
{
    current_surface_id_ = surface_id;

    auto it = surfaces_.find(surface_id);
    if (it == surfaces_.end()) return;
    auto& surface = it->second;

    vkWaitForFences(device_, 1, &in_flight_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &in_flight_fence_);

    VkResult result = vkAcquireNextImageKHR(device_, surface.swapchain, UINT64_MAX,
                                             image_available_semaphore_, VK_NULL_HANDLE,
                                             &surface.image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Swapchain needs recreation; will be handled by update_surface
        return;
    }

    vkResetCommandBuffer(command_buffer_, 0);

    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(command_buffer_, &begin_info);

    VkClearValue clear_value{};
    clear_value.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderPassBeginInfo rp_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp_begin.renderPass = surface.render_pass;
    rp_begin.framebuffer = surface.framebuffers[surface.image_index];
    rp_begin.renderArea.extent = {static_cast<uint32_t>(surface.width),
                                  static_cast<uint32_t>(surface.height)};
    rp_begin.clearValueCount = 1;
    rp_begin.pClearValues = &clear_value;

    vkCmdBeginRenderPass(command_buffer_, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.width = static_cast<float>(surface.width);
    viewport.height = static_cast<float>(surface.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command_buffer_, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = {static_cast<uint32_t>(surface.width), static_cast<uint32_t>(surface.height)};
    vkCmdSetScissor(command_buffer_, 0, 1, &scissor);

    // Bind the global bindless descriptor set
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelines_.empty() ? VK_NULL_HANDLE : pipelines_.begin()->second.layout,
                            0, 1, &descriptor_set_, 0, nullptr);

    // Compute projection matrix
    float w = static_cast<float>(surface.width);
    float h = static_cast<float>(surface.height);
    std::memset(projection_, 0, sizeof(projection_));
    projection_[0]  = 2.0f / w;
    projection_[5]  = 2.0f / (-h);  // flip Y
    projection_[10] = -1.0f;
    projection_[12] = -1.0f;
    projection_[13] = 1.0f;
    projection_[15] = 1.0f;
}

void VkBackend::submit(velk::array_view<const RenderBatch> batches)
{
    // Calculate total instance data size
    size_t total_size = 0;
    for (auto& batch : batches) {
        total_size += batch.instance_data.size();
    }
    if (total_size == 0) return;

    ensure_staging_buffer(total_size);

    // Map and copy all instance data
    void* mapped = nullptr;
    vmaMapMemory(allocator_, staging_allocation_, &mapped);

    size_t buffer_offset = 0;
    for (auto& batch : batches) {
        if (batch.instance_count == 0) continue;

        auto pit = pipelines_.find(batch.pipeline_key);
        if (pit == pipelines_.end()) continue;
        auto& pipeline = pit->second;

        std::memcpy(static_cast<uint8_t*>(mapped) + buffer_offset,
                    batch.instance_data.data(), batch.instance_data.size());

        vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);

        // Push projection matrix
        vkCmdPushConstants(command_buffer_, pipeline.layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           kPushConstantOffset_Projection, sizeof(projection_), projection_);

        // Push rect uniform if present
        if (batch.has_rect) {
            float rect_data[] = {batch.rect.x, batch.rect.y, batch.rect.width, batch.rect.height};
            vkCmdPushConstants(command_buffer_, pipeline.layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               kPushConstantOffset_Rect, sizeof(rect_data), rect_data);
        }

        // Push texture index
        if (batch.texture_key != 0) {
            auto tit = textures_.find(batch.texture_key);
            if (tit != textures_.end()) {
                uint32_t tex_index = tit->second.bindless_index;
                vkCmdPushConstants(command_buffer_, pipeline.layout,
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                                   kPushConstantOffset_TextureIndex, sizeof(tex_index), &tex_index);
            }
        }

        // Push material uniforms
        uint32_t uniform_offset = kPushConstantOffset_MaterialStart;
        for (auto& u : batch.uniforms) {
            if (u.location < 0) continue;
            size_t data_size = 0;
            if (u.typeUid == velk::type_uid<float>()) data_size = 4;
            else if (u.typeUid == velk::type_uid<velk::color>() || u.typeUid == velk::type_uid<velk::vec4>()) data_size = 16;
            else if (u.typeUid == velk::type_uid<velk::mat4>()) data_size = 64;
            else if (u.typeUid == velk::type_uid<int32_t>()) data_size = 4;
            else if (u.typeUid == velk::type_uid<velk::vec2>()) data_size = 8;

            if (data_size > 0 && uniform_offset + data_size <= kMaxPushConstantSize) {
                vkCmdPushConstants(command_buffer_, pipeline.layout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   uniform_offset, static_cast<uint32_t>(data_size), u.data);
                uniform_offset += static_cast<uint32_t>(data_size);
            }
        }

        // Bind instance data buffer and draw
        VkDeviceSize vk_offset = buffer_offset;
        vkCmdBindVertexBuffers(command_buffer_, 0, 1, &staging_buffer_, &vk_offset);
        vkCmdDraw(command_buffer_, 4, batch.instance_count, 0, 0);

        buffer_offset += batch.instance_data.size();
    }

    vmaUnmapMemory(allocator_, staging_allocation_);
}

void VkBackend::end_frame()
{
    auto it = surfaces_.find(current_surface_id_);
    if (it == surfaces_.end()) return;
    auto& surface = it->second;

    vkCmdEndRenderPass(command_buffer_);
    vkEndCommandBuffer(command_buffer_);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_available_semaphore_;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_finished_semaphore_;

    vkQueueSubmit(graphics_queue_, 1, &submit, in_flight_fence_);

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished_semaphore_;
    present.swapchainCount = 1;
    present.pSwapchains = &surface.swapchain;
    present.pImageIndices = &surface.image_index;

    vkQueuePresentKHR(graphics_queue_, &present);

    current_surface_id_ = 0;
}

} // namespace velk_ui
