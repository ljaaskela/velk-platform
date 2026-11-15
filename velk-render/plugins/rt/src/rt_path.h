#ifndef VELK_UI_RT_PATH_H
#define VELK_UI_RT_PATH_H

#include <velk/string.h>
#include <velk/vector.h>

#include <unordered_map>

#include <velk/api/change.h>

#include <velk-render/plugin.h>
#include <velk-render/frame/instance_cull.h>
#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/ext/render_path.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_render_path.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/interface/intf_render_state.h>
#include <velk-render/interface/intf_view_entry.h>

namespace velk {

class IShadowTechnique;

/**
 * @brief Compute-shader path tracer render path.
 *
 * Allocates a per-view storage output texture, keeps the view's cut and
 * the trees rays walk over it, dispatches a composed compute shader that
 * traces them, and blits the shaded output to the surface.
 *
 * Owns the compiled compute pipeline cache (keyed by active material
 * + shadow-tech + intersect snippet sets) and per-view RT allocations.
 */
class RtPath : public ext::RenderPath<RtPath, ::velk::IRenderStateObserver>
{
public:
    VELK_CLASS_UID(ClassId::Path::Rt, "RtPath");

    ~RtPath() override;

    Needs needs() const override
    {
        Needs n;
        n.batches = true;
        n.lights = true;
        return n;
    }

    void build_passes(IViewEntry& view,
                      const RenderView& render_view,
                      IRenderTarget::Ptr color_target,
                      FrameContext& ctx,
                      IRenderGraph& graph) override;
    void on_view_removed(IViewEntry& view, FrameContext& ctx) override;
    void shutdown(FrameContext& ctx) override;

    // IRenderStateObserver — view's camera / batches / lights / env
    // changed; invalidate the cached RT pass for that view.
    void on_render_state_changed(::velk::IRenderState* source,
                                 ::velk::RenderStateChange flags) override;

private:
    struct ViewState
    {
        /// The view's kept cut, and the trees over it that rays walk.
        InstanceCullPass instance_cull;
        ::velk::IRenderTarget::Ptr rt_output;
        // Cached size for rt_output. Recreate only on size change so
        // downstream RtRoot bindless ids and `add_write` resource refs
        // stay stable.
        ::velk::uvec2 output_size{};

        /// Cached RT compute+blit pass. Stable Ptr across frames so
        /// the graph compile short-circuits. Rebuilt only when
        /// `rt_dirty` is set by `on_render_state_changed` (camera /
        /// lights / env via view notify) or `rt_output` recreation
        /// (resize). What rays walk is read fresh from FrameGlobals.
        ::velk::IRenderPass::Ptr cached_rt_pass;
        bool rt_dirty = true;
        /// Resize detection: see DeferredPath::ViewState.
        ::velk::IGpuTexture* last_dst_texture = nullptr;
        /// The pipeline the cached pass was recorded with.
        const ::velk::IGpuPipeline* last_pipeline = nullptr;
    };

    std::unordered_map<IViewEntry*, ViewState> view_states_;

    /// Resolves the RT compute pipeline for the active snippet set
    /// (materials, shadow techs, intersects), compiling on a (weak) cache
    /// miss. Returns a strong Ptr the caller keeps alive (the RT pass
    /// holds it). Composition runs through the shared FrameSnippetRegistry.
    IGpuPipeline::Ptr ensure_pipeline(FrameContext& ctx);
};

} // namespace velk

#endif // VELK_UI_RT_PATH_H
