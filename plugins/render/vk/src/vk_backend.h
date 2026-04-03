#ifndef VELK_UI_VK_BACKEND_H
#define VELK_UI_VK_BACKEND_H

#include <velk/ext/object.h>
#include <velk-ui/plugins/vk/plugin.h>
#include <velk-ui/plugins/render/intf_render_backend.h>

#include <volk/volk.h>
#include <vma/vk_mem_alloc.h>

#include <cstdint>
#include <unordered_map>

namespace velk_ui {

class VkBackend : public velk::ext::Object<VkBackend, IRenderBackend>
{
public:
    VELK_CLASS_UID(ClassId::VkBackend, "VkBackend");

    ~VkBackend() override;

    bool init(void* params) override;
    void shutdown() override;

    bool create_surface(uint64_t surface_id, const SurfaceDesc& desc) override;
    void destroy_surface(uint64_t surface_id) override;
    void update_surface(uint64_t surface_id, const SurfaceDesc& desc) override;

    bool register_pipeline(uint64_t pipeline_key, const PipelineDesc& desc) override;
    velk::vector<UniformInfo> get_pipeline_uniforms(uint64_t pipeline_key) const override;
    void upload_texture(uint64_t texture_key,
                        const uint8_t* pixels, int width, int height) override;

    void begin_frame(uint64_t surface_id) override;
    void submit(velk::array_view<const RenderBatch> batches) override;
    void end_frame() override;

private:
    static constexpr uint32_t kMaxBindlessTextures = 1024;

    // Push constant layout: mat4 projection (64B) + vec4 rect (16B) + uint texture_index (4B)
    // Material uniforms follow after (up to 128B total).
    static constexpr uint32_t kPushConstantOffset_Projection = 0;
    static constexpr uint32_t kPushConstantOffset_Rect = 64;
    static constexpr uint32_t kPushConstantOffset_TextureIndex = 80;
    static constexpr uint32_t kPushConstantOffset_MaterialStart = 84;
    static constexpr uint32_t kMaxPushConstantSize = 128;

    struct PipelineEntry
    {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkShaderModule vert_module = VK_NULL_HANDLE;
        VkShaderModule frag_module = VK_NULL_HANDLE;
        uint32_t instance_stride = 0;
        velk::vector<UniformInfo> uniforms;
    };

    struct SurfaceData
    {
        int width = 0;
        int height = 0;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        velk::vector<VkImage> images;
        velk::vector<VkImageView> image_views;
        velk::vector<VkFramebuffer> framebuffers;
        VkRenderPass render_pass = VK_NULL_HANDLE;
        VkFormat image_format = VK_FORMAT_B8G8R8A8_SRGB;
        uint32_t image_index = 0;
    };

    struct TextureData
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        uint32_t bindless_index = 0;
    };

    // Vulkan core objects
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkSurfaceKHR surface_khr_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    // Command submission
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;

    // Synchronization (single frame in flight)
    VkSemaphore image_available_semaphore_ = VK_NULL_HANDLE;
    VkSemaphore render_finished_semaphore_ = VK_NULL_HANDLE;
    VkFence in_flight_fence_ = VK_NULL_HANDLE;

    // Bindless descriptor set
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    VkSampler default_sampler_ = VK_NULL_HANDLE;
    uint32_t next_bindless_index_ = 0;

    // Per-pipeline data
    std::unordered_map<uint64_t, PipelineEntry> pipelines_;

    // Per-surface data
    std::unordered_map<uint64_t, SurfaceData> surfaces_;
    uint64_t current_surface_id_ = 0;

    // Textures
    std::unordered_map<uint64_t, TextureData> textures_;

    // Instance data staging buffer (host-visible, reused each frame)
    VkBuffer staging_buffer_ = VK_NULL_HANDLE;
    VmaAllocation staging_allocation_ = VK_NULL_HANDLE;
    size_t staging_buffer_size_ = 0;
    static constexpr size_t kInitialStagingSize = 256 * 1024; // 256 KB

    // Current frame projection
    float projection_[16]{};

    bool initialized_ = false;

    bool create_instance();
    bool select_physical_device();
    bool create_device();
    bool create_allocator();
    bool create_command_pool();
    bool create_sync_objects();
    bool create_bindless_descriptor();
    bool create_staging_buffer(size_t size);

    bool create_swapchain(SurfaceData& surface);
    void destroy_swapchain(SurfaceData& surface);

    void ensure_staging_buffer(size_t required_size);
};

} // namespace velk_ui

#endif // VELK_UI_VK_BACKEND_H
