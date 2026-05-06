#ifndef VELK_RENDER_INTF_POST_PROCESS_H
#define VELK_RENDER_INTF_POST_PROCESS_H

#include <velk/uid.h>

#include <velk-render/interface/intf_render_graph.h>
#include <velk-render/interface/intf_render_stage.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_view_entry.h>

namespace velk {

/**
 * @brief A camera / view-pipeline-tied post-process container.
 *
 * Composes a list of `IEffect`s into the camera's color output.
 * Concrete subclasses define how the composition runs — the default
 * `PostProcess` runs effects linearly, future variants may run them
 * as a DAG. The container owns the per-view intermediate textures
 * needed for ping-pong between effects.
 *
 * One IPostProcess Ptr can be attached to multiple `IViewPipeline`s;
 * per-view state keyed off `IViewEntry*` via the inherited
 * `IRenderStage` lifecycle hooks. A user can build a complex effect
 * setup once and apply it to every camera.
 *
 * The single-input / single-output contract on `emit` matches
 * `IEffect` so containers can compose effects without seam friction.
 * Effects are the leaves; this is the camera-facing container.
 *
 * Chain: IInterface -> IRenderStage -> IPostProcess
 */
class IPostProcess
    : public Interface<IPostProcess, IRenderStage,
                       VELK_UID("c54c4b8c-8dcd-49c6-9dc9-a1ea39f62359")>
{
public:
    /**
     * @brief Emits passes that read @p input and write @p output.
     *
     * @param view    Per-view identity. Stages with view-keyed state use
     *                this as the lookup key for their per-view maps.
     * @param input   Source color target. May be null when called as the
     *                first stage of a chain that doesn't read prior color.
     * @param output  Destination color target. The final stage in a
     *                composition writes here; intermediate stages get
     *                container-allocated temporaries.
     * @param ctx     Shared per-frame context.
     * @param graph   The frame's render graph; passes are appended via
     *                `graph.add_pass(...)`.
     */
    virtual void emit(IViewEntry& view,
                      IRenderTarget::Ptr input,
                      IRenderTarget::Ptr output,
                      FrameContext& ctx,
                      IRenderGraph& graph) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_POST_PROCESS_H
