#ifndef VELK_UI_RENDERER_IMPL_H
#define VELK_UI_RENDERER_IMPL_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <unordered_map>
#include <unordered_set>
#include "batch_builder.h"
#include "render_target_cache.h"
#include "view_preparer.h"
#include <velk-render/interface/intf_render_graph.h>
#include <velk-render/frame/render_view.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_render_path.h>
#include <velk-render/interface/intf_view_pipeline.h>
#include <velk-render/render_path/view_entry.h>
#include <velk-render/detail/intf_gpu_resource_manager_internal.h>
#include <velk-render/detail/intf_render_graph_internal.h>
#include <velk-render/detail/intf_renderer_internal.h>
#include <velk-render/interface/intf_frame_data_manager.h>
#include <velk-render/interface/intf_frame_snippet_registry.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/plugin.h>
#include <velk-render/render_types.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-scene/interface/intf_render_to_texture.h>
#include <velk-scene/interface/intf_renderer.h>
#include <velk-scene/interface/intf_scene.h>
#include "scene_bvh.h"

#include <condition_variable>
#include <mutex>

namespace velk {

class Renderer : public ::velk::ext::Object<Renderer, IRendererInternal, IRenderer>
{
public:
    VELK_CLASS_UID(::velk::ClassId::Renderer, "Renderer");

    ~Renderer() override;

    // IRendererInternal
    void set_backend(const IRenderBackend::Ptr& backend, IRenderContext* ctx) override;
    uint64_t consume_last_prepare_gpu_wait_ns() override;

    // IRenderer
    void add_view(const IElement::Ptr& camera_element, const IWindowSurface::Ptr& surface,
                  const rect& viewport) override;
    void remove_view(const IElement::Ptr& camera_element, const IWindowSurface::Ptr& surface) override;
    Frame prepare(const FrameDesc& desc) override;
    void present(Frame frame) override;
    void render() override;
    void set_max_frames_in_flight(uint32_t count) override;
    void add_debug_overlay(const IWindowSurface::Ptr& surface,
                           TextureId texture_id,
                           const rect& dst_rect) override;
    void clear_debug_overlays() override;
    IGpuResource::Ptr get_named_output(const IElement::Ptr& camera_element,
                                       const IWindowSurface::Ptr& surface,
                                       string_view name) const override;
    void request_bvh_log() override { log_bvh_next_ = true; }
    void shutdown() override;

private:
    struct FrameSlot
    {
        uint64_t id = 0;
        IRenderGraph::Ptr graph;
        bool ready = false;
        uint64_t presented_at = 0;
        /// Backend-issued GPU completion marker (e.g. timeline value)
        /// stamped right after the slot's last end_frame submit. The
        /// claim path waits on this before reusing the slot, so the
        /// CPU can never trample buffers the GPU still reads. 0 means
        /// the slot has never been submitted.
        uint64_t gpu_completion_marker = 0;
        IFrameDataManager::Slot buffer;
    };

    FrameSlot* claim_frame_slot();
    /// Scene-side per-view bookkeeping. `entry` is the velk-render-pure
    /// ViewEntry handed to paths; `camera_element` is the IElement
    /// the entry was registered against (used by Renderer + ViewPreparer
    /// for camera/env lookups, never by paths).
    struct ViewSlot
    {
        IElement::Ptr camera_element;
        ViewEntry entry;
    };

    std::unordered_map<IScene*, SceneState> consume_scenes(const FrameDesc& desc);
    void build_frame_passes(const FrameDesc& desc,
                            std::unordered_map<IScene*, SceneState>& consumed_scenes,
                            FrameSlot& slot);
    bool view_matches(const ViewSlot& slot, const FrameDesc& desc) const;
    FrameContext make_frame_context();

    IRenderBackend::Ptr backend_;
    IRenderContext* render_ctx_ = nullptr;

    // resources_ must outlive any member that holds IProgram::Ptr
    // refs (views_, batch_builder_): material dtors invoke
    // on_gpu_resource_destroyed which calls into resources_.
    IGpuResourceManager::Ptr resources_;

    vector<ViewSlot> views_;
    const PipelineCacheMap* pipeline_map_ = nullptr;

    struct DebugOverlay {
        IWindowSurface::Ptr surface;
        TextureId texture_id = 0;
        rect dst_rect{};
    };
    vector<DebugOverlay> debug_overlays_;

    /// Per-frame snapshot of one view's prepare-phase result. Carried
    /// from build_frame_passes' Phase 1 (rebuild_batches + pipeline
    /// discovery + BVH state capture) into Phase 2 (pipeline emit
    /// after RTT passes are in the graph). Reused across frames via
    /// `prepared_views_`'s capacity to avoid per-frame heap churn.
    struct PreparedView
    {
        ViewSlot* slot = nullptr;
        RenderView render_view{};
        IInterface::Ptr camera_trait;
        vector<IViewPipeline::Ptr> pipelines;
        uint64_t bvh_nodes_addr = 0;
        uint64_t bvh_shapes_addr = 0;
        uint32_t bvh_root = 0;
        uint32_t bvh_node_count = 0;
        uint32_t bvh_shape_count = 0;
    };
    vector<PreparedView> prepared_views_;

    /// Scratch buffer for per-view pipeline discovery. Cleared and
    /// reused inside the prepare loop to avoid one heap allocation
    /// per matched view.
    vector<IViewPipeline::Ptr> pipelines_scratch_;

    // One-shot flag set by request_bvh_log(), consumed in the next
    // BVH-emit cb. The cb walks every mesh instance and prints
    // (instance_index, buffer_addr, ibo_offset, triangle_count) so the
    // log can be compared against the F12-dumped shadow_debug image.
    bool log_bvh_next_ = false;

    /// Duration the most recent claim_frame_slot() spent blocked on
    /// `wait_for_frame_completion`. Read-and-cleared by the perf
    /// overlay through `consume_last_prepare_gpu_wait_ns`.
    uint64_t last_prepare_gpu_wait_ns_ = 0;

    // Diagnostic counters (opt-in via env var VELK_RENDER_DIAG=1).
    // Every kDiagPeriod frames we dump rebuild / fast-path counts and
    // sizes of caches that could leak. Helps narrow down "CPU time
    // growing over time" symptoms without a real profiler.
    struct DiagStats
    {
        uint64_t frames = 0;
        uint64_t rebuild_count = 0;
        uint64_t fast_path_count = 0;
        uint64_t fast_path_failed = 0;
    };
    DiagStats diag_;
    bool diag_enabled_ = false;
    void log_diagnostics();

    // Shared visual-command / gpu-resource cache. Both the dirty-resource
    // upload in consume_scenes and the raster paths read from it, so it
    // lives here rather than inside a single sub-renderer.
    BatchBuilder batch_builder_;

    // Per-frame material upload dedup cache. Cleared once per frame by
    // Renderer; exposed to paths via FrameContext::material_cache.
    MaterialAddrCache material_cache_;

    // Frame data buffer (per-slot GPU staging). Slot lifecycle is on the
    // interface, so a single Ptr is sufficient.
    IFrameDataManager::Ptr frame_buffer_;

    FrameSlot* active_slot_ = nullptr;

    // Shared per-frame scene snippet registry (materials + shadow
    // techniques). Owned here so both the scene-wide BVH build and
    // the sub-renderers (RayTracer composer, DeferredLighter light
    // resolution) read from the same stable ids.
    IFrameSnippetRegistry::Ptr snippets_;

    // Per-scene concrete SceneBvh pointer cache. The attachment on the
    // scene root owns the lifetime; we just cache the typed pointer so
    // rebuild() can be called without a round-trip through IBvh. Stale
    // entries for dead scenes are harmless because we only touch entries
    // whose scene is currently in `consumed_scenes`.
    std::unordered_map<IScene*, impl::SceneBvh*> scene_bvh_cache_;

    // Shared RTT (render-to-texture) subtree cache. Used by Forward +
    // Deferred raster paths via FrameContext.
    RenderTargetCache render_target_cache_;

    // Per-view scene-data preparer. Walks scene state once per view
    // per frame and produces a flat `RenderView` that paths consume.
    // Owns the per-view raster batch cache.
    ViewPreparer view_preparer_;

    // Set of pipelines the Renderer has dispatched to during this run.
    // Populated by build_frame_passes; iterated for lifecycle hooks
    // (on_view_removed, shutdown) so we don't have to track which
    // pipeline was last used by which view.
    std::unordered_set<IViewPipeline*> seen_pipelines_;

    static constexpr uint32_t kDefaultMaxFramesInFlight = 4;
    vector<FrameSlot> frame_slots_{kDefaultMaxFramesInFlight};
    uint64_t next_frame_id_ = 1;
    uint64_t present_counter_ = 0;
    std::mutex slot_mutex_;
    std::condition_variable slot_cv_;
};

} // namespace velk

#endif // VELK_UI_RENDERER_IMPL_H
