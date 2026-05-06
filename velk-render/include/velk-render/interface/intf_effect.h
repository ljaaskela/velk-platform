#ifndef VELK_RENDER_INTF_EFFECT_H
#define VELK_RENDER_INTF_EFFECT_H

#include <velk/uid.h>

#include <velk-render/interface/intf_render_graph.h>
#include <velk-render/interface/intf_render_stage.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_view_entry.h>

namespace velk {

/**
 * @brief A shader with predefined inputs and outputs.
 *
 * Conceptually: a single computation node — read input texture(s),
 * write output texture(s). Today the contract is single color in /
 * single color out (matches what tonemap, FXAA, vignette, simple
 * filters need); additional inputs (depth, motion vectors, prior
 * frame) get added as new emit() parameters when the first such
 * effect lands.
 *
 * `IEffect` is the leaf type. It's composed by an `IPostProcess`
 * (the camera / view-pipeline-tied container) but is intentionally
 * kept generic so it can also stand on its own outside the post-
 * process pipeline — image-processing chains, generative texture
 * pipelines, compute-only views.
 *
 * Sharing: an effect Ptr can be attached to multiple `IPostProcess`
 * containers. Per-view state (if any) keyed off `IViewEntry*` via the
 * inherited `IRenderStage` lifecycle hooks.
 *
 * Chain: IInterface -> IRenderStage -> IEffect
 */
class IEffect
    : public Interface<IEffect, IRenderStage,
                       VELK_UID("a573ea40-890d-4f4f-8edf-75e3cb1c4d97")>
{
public:
    /**
     * @brief Emits passes that read @p input and write @p output.
     *
     * @param view    Per-view identity for state-keyed effects.
     * @param input   Source color target. Storage texture provided by
     *                the container (writable + sampleable).
     * @param output  Destination color target. Storage texture
     *                provided by the container.
     * @param ctx     Shared per-frame context.
     * @param graph   Frame's render graph.
     */
    virtual void emit(IViewEntry& view,
                      IRenderTarget::Ptr input,
                      IRenderTarget::Ptr output,
                      FrameContext& ctx,
                      IRenderGraph& graph) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_EFFECT_H
