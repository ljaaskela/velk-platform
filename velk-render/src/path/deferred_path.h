#ifndef VELK_UI_DEFERRED_PATH_H
#define VELK_UI_DEFERRED_PATH_H

#include <velk/vector.h>

#include <unordered_map>

#include "path/deferred_gbuffer.h"
#include <velk-render/frame/instance_cull.h>
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
 * @brief Deferred shading path: visibility buffer + material resolve +
 *        compute lighting + blit.
 *
 * Per view:
 *
 *   1. A raster pass writes which triangle each pixel shows, and depth.
 *   2. A compute pass evaluates each pixel's material from that, filling
 *      the G-buffer's albedo / normal / position / material / emissive.
 *   3. A compute pass shades the G-buffer into the output texture, which
 *      is blitted to the view surface.
 *
 * On a device that cannot read gl_PrimitiveID, the view is drawn by a
 * forward path instead.
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
        // Recorded after gbuffer creation; consumed by the passes for sizing.
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

        /// Unshadowed diffuse (all lights, no visibility), written sharp
        /// alongside the ratio in `diffuse_irr`. The composite multiplies the
        /// two: filtering the ratio keeps the shadow soft while light falloff
        /// and N.L stay crisp. Recreated on size change with `diffuse_irr`.
        IRenderTarget::Ptr unshadowed;
        /// What each traced pixel picked and whether it is lit, for every pixel to share.
        IRenderTarget::Ptr direct_samples;
        uvec2 direct_samples_size{};
        bool direct_share_on = false;
        IGpuTexture* unshadowed_tex = nullptr;

        /// Ping-pong temporal history for the diffuse-irradiance denoiser.
        /// hist_irr: rgb = accumulated irradiance, a = sample count.
        /// hist_pos: rgb = world position (reprojection validation), a = valid.
        /// The denoise pass reads [frame & 1], writes [1 - (frame & 1)].
        IRenderTarget::Ptr hist_irr[2];
        IGpuTexture* hist_irr_tex[2] = {nullptr, nullptr};
        IRenderTarget::Ptr hist_pos[2];
        IGpuTexture* hist_pos_tex[2] = {nullptr, nullptr};
        /// Accumulated luminance 2nd moment (.r), ping-pong alongside hist_irr.
        /// variance = moment - mean^2 drives the spatial filter's luminance edge
        /// stop (SVGF-style).
        IRenderTarget::Ptr hist_mom[2];
        IGpuTexture* hist_mom_tex[2] = {nullptr, nullptr};
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

        /// Which triangle each pixel shows, and the depth of what is drawn.
        IRenderTextureGroup::Ptr visibility;
        uvec2 visibility_size{0, 0};
        IRenderPass::Ptr cached_visibility_pass;
        bool visibility_dirty = true;
        IRenderPass::Ptr cached_visibility_second_pass;
        bool visibility_second_dirty = true;
        IRenderPass::Ptr visibility_debug_pass;
        IRenderPass::Ptr gi_debug_pass;
        IRenderPass::Ptr visibility_resolve_pass;
        /// The depth of what is drawn at decreasing sizes, in the culling arena.
        ArenaRegion depth_pyramid;
        uvec2 depth_pyramid_size{0, 0};
        uint32_t depth_pyramid_levels = 0;
        IRenderPass::Ptr depth_pyramid_pass;
        InstanceCullPass instance_cull;
        /// Instances too small to draw. A region of the frame's size per visual.
        IRenderPass::Ptr far_instance_pass;
        struct FarInstanceWords
        {
            const void* key = nullptr;
            ArenaRegion words;
        };
        vector<FarInstanceWords> far_instance_words;

        /// Cached lighting (compute + blit) pass. All PushC inputs are
        /// stable across frames now: gbuffer attachments (Step B),
        /// deferred_output / shadow_debug (Step C), lights_base +
        /// light_count (persistent light arena; re-record only on light
        /// count change), cam_pos notify via Step A. Rebuilt only when
        /// `lighting_dirty` is
        /// set by `on_render_state_changed` (camera / batch / lights
        /// change) or by gbuffer / output_size recreation.
        IRenderPass::Ptr cached_lighting_pass;
        bool lighting_dirty = true;
        /// Which lighting-shader variant this view last used. The analytic
        /// area-light path is compiled in only when the view has an area
        /// light, so a change here swaps the pipeline.
        bool had_area_light = false;
        bool had_medium = false;

        /// Cached transparent (blended) forward pass: draws BLEND-mode batches
        /// over the lit composite, depth-testing the retained gbuffer depth
        /// without writing it. Rebuilt on the same triggers as the gbuffer pass
        /// (batch / material / camera change, resize), plus when the composite
        /// target it draws into is recreated.
        IRenderPass::Ptr cached_transparent_pass;
        bool transparent_dirty = true;

        /// The GI probe field, all its levels in one atlas. See VELK_GI_FIELD_GLSL.
        /// Read back by the probe pass for its temporal blend, so it persists
        /// across frames rather than being transient.
        IRenderTarget::Ptr gi_probes;
        /// Each probe's map of irradiance against each direction. See VELK_GI_FIELD_GLSL.
        IRenderTarget::Ptr gi_probe_irradiance;
        /// The field being rebuilt from the top. See emit_probe_pass.
        uint32_t gi_sweep_left = 0;
        uint64_t gi_sweep_frame = 0;
        uint64_t gi_lighting = 0;
        bool gi_swept = false;
        bool gi_field_ready = false;
        IRenderPass::Ptr cached_probe_pass;
        /// What the probe rays found, for the pass that updates the probes' maps.
        ArenaRegion gi_probe_rays;
        IRenderPass::Ptr cached_probe_trace_pass;
        IRenderPass::Ptr cached_probe_maps_pass;
        /// Exponent of the finest probe cell's width. Chosen once distances arrive.
        int32_t gi_finest = 0;
        bool gi_finest_known = false;
        /// Distances to what the camera sees, sampled from the G-buffer each frame.
        ArenaRegion gi_distances;
        IRenderPass::Ptr cached_measure_pass;
        /// Render test measurements, while VELK_TEST_DIAGNOSTICS is set.
        ArenaRegion test_words;
        IRenderTarget::Ptr test_view;
        uvec2 test_view_size{};
        IRenderTarget::Ptr test_history[2];
        uvec2 test_history_size{};
        IRenderPass::Ptr test_diagnostics_pass;
        ArenaRegion gi_print;
        IRenderPass::Ptr gi_print_pass;
        ArenaRegion gi_pixels;
        IRenderPass::Ptr gi_pixels_pass;
        ArenaRegion gi_reference;
        IRenderPass::Ptr gi_reference_pass;
        ArenaRegion gi_truth;
        IRenderPass::Ptr gi_truth_pass;
        IGpuTexture* last_transparent_target = nullptr;
    };

private:
    std::unordered_map<IViewEntry*, ViewState> view_states_;
    /// Draws the views on a device that cannot read gl_PrimitiveID.
    IRenderPath::Ptr forward_fallback_;

    /// LTC lookup for analytic area-light specular (Heitz et al. 2016). The
    /// fit depends only on roughness and view angle, never on the scene, so
    /// one pair of tables is created on first use and shared by every view.
    /// Generated offline by `design-notes/spike_ltc/ltc_fit.cpp` into
    /// `ltc_table.h`; nothing is fitted at runtime.
    ///   ltc_matrix: the transform, as (m00, m02, m11, m20) with m22 == 1.
    ///   ltc_magnitude: (lobe energy, Fresnel weight) for the split-sum term.
    IRenderTarget::Ptr ltc_matrix_;
    IRenderTarget::Ptr ltc_magnitude_;
    bool ltc_upload_failed_ = false;

    /// Creates and uploads the LTC tables once. Returns false if they are
    /// unavailable, in which case area lights fall back to diffuse only.
    bool ensure_ltc_tables(FrameContext& ctx, IRenderGraph& graph);

    /// Resolves the deferred-lighting compute pipeline for the active
    /// snippet set, compiling on a (weak) cache miss. Returns a strong Ptr
    /// the caller must keep alive (the lighting pass holds it).
    IGpuPipeline::Ptr ensure_pipeline(FrameContext& ctx, bool with_area_lights, bool with_medium, string_view ray_motion,
                                      float ray_motion_reach);

    /// Resolves the diffuse-irradiance temporal-accumulate pipeline
    /// (standalone compute). Strong Ptr; the temporal pass holds it.
    IGpuPipeline::Ptr ensure_denoise_pipeline(FrameContext& ctx);

    /// World-space GI probe passes. With @p shade, the pass composed with the RT body so
    /// probe hits get real material evaluation; otherwise the pass that only traces.
    IGpuPipeline::Ptr ensure_probe_pipeline(FrameContext& ctx, bool shade, bool with_medium, string_view ray_motion,
                                            float ray_motion_reach);

    /// Samples distances to what the camera sees, for choosing the probe field's finest level.
    void emit_gi_measure_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx, IRenderGraph& graph);

    /// Chooses the probe field's finest level from the camera's setting, or else from the
    /// distances sampled on earlier frames. Returns whether one is chosen yet.
    bool update_gi_finest(ViewState& vs, FrameContext& ctx);

    /// Allocates the words the probe passes share. Call before recording anything that names
    /// the culling arena.
    void ensure_probe_rays(ViewState& vs, FrameContext& ctx);

    void emit_probe_pass(ViewState& vs, const RenderView& render_view,
                         FrameContext& ctx, IRenderGraph& graph);

    /// Resolves the spatial filter + composite pipeline (standalone compute).
    /// Strong Ptr; the spatial pass holds it.
    IGpuPipeline::Ptr ensure_spatial_pipeline(FrameContext& ctx);

    IRenderTextureGroup* ensure_gbuffer(ViewState& vs, int width, int height,
                                        FrameContext& ctx, IRenderGraph& graph);

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
    void emit_far_instance_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                FrameContext& ctx, int w, int h, IRenderGraph& graph);
    void emit_spatial_composite_pass(IViewEntry& view, ViewState& vs,
                                     const RenderView& render_view,
                                     IRenderTarget::Ptr color_target,
                                     FrameContext& ctx,
                                     int w, int h,
                                     IRenderGraph& graph);

    /// Draws what GPU culling kept. @p second_phase draws what its second test added, over the first.
    void emit_visibility_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx, IRenderGraph& graph,
                              bool second_phase);
    void emit_visibility_resolve_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx, IRenderGraph& graph);
    void emit_depth_pyramid_pass(ViewState& vs, FrameContext& ctx, IRenderGraph& graph);
    /// Diagnostic: with VELK_GI_PRINT="x,y,z,nx,ny,nz", logs the GI field at that point.
    void emit_gi_print_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx, IRenderGraph& graph);
    /// Diagnostic: with VELK_GI_PIXELS="x,y;x,y", logs what lights those pixels.
    void emit_gi_pixels_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx, IRenderGraph& graph);
    /// Diagnostic: with VELK_GI_PIXELS and VELK_GI_REFERENCE=1, logs the true irradiance there.
    void emit_gi_reference_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx, IRenderGraph& graph);
    /// Diagnostic: with VELK_GI_PIXELS and VELK_GI_TRUTH=1, logs each level's probe against
    /// true paths from its own position.
    void emit_gi_truth_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx, IRenderGraph& graph);
    /// Replaces the final image with the visibility buffer as flat colours.
    void emit_visibility_debug_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                    FrameContext& ctx, uint32_t mode, IRenderGraph& graph);
    /// Logs how a frame differs from the last, for render tests. See test_diagnostics_compute_src.
    void emit_test_diagnostics_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                    FrameContext& ctx, IRenderGraph& graph);
    /// Replaces the final image with a view of the GI probe field.
    void emit_gi_debug_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                            FrameContext& ctx, uint32_t mode, IRenderGraph& graph);

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
