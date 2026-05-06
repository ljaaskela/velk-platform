#ifndef VELK_SCENE_INTF_RENDER_PATH_H
#define VELK_SCENE_INTF_RENDER_PATH_H

#include <velk/interface/intf_interface.h>
#include <velk/string.h>
#include <velk/uid.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_gpu_resource.h>

#include <velk-render/interface/intf_render_graph.h>
#include <velk-render/interface/intf_render_stage.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/frame/render_view.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_view_entry.h>

namespace velk {

/**
 * @brief Per-view rendering strategy attached to a camera element.
 *
 * Implementations turn a single view (camera + surface + viewport +
 * scene state) into zero or more `RenderPass` records that the renderer
 * submits to the backend. Built-in paths cover Forward, Deferred, and
 * RT (compute-shader path tracer); plugins can register additional
 * paths and attach them to camera elements like any other trait.
 *
 * Lifetime: owned by the camera element via `add_attachment`. The
 * Renderer discovers the path via `find_attachment<IRenderPath>` per
 * view per frame; no attachment falls back to a built-in `Forward`
 * path so trivial UI samples don't have to opt in explicitly.
 *
 * Per-view state: implementations typically keep an
 * `unordered_map<IViewEntry*, ViewState>` keyed by the stable
 * `IViewEntry*`. The renderer calls `on_view_removed` so paths can
 * release per-view GPU resources, and `shutdown` so paths can release
 * everything during renderer teardown.
 */
class IRenderPath
    : public Interface<IRenderPath, IRenderStage,
                       VELK_UID("15116d0d-6a52-40b4-94f9-f5ab9ffc133f")>
{
public:
    /**
     * @brief Per-path declaration of which scene-side data the
     *        preparer should collect into the path's `RenderView`.
     *
     * Static: returned by `needs()` and queried once per dispatch.
     * Defaults to "nothing" — concrete paths opt into the collections
     * they actually consume so trivial UI scenes don't pay for RT
     * shape walks they'll never use.
     */
    struct Needs
    {
        bool batches = false; ///< Raster batch list (Forward + Deferred).
        bool shapes  = false; ///< RT primary-buffer shapes (RT).
        bool lights  = false; ///< Scene lights + shadow-tech registration (Deferred + RT).
    };

    /// What this path consumes from `RenderView`. ViewPreparer reads this
    /// once per dispatch and skips collections the path doesn't want.
    /// Use `velk::ext::RenderPath<FinalClass>` for an empty default.
    virtual Needs needs() const = 0;

    /**
     * @brief Appends zero or more passes for @p view to @p out_passes.
     *
     * Called once per frame per view by the owning view pipeline. The
     * implementation may maintain per-view state keyed off `&view`.
     * @p render_view is a flat snapshot of resolved scene data for this
     * view (camera matrices, lights, env, raster batches, BVH addresses)
     * paths consume this exclusively; no scene-graph reach-back.
     *
     * @p color_target is the final color destination chosen by the
     * pipeline. Paths must render to / blit into this target rather than
     * assume `view.surface`. Today it equals `view.surface`; with
     * post-process chains it becomes an intermediate target; with the
     * future transient pool it becomes a graph-allocated Ptr.
     *
     * Passes are appended via `graph.add_pass(...)`. The graph tracks
     * resource flow and inserts barriers; paths don't need to think
     * about ordering with other paths or with RTT subtree passes.
     */
    virtual void build_passes(IViewEntry& view,
                              const RenderView& render_view,
                              IRenderTarget::Ptr color_target,
                              FrameContext& ctx,
                              IRenderGraph& graph) = 0;

    /**
     * @brief Looks up a per-view named output produced by this path.
     *
     * Generic introspection hook for debug overlays / external readback.
     * Names are path-specific; conventional names today:
     *   - "gbuffer"           — the deferred G-buffer
     *                           (IRenderTextureGroup; iterate via
     *                           `attachment_count()` / `attachment(i)`).
     *   - "gbuffer.worldpos"  — IRenderTarget alias of the worldpos
     *                           attachment (debug readback / overlay).
     *   - "shadow.debug"      — RT-shadow debug storage texture
     *                           (IRenderTarget).
     *
     * Returns null when the path doesn't produce that output for the
     * given view. Non-introspecting paths can leave the empty default
     * provided by `ext::RenderPath`.
     */
    virtual IGpuResource::Ptr find_named_output(string_view name,
                                                IViewEntry* view) const = 0;
};

} // namespace velk

#endif // VELK_SCENE_INTF_RENDER_PATH_H
