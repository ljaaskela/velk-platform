#include "deferred_lighter.h"

#include "default_ui_shaders.h"
#include "env_helper.h"
#include "frame_snippet_registry.h"
#include "scene_collector.h"

#include <velk/api/state.h>
#include <velk/string.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-render/gbuffer.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-scene/interface/intf_environment.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace velk {

namespace {

constexpr uint64_t kDeferredCompositePipelineKey = 0xD0FE'2ED1'C0A1'1105ULL;

} // namespace

uint64_t DeferredLighter::ensure_pipeline(FrameContext& ctx)
{
    if (!ctx.render_ctx || !ctx.snippets) return 0;

    const auto& intersect_ids = ctx.snippets->frame_intersects();
    const auto& intersect_info_by_id = ctx.snippets->intersect_info_by_id();

    // Pipeline cache key: FNV-1a across the active intersect id set.
    // Different visual-contributed intersect snippets compose into
    // different shader variants.
    constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
    constexpr uint64_t kDeferredTag = 0x44665232'44666572ULL;
    uint64_t key = kFnvBasis ^ kDeferredTag;
    for (auto id : intersect_ids) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    key |= 0x4000000000000000ULL;

    if (compiled_pipelines_.find(key) != compiled_pipelines_.end()) {
        return key;
    }

    string src;
    src += default_deferred_compute_src;
    for (auto id : intersect_ids) {
        if (id < 3 || id - 3 >= intersect_info_by_id.size()) continue;
        const auto& ii = intersect_info_by_id[id - 3];
        src += string_view("#include \"", 10);
        src += ii.include_name;
        src += string_view("\"\n", 2);
    }

    auto append_literal = [&src](const char* s) {
        src += string_view(s, std::strlen(s));
    };

    // intersect_shape dispatch: built-in kinds 0/1/2 forward to the
    // prelude's rect/cube/sphere intersect functions; visual-registered
    // kinds (3+) call their registered snippets.
    append_literal("bool intersect_shape(Ray ray, RtShape shape, out RayHit hit) {\n");
    append_literal("    switch (shape.shape_kind) {\n");
    append_literal("        case 1u: return intersect_cube(ray, shape, hit);\n");
    append_literal("        case 2u: return intersect_sphere(ray, shape, hit);\n");
    append_literal("        case 255u: return intersect_mesh(ray, shape, hit);\n");
    char buf[128];
    for (auto id : intersect_ids) {
        if (id < 3 || id - 3 >= intersect_info_by_id.size()) continue;
        const auto& ii = intersect_info_by_id[id - 3];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        src += ii.fn_name;
        append_literal("(ray, shape, hit);\n");
    }
    append_literal("        default: return intersect_rect(ray, shape, hit);\n");
    append_literal("    }\n");
    append_literal("}\n");

    uint64_t compiled = ctx.render_ctx->compile_compute_pipeline(string_view(src), key);
    if (compiled == 0) return 0;
    compiled_pipelines_[key] = true;
    return key;
}

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
    auto camera = ::velk::find_attachment<ICamera>(entry.camera_element);
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

    // Per-view diagnostic image for RT-shadow debugging. Allocated once
    // and resized with the view; carries (buffer_addr_lo, buffer_addr_hi,
    // ibo_offset, triangle_count) per pixel as RGBA32F bits. Always
    // allocated for now (the GLSL write site is gated by the push
    // constant), so the F12 dump path always has something to read.
    if (entry.shadow_debug_tex != 0 &&
        (entry.shadow_debug_width != w || entry.shadow_debug_height != h)) {
        if (ctx.resources) {
            ctx.resources->defer_texture_destroy(
                entry.shadow_debug_tex, ctx.present_counter + ctx.latency_frames);
        }
        entry.shadow_debug_tex = 0;
    }
    if (entry.shadow_debug_tex == 0) {
        TextureDesc td{};
        td.width = w;
        td.height = h;
        td.format = PixelFormat::RGBA32F;
        td.usage = TextureUsage::Storage;
        entry.shadow_debug_tex = ctx.backend->create_texture(td);
        entry.shadow_debug_width = w;
        entry.shadow_debug_height = h;
    }

    // Compose the compute pipeline for the current frame's visual
    // intersect set. Different sets compose to different shader variants;
    // compile-once caches in compiled_pipelines_.
    uint64_t pipeline_key = ensure_pipeline(ctx);
    if (pipeline_key == 0) return;
    auto pit = ctx.pipeline_map->find(pipeline_key);
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

    // Shadow-caster geometry lives in the scene-wide BVH now, reached
    // through globals_addr -> GlobalData.bvh_{nodes,shapes}. See the
    // Renderer's per-frame BVH build.

    // Scene lights. Deferred compute only hardcodes rt_shadow (tech
    // id 1); any other technique maps to 0 (no shadow) rather than
    // silently calling the wrong function. When a second technique
    // lands we'll switch to composed dispatch like RayTracer.
    vector<GpuLight> lights;
    enumerate_scene_lights(scene_state,
        +[](void* u, LightSite& site) {
            auto& out = *static_cast<vector<GpuLight>*>(u);
            if (auto tech = find_shadow_technique(site.light)) {
                if (tech->get_snippet_fn_name() == string_view("velk_shadow_rt")) {
                    site.base.flags[1] = 1;
                }
            }
            out.push_back(site.base);
        }, &lights);
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
    // Inverse view-projection comes from the view's FrameGlobals
    // (stashed on entry by the Rasterizer), so no separate upload here.
    float cam_px = 0.f, cam_py = 0.f, cam_pz = 0.f;
    if (auto es = read_state<IElement>(entry.camera_element)) {
        cam_px = es->world_matrix(0, 3);
        cam_py = es->world_matrix(1, 3);
        cam_pz = es->world_matrix(2, 3);
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
        uint32_t shadow_debug_image_id; // 52  RGBA32F debug image, 0 = disabled
        uint64_t lights_addr;      // 56
        uint64_t env_data_addr;    // 64
        uint64_t globals_addr;     // 72 - pointer to GlobalData (carries inv_vp + BVH)
    };
    static_assert(sizeof(PushC) == 80, "Deferred PushC layout mismatch");

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
    pc.shadow_debug_image_id = entry.shadow_debug_tex;
    pc.lights_addr = lights_addr;
    pc.env_data_addr = env_data_addr;
    pc.globals_addr = entry.frame_globals_addr;

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
    // NB: forward-on-top-of-deferred depth composition is deferred — if
    // we want that later, populate pass.blit_depth_source_group and
    // restore the depth-preserving load_render_pass variant in the VK
    // backend so it doesn't get re-cleared.
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
    if (entry.shadow_debug_tex != 0 && ctx.resources) {
        ctx.resources->defer_texture_destroy(
            entry.shadow_debug_tex, ctx.present_counter + ctx.latency_frames);
        entry.shadow_debug_tex = 0;
        entry.shadow_debug_width = 0;
        entry.shadow_debug_height = 0;
    }
}

void DeferredLighter::shutdown(FrameContext& /*ctx*/)
{
    // Per-view output textures are released via on_view_removed; compiled
    // compute pipelines live in the render context's pipeline map and
    // are destroyed with it.
    compiled_pipelines_.clear();
}

} // namespace velk
