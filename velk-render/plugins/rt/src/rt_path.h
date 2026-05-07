#ifndef VELK_UI_RT_PATH_H
#define VELK_UI_RT_PATH_H

#include <velk/string.h>
#include <velk/vector.h>

#include <unordered_map>

#include <velk/api/change.h>

#include <velk-render/plugin.h>
#include <velk-render/ext/persistent_buffer.h>
#include <velk-render/ext/render_path.h>
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
        // downstream PushC bindless ids and `add_write` resource refs
        // stay stable.
        ::velk::uvec2 output_size{};

        /// Per-view persistent buffer holding the plane-sorted RtShape
        /// list. Stable GPU address; sort order changes when the
        /// camera moves (depth recomputed), at which point
        /// PersistentBuffer signals `changed` and the upload happens.
        ::velk::PersistentBuffer shapes_buffer;

        /// Cached RT compute+blit pass. Stable Ptr across frames so
        /// the graph compile short-circuits. Rebuilt only when
        /// `rt_dirty` is set by `on_render_state_changed` (camera /
        /// lights / env via view notify), `rt_output` recreation
        /// (resize), shapes_buffer.upload reporting `changed`, or
        /// `rt_change` detecting BVH / shape-count drift.
        ::velk::IRenderPass::Ptr cached_rt_pass;
        bool rt_dirty = true;

        /// PushC fingerprint covering inputs not propagated through
        /// the view notify cascade (BVH addresses + shape_count +
        /// shapes_addr). When BVH or shape topology changes mid-run
        /// these flip even though the view itself hasn't notified.
        struct RtKey
        {
            uint64_t bvh_nodes_addr;
            uint64_t bvh_shapes_addr;
            uint64_t shapes_addr;
            uint32_t bvh_root;
            uint32_t bvh_node_count;
            uint32_t shape_count;
            bool operator==(const RtKey& rhs) const
            {
                return bvh_nodes_addr == rhs.bvh_nodes_addr
                    && bvh_shapes_addr == rhs.bvh_shapes_addr
                    && shapes_addr == rhs.shapes_addr
                    && bvh_root == rhs.bvh_root
                    && bvh_node_count == rhs.bvh_node_count
                    && shape_count == rhs.shape_count;
            }
        };
        ::velk::ChangeCache<RtKey> rt_change;
    };

    std::unordered_map<IViewEntry*, ViewState> view_states_;

    /// Compiled compute pipelines keyed by FNV hash of (materials,
    /// shadow techs, intersects). Composition runs through the shared
    /// FrameSnippetRegistry on ctx.
    std::unordered_map<uint64_t, bool> compiled_pipelines_;

    uint64_t ensure_pipeline(FrameContext& ctx);
};

} // namespace velk

#endif // VELK_UI_RT_PATH_H
