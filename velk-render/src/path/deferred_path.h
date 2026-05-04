#ifndef VELK_UI_DEFERRED_PATH_H
#define VELK_UI_DEFERRED_PATH_H

#include <velk/vector.h>

#include <unordered_map>

#include <velk-render/interface/intf_batch.h>
#include <velk-render/frustum.h>
#include <velk-render/plugin.h>
#include <velk-render/ext/render_path.h>
#include <velk-render/interface/intf_render_path.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_render_texture_group.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/render_path/view_entry.h>

namespace velk {

/**
 * @brief Deferred shading path: G-buffer fill + compute lighting + blit.
 *
 * Two-stage pipeline collapsed into a single IRenderPath since one
 * camera attaches one path. Per view we:
 *
 *   1. Rebuild raster batches if the scene's visual set changed.
 *   2. Allocate / resize the per-view G-buffer (multi-attachment
 *      render target group) and emit a `PassKind::GBufferFill` pass
 *      that fills albedo / normal / world-pos / material-params.
 *   3. Allocate / resize the deferred output storage texture and the
 *      RT-shadow debug texture.
 *   4. Compose a compute pipeline for the active intersect-snippet
 *      set, look up G-buffer attachments, gather lights + env, and
 *      emit a `PassKind::ComputeBlit` pass that shades into the
 *      output texture and blits to the view surface.
 *
 * The two stages share per-view state (batches, gbuffer_group,
 * deferred_output_tex, shadow_debug_tex) via the
 * `ViewState` struct.
 */
class DeferredPath : public ext::RenderPath<DeferredPath>
{
public:
    VELK_CLASS_UID(ClassId::Path::Deferred, "DeferredPath");

    Needs needs() const override
    {
        Needs n;
        n.batches = true;
        n.lights = true;
        return n;
    }

    void build_passes(ViewEntry& view,
                      const RenderView& render_view,
                      IRenderTarget::Ptr color_target,
                      FrameContext& ctx,
                      IRenderGraph& graph) override;

    void on_view_removed(ViewEntry& view, FrameContext& ctx) override;
    void shutdown(FrameContext& ctx) override;

    /// Exposes per-view "gbuffer" (the IRenderTextureGroup),
    /// "gbuffer.worldpos" (a RenderTexture aliasing the worldpos
    /// attachment), "shadow.debug", and "output" outputs for debug
    /// overlays / readback. See `IRenderPath::find_named_output`.
    IGpuResource::Ptr find_named_output(string_view name,
                                        ViewEntry* view) const override;

public:
    struct ViewState
    {
        IRenderTextureGroup::Ptr gbuffer;
        // Recorded after gbuffer creation; consumed by emit_lighting_pass
        // and emit_gbuffer_pass for viewport sizing.
        int gbuffer_width = 0;
        int gbuffer_height = 0;

        IRenderTarget::Ptr deferred_output;
        IRenderTarget::Ptr shadow_debug;

        /// RenderTexture alias for the gbuffer worldpos attachment.
        /// Does not own the GPU texture (the group does); exposed via
        /// find_named_output("gbuffer.worldpos") for debug readback.
        IRenderTarget::Ptr worldpos_alias;
    };

private:
    std::unordered_map<ViewEntry*, ViewState> view_states_;

    /// Compiled compute pipelines keyed by FNV hash of active intersect
    /// id set; each variant compiles once, kept across frames.
    std::unordered_map<uint64_t, bool> compiled_pipelines_;

    uint64_t ensure_pipeline(FrameContext& ctx);

    RenderTargetGroup ensure_gbuffer(ViewState& vs, int width, int height,
                                     FrameContext& ctx, IRenderGraph& graph);

    void emit_gbuffer_pass(ViewEntry& view, ViewState& vs,
                           const RenderView& render_view, FrameContext& ctx,
                           IRenderGraph& graph);

    void emit_lighting_pass(ViewEntry& view, ViewState& vs,
                            const RenderView& render_view,
                            IRenderTarget::Ptr color_target,
                            FrameContext& ctx,
                            int w, int h,
                            IRenderGraph& graph);
};

} // namespace velk

#endif // VELK_UI_DEFERRED_PATH_H
