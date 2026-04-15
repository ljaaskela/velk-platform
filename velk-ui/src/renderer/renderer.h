#ifndef VELK_UI_RENDERER_IMPL_H
#define VELK_UI_RENDERER_IMPL_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <unordered_map>
#include <velk-render/detail/intf_renderer_internal.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_material.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/plugin.h>
#include <velk-render/render_types.h>
#include <velk-ui/interface/intf_camera.h>
#include <velk-ui/interface/intf_render_to_texture.h>
#include <velk-ui/interface/intf_renderer.h>
#include <velk-ui/interface/intf_scene.h>

#include <condition_variable>
#include <mutex>

namespace velk::ui {

class Renderer : public ::velk::ext::Object<Renderer, IRendererInternal, IRenderer, IGpuResourceObserver>
{
public:
    VELK_CLASS_UID(::velk::ClassId::Renderer, "Renderer");

    // IRendererInternal
    void set_backend(const IRenderBackend::Ptr& backend, IRenderContext* ctx) override;

    // IGpuResourceObserver
    void on_gpu_resource_destroyed(IGpuResource* resource) override;

    // IRenderer
    void add_view(const IElement::Ptr& camera_element, const IWindowSurface::Ptr& surface,
                  const rect& viewport) override;
    void remove_view(const IElement::Ptr& camera_element, const IWindowSurface::Ptr& surface) override;
    Frame prepare(const FrameDesc& desc) override;
    void present(Frame frame) override;
    void render() override;
    void set_max_frames_in_flight(uint32_t count) override;
    void shutdown() override;

private:
    struct VisualCommands
    {
        vector<DrawEntry> entries;
        uint64_t pipeline_override = 0;
        IMaterial::Ptr material;
    };

    struct ElementCache
    {
        vector<VisualCommands> before_visuals;  ///< VisualPhase::BeforeChildren
        vector<VisualCommands> after_visuals;   ///< VisualPhase::AfterChildren
        /// Weak references to GPU resources used by visuals on this element.
        /// IBuffer is the common base for anything uploadable: textures
        /// (which inherit IBuffer via ITexture) and plain shader-readable
        /// byte buffers. The renderer does NOT extend resource lifetimes:
        /// the resource's real owner (visual, image cache, font, etc.)
        /// decides when it dies. At use time the upload loop locks each
        /// weak_ptr to a temporary strong ref so the resource cannot vanish
        /// mid-call. When a resource dies (on any thread), the observer
        /// callback removes it from texture_map_ / buffer_map_ and enqueues
        /// its GPU handle for deferred destruction.
        vector<IBuffer::WeakPtr> gpu_resources;
    };

    struct Batch
    {
        uint64_t pipeline_key = 0;
        uint64_t texture_key = 0;
        vector<uint8_t> instance_data;
        uint32_t instance_stride = 0;
        uint32_t instance_count = 0;
        IMaterial::Ptr material;
    };

    struct ViewEntry
    {
        IElement::Ptr camera_element;
        IWindowSurface::Ptr surface;
        rect viewport;
        bool batches_dirty = true;
        int cached_width = 0;
        int cached_height = 0;
        vector<Batch> batches;  ///< Cached batches for this view; rebuilt when dirty.
    };

    /** @brief Target for a render pass: either a swapchain surface or an off-screen texture. */
    struct RenderTarget
    {
        IRenderTarget::Ptr target;
    };

    /** @brief A single render pass captured by prepare() for present(). */
    struct RenderPass
    {
        RenderTarget target;
        rect viewport;
        vector<DrawCall> draw_calls;
    };

    /** @brief A prepared frame waiting to be presented. */
    struct FrameSlot
    {
        uint64_t id = 0;
        vector<RenderPass> passes;
        bool ready = false;         ///< Prepared and waiting for present.
        uint64_t presented_at = 0;  ///< Frame counter when this slot was presented (0 = free).

        GpuBuffer frame_buffer{};
        void* frame_ptr = nullptr;
        uint64_t frame_gpu_base = 0;
        size_t buffer_size = 0;  ///< Current size of frame_buffer in bytes.
    };

    void rebuild_commands(IElement* element);
    void rebuild_batches(const SceneState& state, ViewEntry& entry);
    void build_draw_calls(const ViewEntry& entry);
    void prepend_environment_batch(ICamera& camera, ViewEntry& entry);

    uint64_t write_to_frame_buffer(const void* data, size_t size, size_t alignment = 16);

    /** @brief Checks if a view matches the FrameDesc filter. */
    bool view_matches(const ViewEntry& entry, const FrameDesc& desc) const;

    IRenderBackend::Ptr backend_;
    IRenderContext* render_ctx_ = nullptr;
    vector<ViewEntry> views_;
    std::unordered_map<IElement*, ElementCache> element_cache_;

    const std::unordered_map<uint64_t, PipelineId>* pipeline_map_ = nullptr;

    static constexpr size_t kInitialFrameBufferSize = 1024 * 1024;
    size_t frame_buffer_size_ = 0;
    size_t write_offset_ = 0;
    size_t peak_usage_ = 0;
    bool frame_overflow_ = false;       ///< Set when a write overflows; triggers retry with larger buffer.
    FrameSlot* active_slot_ = nullptr;  ///< Slot being filled by prepare().

    void ensure_frame_buffer_capacity();
    void grow_frame_buffer();
    void init_slot_buffers(FrameSlot& slot);

    uint64_t globals_gpu_addr_ = 0;  ///< Per-view, written into the staging buffer during prepare().

    /// Per-texture state. Keyed by ISurface*. The renderer observes each
    /// key via add_gpu_resource_observer; on destruction notification, the
    /// entry is removed and the TextureId is enqueued in deferred_destroy_.
    std::unordered_map<ISurface*, TextureId> texture_map_;

    /// Per-buffer state for non-texture IBuffer resources (e.g. font curve
    /// buffers). Keyed by IBuffer*. Holds renderer-internal bookkeeping that
    /// doesn't belong on IBuffer itself: the backend GpuBuffer handle (used
    /// to map and to destroy) and the last-uploaded size (used to detect a
    /// CPU-side regrow that requires reallocating the GPU buffer). The GPU
    /// virtual address lives on the IBuffer itself via set_gpu_address.
    struct BufferEntry
    {
        GpuBuffer handle{};
        size_t size = 0;
    };
    std::unordered_map<IBuffer*, BufferEntry> buffer_map_;

    /** @brief Per render-to-texture trait: GPU texture management. */
    struct RenderTargetEntry
    {
        IRenderTarget::Ptr target;
        TextureId texture_id = 0;
        int width = 0;
        int height = 0;
        bool dirty = true;
    };
    std::unordered_map<IElement*, RenderTargetEntry> render_target_entries_;

    /** @brief Visual entries for a render target pass, collected during pre-filter. */
    struct RenderTargetPassData
    {
        IElement* element = nullptr;
        vector<VisualListEntry> before_entries;
        vector<VisualListEntry> after_entries;
        vector<Batch> batches;
    };
    vector<RenderTargetPassData> render_target_passes_;

    struct DeferredTextureDestroy
    {
        TextureId tid;
        uint64_t safe_after_frame; ///< destroy when present_counter_ > this
    };
    struct DeferredBufferDestroy
    {
        GpuBuffer handle;
        uint64_t safe_after_frame;
    };
    /// Cross-thread safe queues of GPU handles waiting for the safe window
    /// before destruction. Pushed by on_gpu_resource_destroyed (any thread)
    /// or by the upload path when a buffer is regrown, drained on the
    /// renderer thread at frame start.
    vector<DeferredTextureDestroy> deferred_destroy_;
    vector<DeferredBufferDestroy> deferred_buffer_destroy_;
    std::mutex deferred_destroy_mutex_;

    void drain_deferred_destroy();

    vector<DrawCall> draw_calls_;
    /// Environment textures we've observed (not tracked via element_cache_).
    /// Unregistered in shutdown() to prevent the GpuResource dtor from
    /// calling back into a dead renderer during resource store teardown.
    vector<IBuffer::WeakPtr> observed_env_resources_;

    static constexpr uint64_t kGpuLatencyFrames = 3;  ///< Frames to wait before reusing a slot's GPU buffer.
    static constexpr uint32_t kDefaultMaxFramesInFlight = kGpuLatencyFrames + 1;
    vector<FrameSlot> frame_slots_{kDefaultMaxFramesInFlight};
    uint64_t next_frame_id_ = 1;
    uint64_t present_counter_ = 0;
    std::mutex slot_mutex_;
    std::condition_variable slot_cv_;
};

} // namespace velk::ui

#endif // VELK_UI_RENDERER_IMPL_H
