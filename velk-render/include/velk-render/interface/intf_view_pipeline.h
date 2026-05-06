#ifndef VELK_RENDER_INTF_VIEW_PIPELINE_H
#define VELK_RENDER_INTF_VIEW_PIPELINE_H

#include <velk/interface/intf_interface.h>
#include <velk/uid.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_render_graph.h>
#include <velk-render/frame/render_view.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_render_path.h>
#include <velk-render/interface/intf_view_entry.h>

namespace velk {

/**
 * @brief Per-view orchestrator. Composes the stages that produce GPU
 *        work for a single view (path today; path + post-process chain
 *        + view-scoped compute later).
 *
 * Renderer doesn't know about IRenderPath / IPostProcess directly. It
 * iterates `find_attachments<IViewPipeline>` on the camera trait and
 * calls `emit` on each. Stage knowledge lives in the pipeline.
 *
 * Multiple pipelines may be attached to one camera (e.g. main pipeline
 * every frame, half-rate pipeline every other frame). Each pipeline
 * self-gates inside `emit`; the Renderer iterates unconditionally.
 *
 * The `color_target` argument is the final destination for this view's
 * color output. Today it is the camera's window surface; with
 * post-process chains it becomes an intermediate target chosen by the
 * pipeline; with the future Tier 2 transient pool it becomes a
 * graph-allocated Ptr. Pipelines pass it down to stages so paths never
 * assume `entry.surface` is the target.
 */
class IViewPipeline
    : public Interface<IViewPipeline, IInterface,
                       VELK_UID("921ac7ee-c1bf-45b2-91da-02362f142490")>
{
public:
    /**
     * @brief Aggregate of stage `Needs` for the ViewPreparer.
     *
     * Returned once per dispatch and unioned across all pipelines
     * attached to a camera trait. @p ctx carries `view_camera_trait`
     * so the pipeline can resolve its currently-attached stages.
     *
     * Use `velk::ext::ViewPipeline<FinalClass>` for an empty default.
     */
    virtual IRenderPath::Needs needs(const FrameContext& ctx) const = 0;

    /**
     * @brief Emit GPU work for this view into @p out_passes.
     *
     * @param view         Per-view identity (surface + viewport + dirty cache).
     * @param render_view  Flat per-view scene snapshot (matrices, lights, batches).
     * @param color_target Final color destination for this view. Forwarded to
     *                     stages instead of `entry.surface`.
     * @param ctx          Shared per-frame context (backend, resources, BVH addrs).
     * @param graph        Frame's RenderGraph. Pipelines append passes via
     *                     `graph.add_pass(...)`; the graph orders execution
     *                     and inserts barriers.
     */
    virtual void emit(IViewEntry& view,
                      const RenderView& render_view,
                      IRenderTarget::Ptr color_target,
                      FrameContext& ctx,
                      IRenderGraph& graph) = 0;

    /** @brief Hook called when a view is removed. Release per-view state. */
    virtual void on_view_removed(IViewEntry& view, FrameContext& ctx) = 0;

    /** @brief Hook called on Renderer shutdown. Release all remaining state. */
    virtual void shutdown(FrameContext& ctx) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_VIEW_PIPELINE_H
