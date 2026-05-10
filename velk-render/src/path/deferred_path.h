#ifndef VELK_UI_DEFERRED_PATH_H
#define VELK_UI_DEFERRED_PATH_H

#include <velk/vector.h>

#include <unordered_map>

#include <velk-render/interface/intf_batch.h>
#include <velk-render/frustum.h>
#include <velk-render/plugin.h>
#include <velk-render/ext/render_path.h>
#include <velk-render/interface/intf_render_path.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/interface/intf_render_state.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_render_texture_group.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_view_entry.h>

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
class DeferredPath : public ext::RenderPath<DeferredPath, IRenderStateObserver>
{
public:
    VELK_CLASS_UID(ClassId::Path::Deferred, "DeferredPath");

    ~DeferredPath() override;

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

    // IRenderStateObserver — view's batch set or camera changed;
    // invalidate the cached gbuffer pass for that view.
    void on_render_state_changed(IRenderState* source,
                                 RenderStateChange flags) override;

    /// Exposes per-view "gbuffer" (the IRenderTextureGroup),
    /// "gbuffer.worldpos" (a RenderTexture aliasing the worldpos
    /// attachment), "shadow.debug", and "output" outputs for debug
    /// overlays / readback. See `IRenderPath::find_named_output`.
    IGpuResource::Ptr find_named_output(string_view name,
                                        IViewEntry* view) const override;

public:
    struct ViewState
    {
        IRenderTextureGroup::Ptr gbuffer;
        // Recorded after gbuffer creation; consumed by emit_lighting_pass
        // and emit_gbuffer_pass for viewport sizing.
        uvec2 gbuffer_size{};

        IRenderTarget::Ptr deferred_output;
        IGpuTexture* deferred_output_tex = nullptr;
        IRenderTarget::Ptr shadow_debug;
        IGpuTexture* shadow_debug_tex = nullptr;
        // Cached size for deferred_output + shadow_debug (both follow
        // the lighting target dims). Recreate only on size change.
        uvec2 output_size{};

        /// RenderTexture alias for the gbuffer worldpos attachment.
        /// Does not own the GPU texture (the group does); exposed via
        /// find_named_output("gbuffer.worldpos") for debug readback.
        IRenderTarget::Ptr worldpos_alias;

        /// Cached gbuffer pass. Stable Ptr across frames so the graph's
        /// compile-time short-circuit can match. Rebuilt only when
        /// `gbuffer_dirty` is set by `on_render_state_changed` or by
        /// `ensure_gbuffer` recreating the gbuffer group on resize.
        IRenderPass::Ptr cached_gbuffer_pass;
        bool gbuffer_dirty = true;

        /// Cached lighting (compute + blit) pass. All PushC inputs are
        /// stable across frames now: gbuffer attachments (Step B),
        /// deferred_output / shadow_debug (Step C), lights_addr +
        /// light_count via write_diff notification (Step D), cam_pos
        /// notify via Step A. Rebuilt only when `lighting_dirty` is
        /// set by `on_render_state_changed` (camera / batch / lights
        /// change) or by gbuffer / output_size recreation.
        IRenderPass::Ptr cached_lighting_pass;
        bool lighting_dirty = true;

        /// Cached surface-blit pass, set only when color_target is a
        /// swapchain surface. The cached_lighting_pass cmd buffer
        /// dispatches into deferred_output; this pass routes
        /// IRenderBackend::blit_to_surface for the per-frame swapchain
        /// copy. Rebuilt under the same lighting_dirty signal.
        IRenderPass::Ptr cached_surface_blit_pass;
    };

private:
    std::unordered_map<IViewEntry*, ViewState> view_states_;

    /// Compiled compute pipelines keyed by FNV hash of active intersect
    /// id set; each variant compiles once, kept across frames.
    std::unordered_map<uint64_t, bool> compiled_pipelines_;

    uint64_t ensure_pipeline(FrameContext& ctx);

    IRenderTextureGroup* ensure_gbuffer(ViewState& vs, int width, int height,
                                        FrameContext& ctx, IRenderGraph& graph);

    void emit_gbuffer_pass(IViewEntry& view, ViewState& vs,
                           const RenderView& render_view, FrameContext& ctx,
                           IRenderGraph& graph);

    void emit_lighting_pass(IViewEntry& view, ViewState& vs,
                            const RenderView& render_view,
                            IRenderTarget::Ptr color_target,
                            FrameContext& ctx,
                            int w, int h,
                            IRenderGraph& graph);
};

} // namespace velk

#endif // VELK_UI_DEFERRED_PATH_H
