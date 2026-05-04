#ifndef VELK_UI_VIEW_PREPARER_H
#define VELK_UI_VIEW_PREPARER_H

#include <velk/vector.h>

#include <unordered_map>

#include <velk-render/interface/intf_batch.h>
#include <velk-render/frame/render_view.h>
#include <velk-scene/interface/intf_scene_observer.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_render_path.h>
#include <velk-scene/interface/intf_element.h>
#include <velk-render/render_path/view_entry.h>

namespace velk { class BatchBuilder; }

namespace velk {

/**
 * @brief Walks a `ViewEntry` + scene state and produces a flat `RenderView`.
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
 * Lifetime: the per-view batch cache is keyed by `ViewEntry*`.
 * `on_view_removed` releases it; `clear()` releases all (called on
 * Renderer shutdown).
 */
class ViewPreparer
{
public:
    /// Builds a `RenderView` for @p entry. @p camera_element is the
    /// scene-side camera the entry was registered against (needed for
    /// camera + env resolution but not exposed to paths). @p needs
    /// declares which optional collections (batches, shapes, lights)
    /// the active path will consume.
    RenderView prepare(ViewEntry& entry,
                       const IElement::Ptr& camera_element,
                       const SceneState& scene_state,
                       FrameContext& ctx,
                       BatchBuilder& batch_builder,
                       const IRenderPath::Needs& needs);

    /// Releases the per-view batch cache for @p entry.
    void on_view_removed(ViewEntry& entry);

    /// Releases all per-view caches. Called on Renderer shutdown.
    void clear();

private:
    struct ViewCache
    {
        vector<IBatch::Ptr> batches;
    };
    std::unordered_map<ViewEntry*, ViewCache> view_caches_;

    /// Rebuilds the per-view raster batch cache when @p entry.batches_dirty
    /// is set, then points @p rv at it.
    void prepare_batches(ViewEntry& entry, const SceneState& scene_state,
                         BatchBuilder& batch_builder, RenderView& rv);

    /// Resolves the camera (or ortho fallback) into view-projection /
    /// inverse-VP / cam_pos / frustum on @p rv, using @p entry's
    /// surface size and viewport rect.
    void prepare_camera(ViewEntry& entry, const IElement::Ptr& camera_element,
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
    void prepare_env(const IElement::Ptr& camera_element, FrameContext& ctx,
                     RenderView& rv);
};

} // namespace velk

#endif // VELK_UI_VIEW_PREPARER_H
