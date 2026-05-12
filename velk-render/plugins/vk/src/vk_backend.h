#ifndef VELK_VK_BACKEND_H
#define VELK_VK_BACKEND_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/plugins/vk/plugin.h>
#include <vma/vk_mem_alloc.h>
#include <volk/volk.h>

namespace velk::vk {

class VkCommandBuffer;
class IVkGpuTexture;
class IVkRenderTargetGroup;

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
    IRenderTarget::Ptr acquire_swapchain_texture(uint64_t surface_id) override;

    IGpuBuffer::Ptr create_gpu_buffer(const GpuBufferDesc& desc) override;
    void defer_destroy_gpu_buffer(IGpuBuffer* gb,
                                  uint64_t completion_marker) override;
    void record_buffer_update(IGpuBuffer& target, size_t offset,
                              size_t size, const void* data) override;

    IGpuTexture::Ptr create_texture(const TextureDesc& desc) override;
    void upload_texture(IGpuTexture& texture, const uint8_t* pixels, int width, int height) override;
    bool read_texture(IGpuTexture& texture, vector<uint8_t>& out_pixels,
                      PixelFormat& out_format, uvec2& out_dims) override;

    IRenderTextureGroup::Ptr create_render_target_group(const TextureGroupDesc& desc) override;
    void defer_destroy_gpu_render_target_group(IRenderTextureGroup* group,
                                               uint64_t completion_marker) override;

    IGpuPipeline::Ptr create_pipeline_dynamic(
        const PipelineDesc& desc,
        array_view<const PixelFormat> color_formats,
        DepthFormat depth_format) override;
    IGpuPipeline::Ptr create_compute_pipeline(const ComputePipelineDesc& desc) override;
    void defer_destroy_gpu_pipeline(IGpuPipeline* pipeline) override;
    void defer_destroy_gpu_texture(IGpuTexture* texture,
                                   uint64_t completion_marker) override;
    IGpuTexture::Ptr create_depth_attachment_texture(int width, int height,
                                                     DepthFormat format) override;

    void begin_frame() override;
    void blit_to_texture(IGpuTexture& source, IGpuTexture& dest, rect dst_rect) override;

    IGpuCommandBuffer::Ptr create_command_buffer() override;
    void execute(const IGpuCommandBuffer::Ptr& cmd) override;

    // VkCommandBuffer needs access to internal lookup maps + handles
    // to record vkCmd* against producer-recorded draw calls.
    friend class VkCommandBuffer;

    /// Per-draw `vkCmd*` recording loop. Called by
    /// `VkCommandBuffer::record_draws` to emit BindPipeline +
    /// PushConstants + optional BindIndexBuffer + Draw*IndirectCount
    /// per call against the backend's pipeline / buffer maps.
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
    /// `blit_to_surface` primary path. Returns false if the target id
    /// does not resolve to a known texture.
    /// Records the layout-transition + vkCmdBlitImage + final-layout
    /// transition sequence for blitting one IGpuTexture into another.
    /// Surface destinations live on the legacy primary blit_to_surface
    /// path (per-frame swapchain acquisition can't be baked in).
    void record_blit_to_texture(::VkCommandBuffer cb, IGpuTexture& source,
                                IGpuTexture& dest, rect dst_rect);

    /// Defers `vkFreeCommandBuffers` for a persistent-pool secondary
    /// to the next time the current frame slot rolls around — i.e.
    /// after `kFrameOverlap` more frame submissions have all
    /// completed. Called by `VkCommandBuffer::~VkCommandBuffer` when
    /// a producer drops its Ptr, since the GPU may still be running
    /// commands from the last submission that referenced it.
    void defer_free_persistent_secondary(::VkCommandBuffer cb);
    void barrier(PipelineStage src, PipelineStage dst) override;
    void end_frame() override;

    /// @name Debug-utils label helpers.
    /// Wrap the VK_EXT_debug_utils calls so call sites don't have to
    /// build VkDebugUtilsLabelEXT structs or null-check the function
    /// pointer. No-op when the extension isn't available.
    /// @{
    static void cmd_push_label(::VkCommandBuffer cb, const char* name);
    static void cmd_pop_label(::VkCommandBuffer cb);
    /// @}

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
    // Atomic because end_frame (render thread) updates these while prepare
    // (main thread) may read frame_completion_marker concurrently.
    std::atomic<uint64_t> next_frame_value_{1};
    std::atomic<uint64_t> last_frame_value_{0};

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

    // Per-frame-in-flight sync: fence + command buffer + per-frame state.
    // Each in-flight frame owns its own slot so prepare() (main thread)
    // and submit() (render thread) never touch the same one concurrently.
    struct FrameSync
    {
        VkFence fence = VK_NULL_HANDLE;
        // Qualified `::VkCommandBuffer` everywhere — `VkCommandBuffer`
        // resolves to our impl class inside `velk::vk`.
        ::VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        /// Per-slot pool so begin_frame (main thread) and end_frame
        /// (render thread, on a different slot) never touch the same
        /// pool concurrently — Vulkan requires pool ops to be externally
        /// synchronized.
        VkCommandPool command_pool = VK_NULL_HANDLE;
        /// Persistent-pool secondaries queued for free at this slot
        /// when their owning `IGpuCommandBuffer` Ptr drops. Drained at
        /// the top of `begin_frame` for this slot, after the slot's
        /// fence has fired — guarantees kFrameOverlap frames of
        /// in-flight grace before vkFreeCommandBuffers runs.
        ::velk::vector<::VkCommandBuffer> deferred_persistent_frees;
        /// Surface picked at acquire time; consumed by end_frame's submit
        /// path. Per-slot so a second prepare() can pick a different
        /// surface without trampling the one a concurrent submit needs.
        uint64_t present_surface_id = 0;
        /// Index into image_available_ for the acquire semaphore tied to
        /// this frame's swapchain acquire. Per-slot for the same reason.
        uint32_t present_acquire_sem_idx = 0;
        /// Coalesced TRANSFER → SHADER_READ barrier flag for this frame's
        /// vkCmdUpdateBuffer calls; set during recording, consumed at the
        /// next begin_pass.
        bool pending_buffer_update_barrier = false;
    };
    FrameSync frame_sync_[kFrameOverlap]{};

    /// Slot currently being prepared/recorded on the main thread. Advances
    /// at begin_frame. Main thread only — no lock needed.
    uint32_t recording_slot_ = kFrameOverlap - 1;

    /// FIFO queue of slots between begin_frame and end_frame. begin_frame
    /// pushes; end_frame pops. Bridges the prepare (main) → submit (render)
    /// thread split so end_frame knows which slot to consume.
    std::deque<uint32_t> in_flight_slots_;
    std::mutex in_flight_mutex_;

    /// Tracked at `begin_pass` so `submit()` and
    /// `create_command_buffer()` can populate `VkCommandBufferInheritanceInfo`
    /// for the SECONDARY recordings. Framebuffer is technically optional
    /// in inheritance info but some drivers (AMD especially on MRT
    /// renderpasses) misbehave when it's VK_NULL_HANDLE.
    VkRenderPass current_render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer current_framebuffer_ = VK_NULL_HANDLE;

    // pending_buffer_update_barrier moved into FrameSync (per-slot).

    // Per-swapchain-image semaphores to avoid present engine conflicts.
    // Indexed by the acquired image index, not the frame sync index.
    // Bumped from 4 to 8 because Android compositors commonly return 5–6
    // swapchain images, which would OOB the smaller bound.
    static constexpr uint32_t kMaxSwapchainImages = 8;
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

    // Surfaces — the swapchain images are only used as transfer
    // destinations for the per-surface composite blit at end_frame.
    // No render pass / framebuffer / surface depth.
    struct SurfaceData
    {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        vector<VkImage> images;
        vector<VkImageView> image_views;
        VkFormat image_format = VK_FORMAT_UNDEFINED;
        int width = 0;
        int height = 0;
        uint32_t image_index = 0;
        UpdateRate update_rate = UpdateRate::VSync;
        SurfaceColorFormat color_format = SurfaceColorFormat::RGBA8_SRGB;

        /// Per-surface composite intermediate. Producers render here
        /// as if it were any IGpuTexture; backend emits a final
        /// composite-to-swap blit at end_frame whenever it was
        /// rendered this frame. Stable VkImage so cached cmd buffers
        /// work.
        ::velk::IGpuTexture::Ptr composite;
        /// Set by `acquire_swapchain_texture`; reset at `begin_frame`.
        /// Tells `end_frame` whether to emit a present blit for this
        /// surface. Lets multi-window apps with paused windows skip
        /// presenting un-rendered frames.
        bool composite_acquired_this_frame = false;
    };

    std::unordered_map<uint64_t, SurfaceData> surfaces_;
    uint64_t next_surface_id_ = 1;
    // present_surface_id, present_acquire_sem_idx moved into FrameSync.
    bool frame_open_ = false;             ///< True between begin_frame/end_frame (debug flag only).
    vector<IVkRenderTargetGroup*> live_render_target_groups_; ///< Walked at begin_frame to clear cleared-flags.

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

    // Textures: state lives entirely on the VkGpuTexture / VkRenderTexture
    // wrappers. Backend keeps one non-owning sidecar:
    //  * `live_render_targets_` — list of renderable wrappers walked at
    //    `begin_frame` to clear their per-frame "cleared" flag.
    vector<IVkGpuTexture*> live_render_targets_;

    // MRT render target groups: state lives on `VkRenderTargetGroup`
    // wrappers (mirrors the texture pattern). The backend keeps no
    // owning map; lifecycle flows through Ptr drops.

    /// Pending `vkDestroyPipeline` calls keyed by the
    /// frame-completion marker captured at drop time.
    struct DeferredGpuPipelineDestroy
    {
        ::VkPipeline pipeline;
        uint64_t     completion_marker;
    };
    ::velk::vector<DeferredGpuPipelineDestroy> deferred_gpu_pipelines_;
    std::mutex deferred_gpu_pipelines_mutex_;

    void drain_deferred_pipelines();

    /// Pending texture-resource frees keyed by completion marker.
    /// Captured in `defer_destroy_gpu_texture` from a dying
    /// VkGpuTexture / VkRenderTexture. Slot is leaked for now (matches
    /// current backend behaviour — `next_bindless_index_` is monotonic).
    struct DeferredGpuTextureDestroy
    {
        ::VkImage         image;
        ::VkImageView     view;
        VmaAllocation     allocation;
        ::VkFramebuffer   framebuffer;
        ::VkRenderPass    render_pass;
        ::VkRenderPass    load_render_pass;
        uint64_t          completion_marker;
    };
    ::velk::vector<DeferredGpuTextureDestroy> deferred_gpu_textures_;
    std::mutex deferred_gpu_textures_mutex_;

    void drain_deferred_textures();

    /// Pending render-target-group frees keyed by completion marker.
    /// Captured in `defer_destroy_gpu_render_target_group` from a dying
    /// VkRenderTargetGroup. Color and depth attachments are real
    /// IGpuTexture::Ptrs whose destructors handle their own deferred
    /// destroy through the observer cascade — this entry only owns the
    /// render pass + framebuffer.
    struct DeferredGpuRenderTargetGroupDestroy
    {
        ::VkRenderPass  render_pass;
        ::VkRenderPass  load_render_pass;
        ::VkFramebuffer framebuffer;
        uint64_t        completion_marker;
    };
    ::velk::vector<DeferredGpuRenderTargetGroupDestroy> deferred_gpu_render_target_groups_;
    std::mutex deferred_gpu_render_target_groups_mutex_;

    void drain_deferred_render_target_groups();

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

    /// Allocate the per-surface composite intermediate sized to the
    /// swapchain. Called after create_swapchain settles dimensions.
    bool create_surface_composite(uint64_t surface_id, SurfaceData& sd);
    void destroy_surface_composite(SurfaceData& sd);

    /// Records the final composite-to-swap blit on the primary cmd
    /// buffer for a surface that was rendered to this frame. Called
    /// from end_frame for each surface whose composite is dirty.
    void emit_surface_present_blit(uint64_t surface_id, SurfaceData& sd);

    ::VkCommandBuffer begin_one_shot_commands();
    void end_one_shot_commands(::VkCommandBuffer cb);
    void transition_image_layout(::VkCommandBuffer cb, VkImage image, VkImageLayout old_layout,
                                 VkImageLayout new_layout, uint32_t mip_levels = 1);
};

} // namespace velk::vk

#endif // VELK_VK_BACKEND_H
