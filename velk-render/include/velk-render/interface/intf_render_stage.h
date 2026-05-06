#ifndef VELK_RENDER_INTF_RENDER_STAGE_H
#define VELK_RENDER_INTF_RENDER_STAGE_H

#include <velk/interface/intf_interface.h>
#include <velk/uid.h>

#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_view_entry.h>

namespace velk {

/**
 * @brief Common base for per-view rendering stages (`IRenderPath`,
 *        `IPostProcess`, future kinds).
 *
 * Stages are objects a view pipeline composes to produce GPU work for
 * one view. They typically maintain per-view state keyed off
 * `IViewEntry*` (caches, intermediate textures) and need lifecycle
 * notifications when a view goes away or the renderer shuts down.
 *
 * The shared surface is just lifecycle. The actual production method
 * differs per stage kind: `IRenderPath::build_passes` consumes a
 * `RenderView` (scene snapshot); `IPostProcess::emit` consumes an
 * input target. They stay on their respective leaf interfaces.
 *
 * Sharing: a stage Ptr can be attached to multiple pipelines / cameras.
 * Per-view state keying makes this safe; lifecycle hooks free state
 * for an individual view without affecting others still using the
 * shared stage.
 *
 * Chain: IInterface -> IRenderStage
 */
class IRenderStage
    : public Interface<IRenderStage, IInterface,
                       VELK_UID("0fc239f9-47c3-40b8-959b-2d857e5e962d")>
{
public:
    /** @brief Hook called when a view is removed. Release per-view state. */
    virtual void on_view_removed(IViewEntry& view, FrameContext& ctx) = 0;

    /** @brief Hook called on Renderer shutdown. Release all remaining state. */
    virtual void shutdown(FrameContext& ctx) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_STAGE_H
