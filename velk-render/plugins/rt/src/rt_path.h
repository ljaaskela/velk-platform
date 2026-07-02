#ifndef VELK_UI_RT_PATH_H
#define VELK_UI_RT_PATH_H

#include <velk/string.h>
#include <velk/vector.h>

#include <unordered_map>

#include <velk/api/change.h>

#include <velk-render/plugin.h>
#include <velk-render/ext/persistent_buffer.h>
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
 * Allocates a per-view storage output texture, builds a flat painter-
 * sorted shape buffer, dispatches a composed compute shader against
 * the scene-wide BVH, and blits the shaded output to the surface.
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
        n.shapes = true;
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
        ::velk::IRenderTarget::Ptr rt_output;
        // Cached size for rt_output. Recreate only on size change so
        // downstream RtRoot bindless ids and `add_write` resource refs
        // stay stable.
        ::velk::uvec2 output_size{};

        /// Per-view region in the shared primary-shapes arena (set = 1 slot
        /// 6) holding the plane-sorted RtShape list. Stable base across
        /// frames (persistent); re-allocated only when the shape count
        /// changes. Sort order / positions change when the camera moves;
        /// those bytes are rewritten in place and read fresh via shapes_base.
        ::velk::ArenaRegion shapes_region;

        /// Cached RT compute+blit pass. Stable Ptr across frames so
        /// the graph compile short-circuits. Rebuilt only when
        /// `rt_dirty` is set by `on_render_state_changed` (camera /
        /// lights / env via view notify), `rt_output` recreation
        /// (resize), a shape-count change (shapes-region realloc), or
        /// `rt_change` detecting BVH / shape-count drift.
        ::velk::IRenderPass::Ptr cached_rt_pass;
        bool rt_dirty = true;
        /// Resize detection: see DeferredPath::ViewState.
        ::velk::IGpuTexture* last_dst_texture = nullptr;

        /// PushC fingerprint covering inputs not propagated through the
        /// view notify cascade (BVH topology + shape count). When the BVH or
        /// shape set changes mid-run these flip even though the view itself
        /// hasn't notified. Bases (bvh node/shape, primary shapes) are
        /// deliberately excluded: they are read fresh from RtRoot /
        /// FrameGlobals each dispatch, so they must not force a re-record.
        struct RtKey
        {
            uint32_t bvh_root;
            uint32_t bvh_node_count;
            uint32_t shape_count;
            bool operator==(const RtKey& rhs) const
            {
                return bvh_root == rhs.bvh_root
                    && bvh_node_count == rhs.bvh_node_count
                    && shape_count == rhs.shape_count;
            }
        };
        ::velk::ChangeCache<RtKey> rt_change;
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
