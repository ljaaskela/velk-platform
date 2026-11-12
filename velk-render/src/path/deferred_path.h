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
        /// The light on the surface, for far instances in front of it: indirect light in
        /// rgb, and in alpha the share of direct light that gets through. Same size.
        IRenderTarget::Ptr surface_light;
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
        IRenderPass::Ptr visibility_debug_pass;
        IRenderPass::Ptr light_cells_debug_pass;
        IRenderPass::Ptr light_cells_pass;
        IRenderPass::Ptr light_cells_update_pass;
        /// The table of light cells, in the culling arena. See light_cells_main_src.
        ArenaRegion light_cells;
        /// What changed lately, for the light cells to check their light against: boxes with
        /// the frame each changed in, oldest first, and their copy in the culling arena.
        struct LightCellChange
        {
            WorldBox box;
            uint32_t frame;
            uint32_t light;  ///< Which light moved, or 0xFFFFFFFF for a box something changed in.
        };
        vector<LightCellChange> light_cell_changes;
        ArenaRegion light_cell_change_words;
        /// The words that say where the table and the list are. See CELLS_HEADER.
        ArenaRegion light_cell_header;
        /// The cells to trace this frame. See CELLS_WORK.
        ArenaRegion light_cell_work;
        /// The bounce cells' table, and the list of those to trace this frame. See
        /// VELK_BOUNCE_GLSL and CELLS_BOUNCE_LIST.
        ArenaRegion light_bounce;
        ArenaRegion light_bounce_list;
        /// The pixels' reflection rays. See kReflectWords.
        ArenaRegion light_reflect;
        /// The lights as each was when last listed as moved, to tell which moved enough since.
        vector<GpuLight> light_cell_lights;
        /// The cells' counters when last printed, and the frame. See kStatAsked.
        uint32_t light_cell_stats[36] = {};
        /// What joined the cells' change list since the counters were last printed: moved
        /// boxes listed, moved boxes too small to list, and moved lights.
        uint32_t light_cell_boxes_listed = 0;
        uint32_t light_cell_boxes_small = 0;
        uint32_t light_cell_lights_moved = 0;
        uint64_t light_cell_stats_frame = 0;
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
        InstanceCullPass instance_cull;
        /// Instances too small to draw. A region of the frame's size per visual.
        IRenderPass::Ptr far_instance_pass;
        struct FarInstanceWords
        {
            const void* key = nullptr;
            ArenaRegion words;
        };
        vector<FarInstanceWords> far_instance_words;

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
    IGpuPipeline::Ptr ensure_pipeline(FrameContext& ctx, bool with_area_lights, string_view ray_motion,
                                      float ray_motion_reach);

    /// Allocates the light cells' words and the render tests' in the culling arena. Call
    /// before recording anything that names the culling arena.
    void ensure_arena_words(ViewState& vs, FrameContext& ctx);

    IRenderTextureGroup* ensure_gbuffer(ViewState& vs, int width, int height,
                                        FrameContext& ctx, IRenderGraph& graph);

    /// Lights the G-buffer into deferred_output, shadows from the light cells, and blits
    /// it to @p color_target.
    void emit_lighting_pass(IViewEntry& view, ViewState& vs,
                            const RenderView& render_view,
                            IRenderTarget::Ptr color_target,
                            FrameContext& ctx,
                            int w, int h,
                            IRenderGraph& graph);

    /// Adds instances too small to draw over deferred_output and blits it again.
    void emit_far_instance_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                FrameContext& ctx, int w, int h, IRenderGraph& graph);

    /// Draws what GPU culling kept. @p second_phase draws what its second test added, over the first.
    void emit_visibility_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx, IRenderGraph& graph,
                              bool second_phase);
    void emit_visibility_resolve_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx, IRenderGraph& graph);
    void emit_tiny_cluster_pass(ViewState& vs, FrameContext& ctx, IRenderGraph& graph, float tiny_pixels);
    /// Where the pixels' ids start in the culling arena: see ViewState::pixel_words.
    static uint32_t pixel_ids_word(const ViewState& vs);
    void emit_depth_pyramid_pass(ViewState& vs, FrameContext& ctx, IRenderGraph& graph);
    /// Replaces the final image with the visibility buffer as flat colours.
    void emit_visibility_debug_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                    FrameContext& ctx, uint32_t mode, IRenderGraph& graph);
    /// Logs how a frame differs from the last, for render tests. See test_diagnostics_compute_src.
    void emit_test_diagnostics_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                    FrameContext& ctx, IRenderGraph& graph);
    /// Replaces the final image with the light cells under each pixel.
    void emit_light_cells_debug_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                     FrameContext& ctx, uint32_t mode, IRenderGraph& graph);
    /// Gives the light cells this frame's changes and tells their shaders where they are.
    /// False if they are not set up.
    bool prepare_light_cells(ViewState& vs, const RenderView& render_view, FrameContext& ctx);
    /// Makes, checks and traces the light cells in view, before anything reads them.
    void emit_light_cells_update_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx,
                                      IRenderGraph& graph);
    /// Replaces the final image with direct light read from the light cells (mode 4) or with
    /// their states (mode 5).
    void emit_light_cells_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
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
