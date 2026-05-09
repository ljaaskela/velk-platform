#ifndef VELK_VK_BACKEND_H
#define VELK_VK_BACKEND_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <mutex>
#include <unordered_map>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/plugins/vk/plugin.h>
#include <vma/vk_mem_alloc.h>
#include <volk/volk.h>

namespace velk::vk {

class VkCommandBuffer;

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
    bool is_surface(uint64_t id) const override
    {
        return surfaces_.find(id) != surfaces_.end();
    }

    IGpuBuffer::Ptr create_gpu_buffer(const GpuBufferDesc& desc) override;
    void defer_destroy_gpu_buffer(IGpuBuffer* gb,
                                  uint64_t completion_marker) override;

    /// Per-frame primary command buffer; valid between begin_frame
    /// and end_frame. `VkGpuBuffer::update` records
    /// `vkCmdUpdateBuffer` against this.
    ::VkCommandBuffer primary_cb() const
    {
        return frame_sync_[frame_sync_index_].command_buffer;
    }

    /// Marks that a `vkCmdUpdateBuffer` was recorded this frame so
    /// `begin_pass` emits a single TRANSFER → SHADER_READ barrier
    /// before the first secondary execute.
    void mark_pending_buffer_update_barrier()
    {
        pending_buffer_update_barrier_ = true;
    }

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

    IGpuCommandBuffer::Ptr create_command_buffer(uint64_t target_id) override;
    void execute(const IGpuCommandBuffer::Ptr& cmd) override;

    // VkCommandBuffer needs access to internal lookup maps + handles
    // to record vkCmd* against producer-recorded draw calls.
    friend class VkCommandBuffer;

    /// Looks up the render pass (compatible with the renderpass used
    /// at execute time) for inheritance info on a SECONDARY command
    /// buffer. Returns `VK_NULL_HANDLE` if @p target_id is unknown
    /// (caller falls back to a non-renderpass cmd buffer).
    VkRenderPass find_render_pass_for_target(uint64_t target_id);

    /// Per-draw `vkCmd*` recording loop, shared by
    /// `VkCommandBuffer::record_draws` (producer-recorded path) and
    /// the legacy `submit` (which records into a transient secondary
    /// every frame). Looks up pipeline / buffer handles in the
    /// backend's maps and emits BindPipeline + PushConstants +
    /// optional BindIndexBuffer + Draw*IndirectCount per call.
    void record_draw_loop(::VkCommandBuffer cb,
                          array_view<const DrawCall> calls);

    /// Records BindPipeline + PushConstants + vkCmdDispatch into @p cb
    /// for one compute call. Shared by `VkCommandBuffer::record_dispatch`
    /// (producer-recorded path) and the legacy `dispatch`.
    void record_dispatch_call(::VkCommandBuffer cb, const DispatchCall& call);

    /// Records the layout-transition + vkCmdBlitImage + final-layout
    /// transition sequence for blitting a texture into a destination
    /// texture (NOT a surface). Surface-destination blits require
    /// per-frame swapchain acquisition state and remain on the legacy
    /// `blit_to_surface` primary path. Returns false if the target is
    /// not a texture (caller should fall back to legacy).
    bool record_blit_to_texture(::VkCommandBuffer cb, TextureId source,
                                uint64_t target_id, rect dst_rect);

    /// Allocates a SECONDARY VkCommandBuffer from the current frame
    /// slot's transient pool. Used by the legacy `submit` path.
    /// Buffers allocated here are reset wholesale at the next
    /// `begin_frame` for this slot.
    ::VkCommandBuffer acquire_transient_secondary();

    /// Defers `vkFreeCommandBuffers` for a persistent-pool secondary
    /// to the next time the current frame slot rolls around — i.e.
    /// after `kFrameOverlap` more frame submissions have all
    /// completed. Called by `VkCommandBuffer::~VkCommandBuffer` when
    /// a producer drops its Ptr, since the GPU may still be running
    /// commands from the last submission that referenced it.
    void defer_free_persistent_secondary(::VkCommandBuffer cb);
    void barrier(PipelineStage src, PipelineStage dst) override;
    void end_frame() override;

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

    /// Long-lived pool for SECONDARY command buffers held by producers
    /// across frames (cached IRenderPass cmd buffers via
    /// `IGpuCommandBuffer`). Allocations from this pool persist until
    /// the producer drops the cmd buffer Ptr; the impl's destructor
    /// frees back here.
    VkCommandPool persistent_secondary_pool_ = VK_NULL_HANDLE;

    static constexpr uint32_t kFrameOverlap = 3;

    // Per-frame-in-flight sync: fence + command buffer.
    struct FrameSync
    {
        VkFence fence = VK_NULL_HANDLE;
        // Qualified `::VkCommandBuffer` everywhere — `VkCommandBuffer`
        // resolves to our impl class inside `velk::vk`.
        ::VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        /// Per-slot pool for transient SECONDARY command buffers used
        /// by the legacy `submit()` path (which records into a fresh
        /// secondary each frame and immediately executes it). Reset
        /// at the top of each `begin_frame` for this slot.
        VkCommandPool transient_secondary_pool = VK_NULL_HANDLE;
        ::velk::vector<::VkCommandBuffer> transient_secondaries;
        size_t transient_secondary_cursor = 0;
        /// Persistent-pool secondaries queued for free at this slot
        /// when their owning `IGpuCommandBuffer` Ptr drops. Drained at
        /// the top of `begin_frame` for this slot, after the slot's
        /// fence has fired — guarantees kFrameOverlap frames of
        /// in-flight grace before vkFreeCommandBuffers runs.
        ::velk::vector<::VkCommandBuffer> deferred_persistent_frees;
    };
    FrameSync frame_sync_[kFrameOverlap]{};
    uint32_t frame_sync_index_ = 0;

    /// Tracked at `begin_pass` so `submit()` and
    /// `create_command_buffer()` can populate `VkCommandBufferInheritanceInfo`
    /// for the SECONDARY recordings. Framebuffer is technically optional
    /// in inheritance info but some drivers (AMD especially on MRT
    /// renderpasses) misbehave when it's VK_NULL_HANDLE.
    VkRenderPass current_render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer current_framebuffer_ = VK_NULL_HANDLE;

/// Set by `mark_pending_buffer_update_barrier`; consumed by the
    /// next `begin_pass` to emit a single TRANSFER → SHADER_READ
    /// barrier covering all `vkCmdUpdateBuffer`s recorded this frame.
    bool pending_buffer_update_barrier_ = false;

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

    /// Pending `vmaDestroyBuffer` calls keyed by the
    /// frame-completion marker captured at drop time.
    struct DeferredGpuBufferDestroy
    {
        ::VkBuffer    buffer;
        VmaAllocation allocation;
        uint64_t      completion_marker;
    };
    ::velk::vector<DeferredGpuBufferDestroy> deferred_gpu_buffers_;
    std::mutex deferred_gpu_buffers_mutex_;

    /// Releases entries whose completion marker has been signalled.
    void drain_deferred_buffers();

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

    ::VkCommandBuffer begin_one_shot_commands();
    void end_one_shot_commands(::VkCommandBuffer cb);
    void transition_image_layout(::VkCommandBuffer cb, VkImage image, VkImageLayout old_layout,
                                 VkImageLayout new_layout, uint32_t mip_levels = 1);
};

} // namespace velk::vk

#endif // VELK_VK_BACKEND_H
