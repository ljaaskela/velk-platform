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

    void reserve(IViewEntry& view, const RenderView& render_view, FrameContext& ctx) override;

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
    /// attachment), and "output" outputs for debug
    /// overlays / readback. See `IRenderPath::find_named_output`.
    IGpuResource::Ptr find_named_output(string_view name,
                                        IViewEntry* view) const override;

public:
    struct ViewState
    {
        IRenderTextureGroup::Ptr gbuffer;
        // Recorded after gbuffer creation; consumed by the passes for sizing.
        uvec2 gbuffer_size{};

        /// The lit image, HDR. Recreated on size change.
        IRenderTarget::Ptr deferred_output;
        IGpuTexture* deferred_output_tex = nullptr;
        uvec2 deferred_output_size{};
        /// The light on the surface, for far instances in front of it: indirect light in
        /// rgb, and in alpha the share of direct light that gets through. Same size.
        uvec2 output_size{};
        /// What the lighting pass blits to; a new one re-records it.
        IGpuTexture* last_output_dst = nullptr;

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
        /// What the view's culling left for draws, which the visibility pass was recorded with.
        CutDrawWords cut_draw_words;
        IRenderPass::Ptr light_claim_pass;
        IRenderPass::Ptr light_search_pass;
        /// The claim again after the search, for the records it laid out (light_claim_src).
        IRenderPass::Ptr light_claim_again_pass;
        IRenderPass::Ptr light_trace_pass;
        /// What changed this frame for the light (kLightChangeWords a change), and the lights as
        /// each was when last listed as changed. See light_changes_main_src.
        ArenaRegion light_changes;
        vector<GpuLight> light_changes_lights;
        IRenderPass::Ptr light_changes_pass;
        /// For the log: frames with changes since it last printed, their changes, the times
        /// everything changed, and the frame it last printed.
        uint32_t light_change_frames = 0;
        uint32_t light_change_count = 0;
        uint32_t light_change_all = 0;
        uint64_t light_change_log_frame = 0;
        uint32_t light_claim_recorded[7] = {};  ///< What the light claim pass was recorded with.        /// The screen probes' two stores (kLightProbeWords a probe), which swap each frame, and
        /// how many probes each holds. See light_probes_main_src.
        ArenaRegion light_probes[2];
        uint32_t light_probe_count = 0;
        uint32_t light_probe_parity = 0;
        IRenderPass::Ptr light_probes_pass;
        IRenderPass::Ptr visibility_debug_pass;
        IRenderPass::Ptr chooser_frustum_pass;
        IRenderPass::Ptr partition_bounds_pass;
        /// The light header in the culling arena, whose word 18 is where the light grid is.
        /// See CELLS_HEADER.
        ArenaRegion light_header;
        /// The lights by where they reach, rebuilt each frame. See VELK_LIGHT_GRID_GLSL.
        ArenaRegion light_grid;
        IRenderPass::Ptr visibility_resolve_pass;
        /// Clusters whose triangles are too small on screen for the hardware, drawn in
        /// compute: their list, with room for one a pixel (kTinyHeaderWords), and a word a
        /// pixel for the depth compute drew, followed by every pixel's ids as the resolve
        /// leaves them, two words a pixel (velk_pixel_ids). All in the culling arena.
        ArenaRegion tiny_list;
        uint32_t tiny_capacity = 0;
        ArenaRegion pixel_words;
        uvec2 pixel_words_size{0, 0};
        IRenderPass::Ptr tiny_pass;
        uint64_t tiny_log_frame = 0;  ///< When the count drawn in compute was last printed.
        /// The depth of what is drawn at decreasing sizes, in the culling arena.
        ArenaRegion depth_pyramid;
        uvec2 depth_pyramid_size{0, 0};
        uint32_t depth_pyramid_levels = 0;
        IRenderPass::Ptr depth_pyramid_pass;
        IRenderPass::Ptr depth_pyramid_compute_pass;  ///< Built again with compute raster's pixels.
        InstanceCullPass instance_cull;

        /// Cached lighting (compute + blit) pass. Rebuilt only when `lighting_dirty` is set
        /// by `on_render_state_changed` (camera / batch / lights change), by gbuffer /
        /// output_size recreation, or by a new blit target.
        IRenderPass::Ptr cached_lighting_pass;
        bool lighting_dirty = true;
        /// Which lighting-shader variant this view last used. The analytic
        /// area-light path is compiled in only when the view has an area
        /// light, so a change here swaps the pipeline.
        bool had_area_light = false;

        /// Cached transparent (blended) forward pass: draws BLEND-mode batches
        /// over the lit composite, depth-testing the retained gbuffer depth
        /// without writing it. Rebuilt on the same triggers as the gbuffer pass
        /// (batch / material / camera change, resize), plus when the composite
        /// target it draws into is recreated.
        IRenderPass::Ptr cached_transparent_pass;
        bool transparent_dirty = true;

        /// Render test measurements, while VELK_TEST_DIAGNOSTICS is set.
        ArenaRegion test_words;
        IRenderTarget::Ptr test_view;
        uvec2 test_view_size{};
        IRenderTarget::Ptr test_history[2];
        uvec2 test_history_size{};
        IRenderPass::Ptr test_diagnostics_pass;
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
    IGpuPipeline::Ptr ensure_pipeline(FrameContext& ctx, bool with_area_lights);

    /// Allocates the screen probes' words, the pixel words, compute raster's list and the render
    /// tests' in the culling arena, for a frame of @p size. From reserve.
    void ensure_arena_words(ViewState& vs, FrameContext& ctx, uvec2 size);
    /// Builds the grid of lights by where they reach and writes it to the culling arena,
    /// growing its region as needed. From reserve. See VELK_LIGHT_GRID_GLSL.
    void build_light_grid(ViewState& vs, const RenderView& render_view, FrameContext& ctx);
    /// The depth pyramid's region in the culling arena, for a frame of @p size. From reserve.
    void ensure_depth_pyramid(ViewState& vs, FrameContext& ctx, uvec2 size);

    IRenderTextureGroup* ensure_gbuffer(ViewState& vs, int width, int height,
                                        FrameContext& ctx, IRenderGraph& graph);

    /// Makes deferred_output the size of the G-buffer, which the lighting pass and the debug
    /// views write.
    void ensure_deferred_output(ViewState& vs, int w, int h, IRenderGraph& graph);
    /// Lights the G-buffer into deferred_output, shadows from the points of light, and blits
    /// it to @p color_target.
    void emit_lighting_pass(IViewEntry& view, ViewState& vs,
                            const RenderView& render_view,
                            IRenderTarget::Ptr color_target,
                            FrameContext& ctx,
                            int w, int h,
                            IRenderGraph& graph);


    /// Draws what GPU culling kept. @p second_phase draws what its second test added, over the first.
    void emit_visibility_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx, IRenderGraph& graph,
                              bool second_phase);
    void emit_visibility_resolve_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx, IRenderGraph& graph);
    void emit_tiny_cluster_pass(ViewState& vs, FrameContext& ctx, IRenderGraph& graph, float tiny_pixels);
    /// Where the pixels' ids start in the culling arena: see ViewState::pixel_words.
    static uint32_t pixel_ids_word(const ViewState& vs);
    /// Builds the depth pyramid from the hardware's depth, or with @p with_compute from that and
    /// what compute raster drew, before the resolve clears it.
    void emit_depth_pyramid_pass(ViewState& vs, FrameContext& ctx, IRenderGraph& graph, bool with_compute);
    /// Gives each kept cut record a pixel shows its light in the view's light pool, the first
    /// time one does (light_claim_src).
    void emit_light_claim_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx, IRenderGraph& graph);
    /// Lists what changed this frame and makes the light it could alter due again
    /// (light_changes_main_src), before the claim. Nothing when nothing changed.
    void emit_light_changes_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx, IRenderGraph& graph);
    /// Gathers the screen probes' bounce light (light_probes_main_src), before the lighting.
    void emit_light_probes_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx, IRenderGraph& graph);
    /// Replaces the final image with the visibility buffer as flat colours.
    void emit_visibility_debug_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                    FrameContext& ctx, uint32_t mode, IRenderGraph& graph);
    /// Draws the bounds of partitioned visuals' partitions over the final image, coloured by
    /// whether they chose again this frame (DebugView::Partition), or with @p tree_level set
    /// the boxes of that level of the trees rays walk over them (DebugView::RayTree), or with
    /// @p top too of the tree over the partitions (DebugView::RayTop).
    void emit_partition_bounds_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                    FrameContext& ctx, IRenderGraph& graph, uint32_t tree_level = kCullNoSlot,
                                    bool top = false);
    /// Draws the edges of the chooser's view over the final image (ICamera::choose_as).
    void emit_chooser_frustum_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                   FrameContext& ctx, IRenderGraph& graph);
    /// Logs how a frame differs from the last, for render tests. See test_diagnostics_compute_src.
    void emit_test_diagnostics_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                    FrameContext& ctx, IRenderGraph& graph);

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
