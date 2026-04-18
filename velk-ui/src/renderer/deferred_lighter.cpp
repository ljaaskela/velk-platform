#include "deferred_lighter.h"

#include "default_ui_shaders.h"

#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-render/gbuffer.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-render/interface/intf_light.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace velk::ui {

namespace {

// Single canonical key for the deferred compute pipeline. Render
// passes for every view's G-buffer are format-compatible, so one
// compiled pipeline works across views; compile once, reuse forever.
constexpr uint64_t kDeferredPipelineKey = 0xD0FE'2ED1'191A'ABCDULL;
constexpr uint64_t kDeferredCompositePipelineKey = 0xD0FE'2ED1'C0A1'1105ULL;

} // namespace

void DeferredLighter::build_passes(ViewEntry& entry,
                                   const SceneState& scene_state,
                                   FrameContext& ctx,
                                   vector<RenderPass>& out_passes)
{
    if (!ctx.backend || !ctx.render_ctx || !ctx.pipeline_map) return;
    if (entry.gbuffer_group == 0) return; // rasterizer hasn't allocated the G-buffer yet

    // Only run on raster-path views. RT views produce their final
    // image through the ray-tracer's compute+blit pipeline.
    if (auto* storage = interface_cast<IObjectStorage>(entry.camera_element)) {
        if (auto* cam = interface_cast<ICamera>(storage->find_attachment<ICamera>())) {
            if (auto cs = read_state<ICamera>(cam)) {
                if (cs->render_path == RenderPath::RayTrace) return;
            }
        }
    }

    int w = entry.gbuffer_width;
    int h = entry.gbuffer_height;
    if (w <= 0 || h <= 0) return;

    // Allocate / resize the output storage image.
    if (entry.deferred_output_tex != 0 &&
        (entry.deferred_width != w || entry.deferred_height != h)) {
        if (ctx.resources) {
            ctx.resources->defer_texture_destroy(
                entry.deferred_output_tex, ctx.present_counter + ctx.latency_frames);
        }
        entry.deferred_output_tex = 0;
    }
    if (entry.deferred_output_tex == 0) {
        TextureDesc td{};
        td.width = w;
        td.height = h;
        td.format = PixelFormat::RGBA8;
        td.usage = TextureUsage::Storage;
        entry.deferred_output_tex = ctx.backend->create_texture(td);
        entry.deferred_width = w;
        entry.deferred_height = h;
    }
    if (entry.deferred_output_tex == 0) return;

    // Compile the compute pipeline lazily. One pipeline shared across
    // all deferred views in this render context.
    if (pipeline_key_ == 0) {
        pipeline_key_ = ctx.render_ctx->compile_compute_pipeline(
            default_deferred_compute_src, kDeferredPipelineKey);
    }
    if (pipeline_key_ == 0) return;
    auto pit = ctx.pipeline_map->find(pipeline_key_);
    if (pit == ctx.pipeline_map->end()) return;

    // Look up G-buffer attachment texture ids for sampling.
    auto albedo_id   = ctx.backend->get_render_target_group_attachment(
        entry.gbuffer_group, static_cast<uint32_t>(GBufferAttachment::Albedo));
    auto normal_id   = ctx.backend->get_render_target_group_attachment(
        entry.gbuffer_group, static_cast<uint32_t>(GBufferAttachment::Normal));
    auto worldpos_id = ctx.backend->get_render_target_group_attachment(
        entry.gbuffer_group, static_cast<uint32_t>(GBufferAttachment::WorldPos));
    auto material_id = ctx.backend->get_render_target_group_attachment(
        entry.gbuffer_group, static_cast<uint32_t>(GBufferAttachment::MaterialParams));

    // Enumerate scene lights into the frame's GPU scratch. Same layout
    // as RayTracer's GpuLight so the shader's Light struct is shared.
    // TODO: refactor this walk into a shared helper used by both
    // RayTracer and DeferredLighter.
    struct GpuLight {
        uint32_t flags[4];
        float    position[4];
        float    direction[4];
        float    color_intensity[4];
        float    params[4];
    };
    static_assert(sizeof(GpuLight) == 80, "GpuLight layout mismatch");

    vector<GpuLight> lights;
    for (auto& ve : scene_state.visual_list) {
        if (ve.type != VisualEntry::Element || !ve.element) continue;
        auto es = read_state<IElement>(ve.element);
        if (!es) continue;
        auto* storage = interface_cast<IObjectStorage>(ve.element);
        if (!storage) continue;
        for (size_t j = 0; j < storage->attachment_count(); ++j) {
            auto* light = interface_cast<ILight>(storage->get_attachment(j));
            if (!light) continue;
            auto ls = read_state<ILight>(light);
            if (!ls) continue;
            GpuLight g{};
            g.flags[0] = static_cast<uint32_t>(ls->type);
            g.flags[1] = 0; // shadow_tech_id: deferred shadow composer is B.3.d
            g.position[0] = es->world_matrix(0, 3);
            g.position[1] = es->world_matrix(1, 3);
            g.position[2] = es->world_matrix(2, 3);
            float fx = -es->world_matrix(0, 2);
            float fy = -es->world_matrix(1, 2);
            float fz = -es->world_matrix(2, 2);
            float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
            if (flen > 1e-6f) { fx /= flen; fy /= flen; fz /= flen; }
            g.direction[0] = fx;
            g.direction[1] = fy;
            g.direction[2] = fz;
            g.color_intensity[0] = ls->color.r;
            g.color_intensity[1] = ls->color.g;
            g.color_intensity[2] = ls->color.b;
            g.color_intensity[3] = ls->intensity;
            g.params[0] = ls->range;
            constexpr float kDegToRad = 0.017453292519943295f;
            g.params[1] = std::cos(ls->cone_inner_deg * kDegToRad);
            g.params[2] = std::cos(ls->cone_outer_deg * kDegToRad);
            lights.push_back(g);
        }
    }
    uint64_t lights_addr = 0;
    if (!lights.empty() && ctx.frame_buffer) {
        lights_addr = ctx.frame_buffer->write(lights.data(), lights.size() * sizeof(GpuLight));
    }

    struct PushC {
        uint32_t output_image_id;
        uint32_t albedo_tex_id;
        uint32_t normal_tex_id;
        uint32_t worldpos_tex_id;
        uint32_t material_tex_id;
        uint32_t width;
        uint32_t height;
        uint32_t light_count;
        uint64_t lights_addr;
    };
    static_assert(sizeof(PushC) == 40, "Deferred PushC layout mismatch");

    PushC pc{};
    pc.output_image_id = entry.deferred_output_tex;
    pc.albedo_tex_id   = albedo_id;
    pc.normal_tex_id   = normal_id;
    pc.worldpos_tex_id = worldpos_id;
    pc.material_tex_id = material_id;
    pc.width  = static_cast<uint32_t>(w);
    pc.height = static_cast<uint32_t>(h);
    pc.light_count = static_cast<uint32_t>(lights.size());
    pc.lights_addr = lights_addr;

    // Compute pass: read G-buffer, write shaded color to deferred_output_tex.
    // The composite-to-surface pass is intentionally not emitted here;
    // the forward raster pass remains the visible output path until
    // every material ships a proper G-buffer fragment variant (SDF
    // corners, glyph coverage, gradient math). Re-enabling the
    // composite then becomes a one-line change; the pipeline source
    // (deferred_composite_{vertex,fragment}_src) and the cache slot
    // (composite_pipeline_key_) stay in place for that future switch.
    RenderPass compute_pass;
    compute_pass.kind = PassKind::Compute;
    compute_pass.compute.pipeline = pit->second;
    compute_pass.compute.groups_x = (w + 7) / 8;
    compute_pass.compute.groups_y = (h + 7) / 8;
    compute_pass.compute.groups_z = 1;
    compute_pass.compute.root_constants_size = sizeof(PushC);
    std::memcpy(compute_pass.compute.root_constants, &pc, sizeof(PushC));
    out_passes.push_back(std::move(compute_pass));
}

void DeferredLighter::on_view_removed(ViewEntry& entry, FrameContext& ctx)
{
    if (entry.deferred_output_tex != 0 && ctx.resources) {
        ctx.resources->defer_texture_destroy(
            entry.deferred_output_tex, ctx.present_counter + ctx.latency_frames);
        entry.deferred_output_tex = 0;
        entry.deferred_width = 0;
        entry.deferred_height = 0;
    }
}

void DeferredLighter::shutdown(FrameContext& /*ctx*/)
{
    // Per-view output textures are released via on_view_removed; the
    // shared compute pipeline lives in the render context's pipeline
    // map and is destroyed with it.
    pipeline_key_ = 0;
}

} // namespace velk::ui
