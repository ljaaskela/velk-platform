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

        /// Demodulated diffuse irradiance (no albedo) written by the lighting
        /// pass: the noisy single-light stochastic estimate the denoise pass
        /// reprojects + accumulates. Recreated on size change.
        IRenderTarget::Ptr diffuse_irr;
        IGpuTexture* diffuse_irr_tex = nullptr;

        /// Ping-pong temporal history for the diffuse-irradiance denoiser.
        /// hist_irr: rgb = accumulated irradiance, a = sample count.
        /// hist_pos: rgb = world position (reprojection validation), a = valid.
        /// The denoise pass reads [frame & 1], writes [1 - (frame & 1)].
        IRenderTarget::Ptr hist_irr[2];
        IGpuTexture* hist_irr_tex[2] = {nullptr, nullptr};
        IRenderTarget::Ptr hist_pos[2];
        IGpuTexture* hist_pos_tex[2] = {nullptr, nullptr};
        uvec2 hist_size{};
        /// Cached temporal-accumulate pass (writes the history). Re-records
        /// every frame (ping-pong ids change); cheap (one dispatch).
        IRenderPass::Ptr cached_denoise_pass;
        bool denoise_dirty = true;
        /// Discards temporal history for one frame (first frame / resize). NOT
        /// set on camera motion (reprojection handles that).
        bool denoise_reset_pending = true;

        /// Cached spatial-filter + composite pass (count-driven a-trous over the
        /// accumulated history, then composite + surface blit). Re-records every
        /// frame (the history ping-pong id alternates).
        IRenderPass::Ptr cached_spatial_pass;
        bool spatial_dirty = true;
        /// Surface-composite blit target the spatial pass bakes; resize detect.
        IGpuTexture* last_spatial_dst = nullptr;


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

        /// Cached transparent (blended) forward pass: draws BLEND-mode batches
        /// over the lit composite, depth-testing the retained gbuffer depth
        /// without writing it. Rebuilt on the same triggers as the gbuffer pass
        /// (batch / material / camera change, resize), plus when the composite
        /// target it draws into is recreated.
        IRenderPass::Ptr cached_transparent_pass;
        bool transparent_dirty = true;
        IGpuTexture* last_transparent_target = nullptr;
    };

private:
    std::unordered_map<IViewEntry*, ViewState> view_states_;

    /// Resolves the deferred-lighting compute pipeline for the active
    /// snippet set, compiling on a (weak) cache miss. Returns a strong Ptr
    /// the caller must keep alive (the lighting pass holds it).
    IGpuPipeline::Ptr ensure_pipeline(FrameContext& ctx);

    /// Resolves the diffuse-irradiance temporal-accumulate pipeline
    /// (standalone compute). Strong Ptr; the temporal pass holds it.
    IGpuPipeline::Ptr ensure_denoise_pipeline(FrameContext& ctx);

    /// Resolves the spatial filter + composite pipeline (standalone compute).
    /// Strong Ptr; the spatial pass holds it.
    IGpuPipeline::Ptr ensure_spatial_pipeline(FrameContext& ctx);

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

    /// Reprojects + temporally accumulates the noisy diffuse irradiance into
    /// the ping-pong history (no composite).
    void emit_temporal_pass(IViewEntry& view, ViewState& vs,
                            const RenderView& render_view,
                            FrameContext& ctx,
                            int w, int h,
                            IRenderGraph& graph);

    /// Count-driven spatial filter over the accumulated irradiance history,
    /// then composites the final HDR image (albedo*irr + sharp rest) into
    /// deferred_output and blits it to @p color_target.
    void emit_spatial_composite_pass(IViewEntry& view, ViewState& vs,
                                     const RenderView& render_view,
                                     IRenderTarget::Ptr color_target,
                                     FrameContext& ctx,
                                     int w, int h,
                                     IRenderGraph& graph);

    /// Forward pass for BLEND-mode batches, drawn over the lit composite in
    /// @p color_target with the gbuffer depth bound read-only. No-op when the
    /// view has no transparent batches.
    void emit_transparent_pass(IViewEntry& view, ViewState& vs,
                               const RenderView& render_view,
                               IRenderTarget::Ptr color_target,
                               FrameContext& ctx,
                               IRenderGraph& graph);
};

} // namespace velk

#endif // VELK_UI_DEFERRED_PATH_H
