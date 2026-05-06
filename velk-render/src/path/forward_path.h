#ifndef VELK_UI_FORWARD_PATH_H
#define VELK_UI_FORWARD_PATH_H

#include <velk/vector.h>

#include <unordered_map>

#include <velk-render/interface/intf_batch.h>
#include <velk-render/frustum.h>
#include <velk-render/plugin.h>
#include <velk-render/ext/render_path.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_render_path.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/interface/intf_render_state.h>
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
class ForwardPath : public ext::RenderPath<ForwardPath, IRenderStateObserver>
{
public:
    VELK_CLASS_UID(ClassId::Path::Forward, "ForwardPath");

    ~ForwardPath() override;

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

    void on_view_removed(IViewEntry& view, FrameContext& ctx) override;

    // IRenderStateObserver — view's batch set changed; invalidate
    // the cached pass for that view so the next emit rebuilds it.
    void on_render_state_changed(IRenderState* source,
                                 RenderStateChange flags) override;

private:
    /// Per-view persistent `IRenderPass::Ptr`. The pass identity is
    /// stable across frames so the graph's compile-time short-circuit
    /// can match. Only rebuilds the pass contents when `dirty` is set
    /// by `on_render_state_changed`; non-dirty frames refresh just the
    /// per-frame `view_globals_address` and re-add the same Ptr.
    /// **Known latent bug:** the dirty flag fires only on batch-set
    /// changes (and once camera detection lands, on camera moves).
    /// Frustum cull result depends on camera matrix; until camera
    /// detection lands, a moving camera with widely-distributed
    /// geometry can keep cached draws that exclude/include the wrong
    /// batches. Doesn't manifest for clustered scenes.
    struct CachedPass
    {
        IRenderPass::Ptr pass;
        bool dirty = true;
    };
    std::unordered_map<IViewEntry*, CachedPass> cached_passes_;
};

} // namespace velk

#endif // VELK_UI_FORWARD_PATH_H
