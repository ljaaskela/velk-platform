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
    void recreate_surface(uint64_t surface_id, void* native_handle,
                          int width, int height) override;
    IRenderTarget::Ptr acquire_swapchain_texture(uint64_t surface_id) override;

    IGpuBuffer::Ptr create_gpu_buffer(const GpuBufferDesc& desc) override;
    void defer_destroy_gpu_buffer(IGpuBuffer* gb,
                                  uint64_t completion_marker) override;
    void record_buffer_update(IGpuBuffer& target, size_t offset,
                              size_t size, const void* data) override;

    void set_global_buffer(uint32_t binding, IGpuBuffer* buffer) override;
    uint32_t current_frame_slot() const override { return recording_slot_; }
    uint32_t frame_slot_count() const override { return kFrameOverlap; }

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
    void close_frame() override;
    void submit_frame() override;

    void set_gpu_timing_enabled(bool enabled) override { gpu_timing_enabled_ = enabled; }
    bool gpu_timing_enabled() const override { return gpu_timing_enabled_; }
    void begin_gpu_timer(const char* label) override;
    void end_gpu_timer() override;
    array_view<const GpuPassTiming> last_gpu_timings() const override
    {
        return {last_timings_.data(), last_timings_.size()};
    }

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
    static constexpr uint32_t kMaxSwapchainImages = 8;

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
        /// Surface picked at acquire time; consumed by submit_frame. Per-slot
        /// so a second prepare() can pick a different surface without
        /// trampling the one a concurrent submit needs.
        uint64_t present_surface_id = 0;
        /// Index into image_available_ for the acquire semaphore tied to
        /// this frame's swapchain acquire. Per-slot for the same reason.
        uint32_t present_acquire_sem_idx = 0;
        /// Acquired swap image index for this frame (set by record_present_blit
        /// on the render thread); also indexes render_finished_.
        uint32_t present_image_index = 0;
        /// Present target snapshotted at `close_frame` (main thread) so
        /// submit_frame (render thread) does acquire + present-blit + present
        /// without reading shared SurfaceData (which prepare may be mutating).
        /// The composite blit is layout-neutral (returns it to SHADER_READ), so
        /// close_frame updates the composite's tracked layout and the render
        /// thread never writes shared composite state. present_swapchain ==
        /// VK_NULL_HANDLE means nothing to present this frame.
        VkSwapchainKHR present_swapchain = VK_NULL_HANDLE;
        VkImage present_composite_image = VK_NULL_HANDLE;
        VkImageLayout present_composite_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage present_images[kMaxSwapchainImages]{};
        uint32_t present_image_count = 0;
        int present_width = 0;
        int present_height = 0;
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

    // pending_buffer_update_barrier moved into FrameSync (per-slot).

    // Per-swapchain-image semaphores to avoid present engine conflicts.
    // Indexed by the acquired image index, not the frame sync index.
    // Bumped from 4 to 8 because Android compositors commonly return 5–6
    // swapchain images, which would OOB the smaller bound.
    VkSemaphore image_available_[kMaxSwapchainImages]{};
    VkSemaphore render_finished_[kMaxSwapchainImages]{};
    uint32_t acquire_semaphore_index_ = 0;

    // Bindless textures
    static constexpr uint32_t kMaxBindlessTextures = 1024;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

    // "Global buffer" descriptor set (set = 1): scene-global storage
    // buffers (BVH nodes/shapes today; GpuArena/GpuHive pages later) that
    // compute shaders read by index rather than by buffer_device_address.
    // A SINGLE frame-invariant set, bound by the primary and by every
    // (simultaneous-use) secondary command buffer that records compute.
    // Per-frame data variance lives in the buffer CONTENTS (refreshed in
    // place via IGpuArena), not the descriptor: producers rewrite the
    // descriptor only when a buffer's address changes (first bind + the
    // rare growth realloc), so steady state never touches it. The arena
    // writes it during prepare, after the set was bound, so the binding
    // is UPDATE_AFTER_BIND.
    VkDescriptorPool bound_buffer_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bound_buffer_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet bound_buffer_set_ = VK_NULL_HANDLE;
    VkSampler linear_sampler_ = VK_NULL_HANDLE;  ///< Default Repeat+Linear sampler. Kept as the fallback when no per-texture desc is supplied.
    uint32_t next_bindless_index_ = 1; // 0 reserved for "no texture"

    /// Recycled bindless slots from destroyed textures. `create_texture`
    /// pops from here before bumping `next_bindless_index_`; a slot is
    /// returned only once its texture's frame-completion marker resolves
    /// (in `drain_deferred_textures`), so no in-flight frame can still
    /// reference it when it is handed out again. Touched only on the main
    /// thread (create_texture + begin_frame's drain), so it needs no lock.
    ::velk::vector<uint32_t> free_bindless_indices_;

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

    /// Surface-create callback retained from init (VulkanInitParams). Stored
    /// so `recreate_surface` can build a fresh VkSurfaceKHR from a new native
    /// window (Android suspend/resume). Signature matches CreateSurfaceFn:
    /// bool(vk_instance, out_VkSurfaceKHR, user_data=native_handle).
    bool (*surface_create_fn_)(void*, void*, void*) = nullptr;
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
    /// VkGpuTexture / VkRenderTexture. `bindless_index` is returned to
    /// `free_bindless_indices_` for reuse once the marker resolves
    /// (UINT32_MAX / 0 = no slot, e.g. depth attachments).
    struct DeferredGpuTextureDestroy
    {
        ::VkImage         image;
        ::VkImageView     view;
        VmaAllocation     allocation;
        uint32_t          bindless_index;
        uint64_t          completion_marker;
    };
    ::velk::vector<DeferredGpuTextureDestroy> deferred_gpu_textures_;
    std::mutex deferred_gpu_textures_mutex_;

    void drain_deferred_textures();

    bool initialized_ = false;

    /// @name GPU timing (per-pass timestamp queries)
    /// One timestamp query pool shared across frame slots; each slot owns
    /// the contiguous range `[slot * 2*kMaxTimedPasses, ...)` so a slot's
    /// queries are only read after that slot's fence has fired. Disabled
    /// by default: when off nothing is recorded or read.
    /// @{
    static constexpr uint32_t kMaxTimedPasses = 48; ///< Max timed regions per frame.
    VkQueryPool timestamp_pool_ = VK_NULL_HANDLE;
    float timestamp_period_ns_ = 0.f;  ///< ns per timestamp tick; 0 = unsupported.
    bool gpu_timing_enabled_ = false;
    bool timer_region_open_ = false;   ///< True between begin/end_gpu_timer (main thread only).

    /// Per-slot recorded-region bookkeeping. `count` is the number of
    /// closed regions this slot recorded; `labels` tags each region for
    /// aggregation. `begin_frame` reads the prior frame's results using
    /// the surviving count, then resets count to 0 for the new frame.
    struct TimerSlot
    {
        uint32_t count = 0;
        const char* labels[kMaxTimedPasses] = {};
    };
    TimerSlot timer_slots_[kFrameOverlap];

    /// Last fully-resolved frame's per-pass durations, aggregated by
    /// label. Written at `begin_frame`, read by the overlay. Main thread.
    ::velk::vector<GpuPassTiming> last_timings_;

    /// Reads a completed slot's timestamps and aggregates them by label
    /// into `last_timings_`. Called at `begin_frame` after the slot's
    /// fence wait, before the pool range is reset.
    void resolve_gpu_timings(uint32_t slot);
    /// @}

    // Internal helpers
    bool create_vk_instance();
    bool select_physical_device();
    bool create_device();
    bool create_allocator();
    bool create_command_pool();
    bool create_sync_objects();
    bool create_bindless_descriptor();
    bool create_bound_buffer_descriptor();
    bool create_pipeline_layout();

    bool create_swapchain(SurfaceData& sd);
    void destroy_swapchain(SurfaceData& sd);

    /// Allocate the per-surface composite intermediate sized to the
    /// swapchain. Called after create_swapchain settles dimensions.
    bool create_surface_composite(uint64_t surface_id, SurfaceData& sd);
    void destroy_surface_composite(SurfaceData& sd);

    /// Acquires the next swap image and records the composite-to-swap blit
    /// onto the slot's primary, entirely from the @p sync snapshot taken at
    /// close_frame. Runs on the render thread (from submit_frame) so acquire +
    /// present stay on one thread and no shared SurfaceData is touched.
    /// Returns false if the swapchain is out of date (skip present).
    bool record_present_blit(FrameSync& sync);

    ::VkCommandBuffer begin_one_shot_commands();
    void end_one_shot_commands(::VkCommandBuffer cb);
    void transition_image_layout(::VkCommandBuffer cb, VkImage image, VkImageLayout old_layout,
                                 VkImageLayout new_layout, uint32_t mip_levels = 1);
};

} // namespace velk::vk

#endif // VELK_VK_BACKEND_H
