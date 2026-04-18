#include "deferred_lighter.h"

#include "default_ui_shaders.h"
#include "env_helper.h"
#include "scene_collector.h"

#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-render/gbuffer.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-ui/interface/intf_environment.h>

#include <cstdlib>
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

    // Only run for Deferred views. Forward views skip the deferred
    // pipeline entirely; RT views produce their final image through
    // the ray-tracer's compute+blit pipeline.
    ICamera* camera = nullptr;
    if (auto* storage = interface_cast<IObjectStorage>(entry.camera_element)) {
        camera = interface_cast<ICamera>(storage->find_attachment<ICamera>());
    }
    bool is_deferred = false;
    if (camera) {
        if (auto cs = read_state<ICamera>(camera)) {
            is_deferred = (cs->render_path == RenderPath::Deferred);
        }
    }
    if (!is_deferred) return;

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

    // Shadow-caster geometry: mirror the RT shape buffer so the shadow
    // ray cast can reuse the intersect_* helpers. Material fields stay
    // zero because shadow rays don't consult them.
    vector<RtShape> shapes;
    shapes.reserve(scene_state.visual_list.size());
    enumerate_scene_shapes(scene_state, [&](ShapeSite& site) {
        shapes.push_back(site.geometry);
    });
    uint64_t shapes_addr = 0;
    if (!shapes.empty() && ctx.frame_buffer) {
        shapes_addr = ctx.frame_buffer->write(shapes.data(), shapes.size() * sizeof(RtShape));
    }

    // Scene lights. Deferred compute only hardcodes rt_shadow (tech
    // id 1); any other technique maps to 0 (no shadow) rather than
    // silently calling the wrong function. When a second technique
    // lands we'll switch to composed dispatch like RayTracer.
    vector<GpuLight> lights;
    enumerate_scene_lights(scene_state, [&](LightSite& site) {
        if (auto* tech = find_shadow_technique(site.light)) {
            if (tech->get_snippet_fn_name() == string_view("velk_shadow_rt")) {
                site.base.flags[1] = 1;
            }
        }
        lights.push_back(site.base);
    });
    uint64_t lights_addr = 0;
    if (!lights.empty() && ctx.frame_buffer) {
        lights_addr = ctx.frame_buffer->write(lights.data(), lights.size() * sizeof(GpuLight));
    }

    // Resolve the environment (equirect HDR + intensity/rotation params).
    // 0 texture_id means "no environment" — shader's env_miss_color
    // returns vec3(0) and env terms drop out.
    uint32_t env_texture_id = 0;
    uint64_t env_data_addr  = 0;
    if (camera) {
        auto resolved = ensure_env_ready(*camera, ctx);
        if (resolved.env) {
            env_texture_id = resolved.texture_id;
            if (auto env_mat = resolved.env->get_material()) {
                auto env_prog = interface_pointer_cast<IProgram>(env_mat);
                if (auto* dd = interface_cast<IDrawData>(env_prog.get())) {
                    size_t sz = dd->get_draw_data_size();
                    if (sz > 0 && ctx.frame_buffer) {
                        void* scratch = std::malloc(sz);
                        if (scratch) {
                            std::memset(scratch, 0, sz);
                            if (dd->write_draw_data(scratch, sz) == ReturnValue::Success) {
                                env_data_addr = ctx.frame_buffer->write(scratch, sz);
                            }
                            std::free(scratch);
                        }
                    }
                }
            }
        }
    }

    // Camera world position for view direction V in PBR shading.
    float cam_px = 0.f, cam_py = 0.f, cam_pz = 0.f;
    mat4 cam_world = mat4::identity();
    if (auto es = read_state<IElement>(entry.camera_element)) {
        cam_px = es->world_matrix(0, 3);
        cam_py = es->world_matrix(1, 3);
        cam_pz = es->world_matrix(2, 3);
        cam_world = es->world_matrix;
    }

    // Inverse view-projection: needed by the compute shader to reconstruct
    // world-space ray directions for pixels with no G-buffer coverage
    // (sky path). Written into the frame buffer and referenced by the
    // shader via buffer_reference to keep push-constant footprint small.
    uint64_t inv_vp_addr = 0;
    if (camera && ctx.frame_buffer) {
        mat4 vp_mat = camera->get_view_projection(
            cam_world, static_cast<float>(w), static_cast<float>(h));
        mat4 inv_vp = mat4::inverse(vp_mat);
        inv_vp_addr = ctx.frame_buffer->write(inv_vp.m, sizeof(inv_vp.m));
    }

    // Layout mirrors std430 in the shader: vec4 first for 16-byte
    // alignment, uint64 buffer_references at the tail at 8-byte offsets.
    VELK_GPU_STRUCT PushC {
        float    cam_pos[4];       // offset 0
        uint32_t output_image_id;  // 16
        uint32_t albedo_tex_id;    // 20
        uint32_t normal_tex_id;    // 24
        uint32_t worldpos_tex_id;  // 28
        uint32_t material_tex_id;  // 32
        uint32_t width;            // 36
        uint32_t height;           // 40
        uint32_t light_count;      // 44
        uint32_t env_texture_id;   // 48
        uint32_t _env_pad;         // 52
        uint32_t shape_count;      // 56
        uint32_t _shape_pad;       // 60
        uint64_t lights_addr;      // 64
        uint64_t env_data_addr;    // 72
        uint64_t inv_vp_addr;      // 80
        uint64_t shapes_addr;      // 88
    };
    static_assert(sizeof(PushC) == 96, "Deferred PushC layout mismatch");

    PushC pc{};
    pc.cam_pos[0] = cam_px;
    pc.cam_pos[1] = cam_py;
    pc.cam_pos[2] = cam_pz;
    pc.cam_pos[3] = 0.f;
    pc.output_image_id = entry.deferred_output_tex;
    pc.albedo_tex_id   = albedo_id;
    pc.normal_tex_id   = normal_id;
    pc.worldpos_tex_id = worldpos_id;
    pc.material_tex_id = material_id;
    pc.width  = static_cast<uint32_t>(w);
    pc.height = static_cast<uint32_t>(h);
    pc.light_count = static_cast<uint32_t>(lights.size());
    pc.env_texture_id = env_texture_id;
    pc.shape_count = static_cast<uint32_t>(shapes.size());
    pc.lights_addr = lights_addr;
    pc.env_data_addr = env_data_addr;
    pc.inv_vp_addr = inv_vp_addr;
    pc.shapes_addr = shapes_addr;

    // Compute + blit: evaluate lighting into deferred_output_tex, then
    // blit to the surface subrect. Same pattern as RayTracer — a single
    // ComputeBlit pass keeps the path simple for now. A dedicated
    // composite render pass (shader source
    // deferred_composite_{vertex,fragment}_src, key slot
    // composite_pipeline_key_) stays compiled-out; we'd switch to it
    // only when multi-view alpha compositing is needed.
    auto sstate = read_state<IWindowSurface>(entry.surface);
    float sw = static_cast<float>(sstate ? sstate->size.x : 0);
    float sh = static_cast<float>(sstate ? sstate->size.y : 0);
    bool has_vp = entry.viewport.width > 0 && entry.viewport.height > 0;
    float vp_x = has_vp ? entry.viewport.x * sw : 0.f;
    float vp_y = has_vp ? entry.viewport.y * sh : 0.f;
    float vp_w = has_vp ? entry.viewport.width * sw : sw;
    float vp_h = has_vp ? entry.viewport.height * sh : sh;

    RenderPass pass;
    pass.kind = PassKind::ComputeBlit;
    pass.compute.pipeline = pit->second;
    pass.compute.groups_x = (w + 7) / 8;
    pass.compute.groups_y = (h + 7) / 8;
    pass.compute.groups_z = 1;
    pass.compute.root_constants_size = sizeof(PushC);
    std::memcpy(pass.compute.root_constants, &pc, sizeof(PushC));
    pass.blit_source = entry.deferred_output_tex;
    pass.blit_surface_id = entry.surface ? entry.surface->get_render_target_id() : 0;
    pass.blit_dst_rect = {vp_x, vp_y, vp_w, vp_h};
    out_passes.push_back(std::move(pass));
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
