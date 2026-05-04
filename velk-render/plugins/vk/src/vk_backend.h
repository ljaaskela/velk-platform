#ifndef VELK_VK_BACKEND_H
#define VELK_VK_BACKEND_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <unordered_map>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/plugins/vk/plugin.h>
#include <vma/vk_mem_alloc.h>
#include <volk/volk.h>

namespace velk::vk {

class VkBackend : public ext::Object<VkBackend, IRenderBackend>
{
public:
    VELK_CLASS_UID(ClassId::VkBackend, "VkBackend");

    ~VkBackend() override;

    // IRenderBackend
    bool init(void* params) override;
    void shutdown() override;
    void wait_idle() override;
    uint64_t frame_completion_marker() const override;
    void wait_for_frame_completion(uint64_t marker) override;
    uint64_t pending_frame_completion_marker() const override;
    bool is_frame_complete(uint64_t marker) const override;

    uint64_t create_surface(const SurfaceDesc& desc) override;
    void destroy_surface(uint64_t surface_id) override;
    void resize_surface(uint64_t surface_id, int width, int height) override;

    GpuBuffer create_buffer(const GpuBufferDesc& desc) override;
    void destroy_buffer(GpuBuffer buffer) override;
    void* map(GpuBuffer buffer) override;
    uint64_t gpu_address(GpuBuffer buffer) override;

    TextureId create_texture(const TextureDesc& desc) override;
    void destroy_texture(TextureId texture) override;
    void upload_texture(TextureId texture, const uint8_t* pixels, int width, int height) override;
    bool read_texture(TextureId texture, vector<uint8_t>& out_pixels,
                      PixelFormat& out_format, uvec2& out_dims) override;

    RenderTargetGroup create_render_target_group(const TextureGroupDesc& desc) override;
    void destroy_render_target_group(RenderTargetGroup group) override;
    TextureId get_render_target_group_attachment(
        RenderTargetGroup group, uint32_t index) const override;

    PipelineId create_pipeline(const PipelineDesc& desc,
                               PixelFormat target_format = PixelFormat::Surface,
                               RenderTargetGroup target_group = 0) override;
    PipelineId create_compute_pipeline(const ComputePipelineDesc& desc) override;
    void destroy_pipeline(PipelineId pipeline) override;

    void begin_frame() override;
    void begin_pass(uint64_t target_id) override;
    void submit(array_view<const DrawCall> calls, rect viewport) override;
    void end_pass() override;
    void dispatch(array_view<const DispatchCall> calls) override;
    void blit_to_surface(TextureId source, uint64_t surface_id, rect dst_rect) override;
    void blit_group_depth_to_surface(RenderTargetGroup src_group, uint64_t surface_id,
                                     rect dst_rect) override;
    void barrier(PipelineStage src, PipelineStage dst) override;
    void bind_view_globals(GpuBuffer buffer, uint64_t offset, uint32_t range) override;
    void end_frame() override;

private:
    /// View-globals UBO state. Binding 4 is a UNIFORM_BUFFER_DYNAMIC; we
    /// rebind the underlying buffer when it changes (typically once per
    /// frame as the staging slot rotates) and supply the per-view offset
    /// as a dynamic offset on every vkCmdBindDescriptorSets call. 0 means
    /// "no view-globals bound yet"; bindless draws that never touch
    /// binding 4 are unaffected (PARTIALLY_BOUND_BIT).
    GpuBuffer view_globals_buffer_ = 0;
    uint32_t  view_globals_dynamic_offset_ = 0;
    uint32_t  view_globals_range_ = 0;

public:

private:
    // Vulkan core
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    uint32_t graphics_family_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;

    // Per-frame GPU completion timeline. Each end_frame's submit signals
    // this semaphore at `next_frame_value_` and increments. Renderer
    // grabs the value via frame_completion_marker() right after end_frame
    // and stores it on its FrameSlot, then calls wait_for_frame_completion
    // before reusing the slot — replacing the prior CPU-counter heuristic
    // with a real GPU fence.
    VkSemaphore frame_timeline_ = VK_NULL_HANDLE;
    uint64_t next_frame_value_ = 1;
    uint64_t last_frame_value_ = 0;

    // Command submission with double-buffered sync objects.
    // Even with single frame-in-flight, we need 2 sets because the present
    // engine may still reference the previous frame's semaphores.
    VkCommandPool command_pool_ = VK_NULL_HANDLE;

    static constexpr uint32_t kFrameOverlap = 3;

    // Per-frame-in-flight sync: fence + command buffer.
    struct FrameSync
    {
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    };
    FrameSync frame_sync_[kFrameOverlap]{};
    uint32_t frame_sync_index_ = 0;

    // Per-swapchain-image semaphores to avoid present engine conflicts.
    // Indexed by the acquired image index, not the frame sync index.
    static constexpr uint32_t kMaxSwapchainImages = 4;
    VkSemaphore image_available_[kMaxSwapchainImages]{};
    VkSemaphore render_finished_[kMaxSwapchainImages]{};
    uint32_t acquire_semaphore_index_ = 0;

    // Bindless textures
    static constexpr uint32_t kMaxBindlessTextures = 1024;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    VkSampler linear_sampler_ = VK_NULL_HANDLE;  ///< Default Repeat+Linear sampler. Kept as the fallback when no per-texture desc is supplied.
    uint32_t next_bindless_index_ = 1; // 0 reserved for "no texture"

    /// Per-(SamplerDesc) sampler cache. Lookup key is the byte pattern of
    /// the desc (struct is POD with no padding for our enum sizes), so
    /// distinct addressing/filter combinations each materialise exactly
    /// once.
    struct SamplerKey
    {
        SamplerDesc desc{};
        bool operator==(const SamplerKey& o) const { return desc == o.desc; }
    };
    struct SamplerKeyHash
    {
        size_t operator()(const SamplerKey& k) const noexcept;
    };
    std::unordered_map<SamplerKey, VkSampler, SamplerKeyHash> sampler_cache_;

    /// Returns a VkSampler matching @p desc, creating + caching on first use.
    VkSampler get_or_create_sampler(const SamplerDesc& desc);

    // Shared pipeline layout (push constants + bindless set)
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;

    // Default render pass (created at init from the initial surface format,
    // used for pipeline creation before any swapchain exists). If any
    // surface has a depth attachment, the default render pass is recreated
    // with a matching depth attachment so pipelines targeting surfaces are
    // compatible with depth-enabled render passes.
    VkRenderPass default_render_pass_ = VK_NULL_HANDLE;
    VkFormat default_surface_format_ = VK_FORMAT_UNDEFINED;
    VkFormat default_depth_format_ = VK_FORMAT_UNDEFINED; ///< VK_FORMAT_UNDEFINED = no depth.

    /// Per-color-format single-attachment, no-depth render passes used
    /// to compile pipelines that render into format-explicit RTTs (HDR
    /// path target etc.). `Surface` callers go through `default_render_pass_`
    /// and are not stored here. Created lazily on first request.
    std::unordered_map<VkFormat, VkRenderPass> single_attachment_render_passes_;
    VkRenderPass get_or_create_single_attachment_render_pass(VkFormat color_format);

    // Surfaces
    struct SurfaceData
    {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkRenderPass render_pass = VK_NULL_HANDLE;       ///< loadOp=CLEAR (first pass)
        VkRenderPass load_render_pass = VK_NULL_HANDLE;  ///< loadOp=LOAD (subsequent passes)
        vector<VkImage> images;
        vector<VkImageView> image_views;
        vector<VkFramebuffer> framebuffers;
        VkFormat image_format = VK_FORMAT_UNDEFINED;
        int width = 0;
        int height = 0;
        uint32_t image_index = 0;
        UpdateRate update_rate = UpdateRate::VSync;

        // Depth attachment (one per swapchain image; DepthFormat::None means
        // none of these are populated and the render pass has no depth).
        DepthFormat depth_format = DepthFormat::None;
        VkFormat depth_vk_format = VK_FORMAT_UNDEFINED;
        vector<VkImage> depth_images;
        vector<VkImageView> depth_views;
        vector<VmaAllocation> depth_allocations;
    };

    std::unordered_map<uint64_t, SurfaceData> surfaces_;
    uint64_t next_surface_id_ = 1;
    uint64_t current_surface_ = 0;          ///< Surface active in the current render pass (0 if texture target).
    uint64_t present_surface_id_ = 0;     ///< Surface to present in end_frame (0 = headless).
    uint32_t present_acquire_sem_idx_ = 0; ///< Acquire semaphore index used for the surface pass.
    bool frame_open_ = false;             ///< True between begin_frame/end_frame.
    bool surface_has_clear_ = false;       ///< True after first pass on a surface (subsequent passes use LOAD).
    int current_target_width_ = 0;         ///< Width of the current render pass target.
    int current_target_height_ = 0;        ///< Height of the current render pass target.
    vector<TextureId> cleared_textures_;   ///< Textures that have been cleared this frame.
    vector<RenderTargetGroup> cleared_render_target_groups_; ///< MRT groups already cleared this frame.

    // Buffers
    struct BufferData
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
        size_t size = 0;
    };

    std::unordered_map<GpuBuffer, BufferData> buffers_;
    GpuBuffer next_buffer_id_ = 1;

    // Textures
    struct TextureData
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        uint32_t bindless_index = 0;
        int width = 0;
        int height = 0;
        PixelFormat format = PixelFormat::RGBA8;
        bool is_renderable = false;
        uint32_t mip_levels = 1;
        // Current image layout for cross-pass operations (e.g. blits).
        // Storage textures land at GENERAL after their initial transition;
        // render-target attachments sit at SHADER_READ_ONLY_OPTIMAL between
        // passes via the render pass finalLayout. blit_to_surface saves,
        // uses, and restores this value so it works for both kinds.
        VkImageLayout current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkRenderPass render_pass = VK_NULL_HANDLE;
        VkRenderPass load_render_pass = VK_NULL_HANDLE;
    };

    std::unordered_map<TextureId, TextureData> textures_;

    // MRT render target groups: N sampleable attachments sharing one
    // render pass + framebuffer. Individual attachments are regular
    // TextureIds (also tracked in textures_) so shaders can sample
    // them after the group's pass ends.
    struct RenderTargetGroupData
    {
        vector<TextureId> attachments;
        vector<VkFormat> vk_formats;
        VkRenderPass render_pass = VK_NULL_HANDLE;      ///< loadOp=CLEAR
        VkRenderPass load_render_pass = VK_NULL_HANDLE; ///< loadOp=LOAD
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        int width = 0;
        int height = 0;

        // Optional depth attachment. VK_FORMAT_UNDEFINED means no depth.
        VkFormat depth_vk_format = VK_FORMAT_UNDEFINED;
        VkImage depth_image = VK_NULL_HANDLE;
        VkImageView depth_view = VK_NULL_HANDLE;
        VmaAllocation depth_allocation = VK_NULL_HANDLE;
    };

    std::unordered_map<RenderTargetGroup, RenderTargetGroupData> render_target_groups_;
    uint64_t next_render_target_group_id_ = 1;

    // Pipelines
    struct PipelineEntry
    {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    };

    std::unordered_map<PipelineId, PipelineEntry> pipelines_;
    PipelineId next_pipeline_id_ = 1;

    bool initialized_ = false;

    // Internal helpers
    bool create_vk_instance();
    bool select_physical_device();
    bool create_device();
    bool create_allocator();
    bool create_command_pool();
    bool create_sync_objects();
    bool create_bindless_descriptor();
    bool create_pipeline_layout();

    bool create_swapchain(SurfaceData& sd);
    void destroy_swapchain(SurfaceData& sd);

    VkCommandBuffer begin_one_shot_commands();
    void end_one_shot_commands(VkCommandBuffer cb);
    void transition_image_layout(VkCommandBuffer cb, VkImage image, VkImageLayout old_layout,
                                 VkImageLayout new_layout, uint32_t mip_levels = 1);
};

} // namespace velk::vk

#endif // VELK_VK_BACKEND_H
