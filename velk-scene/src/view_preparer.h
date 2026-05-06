#ifndef VELK_UI_VIEW_PREPARER_H
#define VELK_UI_VIEW_PREPARER_H

#include <velk/vector.h>

#include <unordered_map>
#include <unordered_set>

#include "batch_builder.h"

#include <velk-render/ext/render_state.h>
#include <velk-render/interface/intf_batch.h>
#include <velk-render/frame/render_view.h>
#include <velk-scene/interface/intf_scene_observer.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_render_path.h>
#include <velk-render/interface/intf_render_state.h>
#include <velk-render/interface/intf_view_entry.h>
#include <velk-scene/interface/intf_element.h>

namespace velk {

/**
 * @brief Walks a `IViewEntry` + scene state and produces a flat `RenderView`.
 *
 * Centralises everything a path used to do for itself before emitting
 * GPU passes:
 *   - Resolves the camera into view/inverse_view-projection matrices
 *     plus world position.
 *   - Computes pixel viewport from surface size + viewport rect.
 *   - Writes `FrameGlobals` to the frame buffer; returns its address.
 *   - Rebuilds the raster batch cache when `entry.batches_dirty` is
 *     set (cache lifetime is per-view, persistent across frames).
 *   - Collects scene lights into `RenderView::lights`.
 *   - Resolves the camera's environment into `RenderView::env`.
 *
 * RT shape collection still lives on `RtPath` for now; migrates in 3.3.
 *
 * Lifetime: the per-view batch cache is keyed by `IViewEntry*`.
 * `on_view_removed` releases it; `clear()` releases all (called on
 * Renderer shutdown).
 */
class ViewPreparer
    : public ::velk::ext::RenderState<ViewPreparer, IRenderState, IRenderStateObserver>
{
public:
    ~ViewPreparer();

    /// Builds a `RenderView` for @p entry. @p camera_element is the
    /// scene-side camera the entry was registered against (needed for
    /// camera + env resolution but not exposed to paths). @p needs
    /// declares which optional collections (batches, shapes, lights)
    /// the active path will consume.
    RenderView prepare(IViewEntry& entry,
                       const IElement::Ptr& camera_element,
                       const SceneState& scene_state,
                       FrameContext& ctx,
                       BatchBuilder& batch_builder,
                       const IRenderPath::Needs& needs);

    /// Releases the per-view batch cache for @p entry. Each batch
    /// owns its own GpuBufferHandle; dropping the cache's IBatch::Ptrs
    /// cascades to the resource manager's deferred-destroy queue.
    void on_view_removed(IViewEntry& entry);

    /// Releases all per-view caches. Called on Renderer shutdown.
    void clear();

    /// IRenderStateObserver — re-emits a child view's change event up
    /// the chain so subscribers of this preparer (e.g. the renderer)
    /// see the same notification flow without holding refs to each
    /// individual view.
    void on_render_state_changed(IRenderState* source,
                                 RenderStateChange flags) override;

    // Diagnostic counters. Read by Renderer's optional per-second log
    // and reset there after each report.
    uint64_t diag_rebuild_count = 0;
    uint64_t diag_fast_path_count = 0;
    uint64_t diag_fast_path_failed = 0;

    /// @name Diagnostic accessors. Used by Renderer's optional
    ///       per-second log; all O(1) reads of internal state.
    /// @{
    size_t view_count() const { return view_caches_.size(); }
    size_t total_batches() const
    {
        size_t n = 0;
        for (auto& [k, v] : view_caches_) n += v.batches.size();
        return n;
    }
    size_t total_element_slots() const
    {
        size_t n = 0;
        for (auto& [k, v] : view_caches_) n += v.element_slots.size();
        return n;
    }
    /// @}

private:
    struct ViewCache
    {
        vector<IBatch::Ptr> batches;
        BatchBuilder::ElementSlotMap element_slots;
        std::unordered_set<IElement*> rtt_roots;
        /// Cached forward-env batch + the IEnvironment* it was built
        /// against. Reused across frames so prepare_env doesn't
        /// allocate a fresh DefaultBatch every frame; rebuilt only
        /// when the camera's environment attachment changes.
        IBatch::Ptr env_batch;
        const void* env_material_key = nullptr;
    };
    std::unordered_map<IViewEntry*, ViewCache> view_caches_;

    /// Try the transform-only fast path for @p entry. Returns true if
    /// @p scene_state's flags + dirty list permit incremental update
    /// (Layout-only changes, no structural mutations) and every dirty
    /// element was found in the cached slot map. Falls back to full
    /// rebuild when this returns false. update_instance_at writes
    /// through each batch's mapped pointer directly into the same
    /// host-coherent memory the GPU reads — fast path is valid for
    /// transform-only churn because each batch owns its own buffer.
    bool try_update_transforms(IViewEntry& entry,
                               const SceneState& scene_state);

    /// Rebuilds the per-view raster batch cache when @p entry.batches_dirty
    /// is set, uploads any dirty batches' instance buffers via the
    /// resource manager (giving each batch a stable GPU address), then
    /// points @p rv at the cache.
    void prepare_batches(IViewEntry& entry, const SceneState& scene_state,
                         BatchBuilder& batch_builder, FrameContext& ctx,
                         RenderView& rv);

    /// Resolves the camera (or ortho fallback) into view-projection /
    /// inverse-VP / cam_pos / frustum on @p rv, using @p entry's
    /// surface size and viewport rect.
    void prepare_camera(IViewEntry& entry, const IElement::Ptr& camera_element,
                        FrameContext& ctx, RenderView& rv);

    /// Writes the per-view FrameGlobals block to @p ctx.frame_buffer
    /// and stores its GPU address on @p rv.
    void prepare_frame_globals(FrameContext& ctx, RenderView& rv);

    /// Walks scene lights, registers each light's shadow tech with the
    /// snippet registry, and accumulates GpuLight records into @p rv.
    void prepare_lights(const SceneState& scene_state, FrameContext& ctx,
                        RenderView& rv);

    /// Walks scene shapes, resolves material / texture / intersect /
    /// mesh-data per shape, and accumulates RtShape records into @p rv.
    void prepare_shapes(const SceneState& scene_state, FrameContext& ctx,
                        RenderView& rv);

    /// Resolves the camera's environment (texture + material) into @p rv.env.
    /// Also stamps @p rv.env_batch from the per-view cache, rebuilding
    /// only when the env material changes.
    void prepare_env(IViewEntry& entry,
                     const IElement::Ptr& camera_element,
                     FrameContext& ctx, RenderView& rv);
};

} // namespace velk

#endif // VELK_UI_VIEW_PREPARER_H
