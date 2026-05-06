#ifndef VELK_UI_FORWARD_PATH_H
#define VELK_UI_FORWARD_PATH_H

#include <velk/vector.h>

#include <velk-render/interface/intf_batch.h>
#include <velk-render/frustum.h>
#include <velk-render/plugin.h>
#include <velk-render/ext/render_path.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_render_path.h>
#include <velk-render/interface/intf_view_entry.h>

namespace velk {

/**
 * @brief Classic forward shading path.
 *
 * Activated when no path is attached (built-in fallback) or when a
 * camera explicitly attaches a `ForwardPath`. Emits one Raster pass per
 * view with the env batch (prepended in batch rebuild) followed by
 * scene geometry. RTT subtrees are emitted from `build_shared_passes`
 * via the shared `RenderTargetCache` on `FrameContext`.
 */
class ForwardPath : public ext::RenderPath<ForwardPath>
{
public:
    VELK_CLASS_UID(ClassId::Path::Forward, "ForwardPath");

    Needs needs() const override
    {
        Needs n;
        n.batches = true;
        return n;
    }

    void build_passes(IViewEntry& view,
                      const RenderView& render_view,
                      IRenderTarget::Ptr color_target,
                      FrameContext& ctx,
                      IRenderGraph& graph) override;
};

} // namespace velk

#endif // VELK_UI_FORWARD_PATH_H
