#ifndef VELK_UI_RT_PATH_H
#define VELK_UI_RT_PATH_H

#include <velk/string.h>
#include <velk/vector.h>

#include <unordered_map>

#include <velk-render/plugin.h>
#include <velk-render/ext/render_path.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_render_path.h>
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
class RtPath : public ext::RenderPath<RtPath>
{
public:
    VELK_CLASS_UID(ClassId::Path::Rt, "RtPath");

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

private:
    struct ViewState
    {
        ::velk::IRenderTarget::Ptr rt_output;
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
