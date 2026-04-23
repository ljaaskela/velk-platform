#include "ray_tracer.h"

#include "default_ui_shaders.h"
#include "env_helper.h"
#include "frame_snippet_registry.h"
#include "scene_collector.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>
#include <velk/string.h>

#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_render_technique.h>
#include <velk-render/interface/intf_shadow_technique.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-ui/interface/intf_environment.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace velk::ui {

namespace {

// Same ortho builder the raster path uses as a fallback when no camera
// is attached. Duplicated here to keep the RT unit self-contained.
void build_ortho_projection(float* out, float width, float height)
{
    std::memset(out, 0, 16 * sizeof(float));
    out[0] = 2.0f / width;
    out[5] = 2.0f / height;
    out[10] = -1.0f;
    out[12] = -1.0f;
    out[13] = -1.0f;
    out[15] = 1.0f;
}

} // namespace

uint64_t RayTracer::ensure_pipeline(FrameContext& ctx)
{
    if (!ctx.render_ctx || !ctx.snippets) {
        return 0;
    }

    const auto& material_ids = ctx.snippets->frame_materials();
    const auto& shadow_tech_ids = ctx.snippets->frame_shadow_techs();
    const auto& intersect_ids = ctx.snippets->frame_intersects();
    const auto& material_info_by_id = ctx.snippets->material_info_by_id();
    const auto& shadow_tech_info_by_id = ctx.snippets->shadow_tech_info_by_id();
    const auto& intersect_info_by_id = ctx.snippets->intersect_info_by_id();

    // Pipeline cache key: FNV-1a across the sorted material / shadow /
    // intersect id lists. Different snippet combos compose to
    // different shaders, so they need distinct cache entries.
    constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
    constexpr uint64_t kRtTag = 0x5274436f6d702000ULL;
    uint64_t key = kFnvBasis ^ kRtTag;
    for (auto id : material_ids) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    key = (key ^ 0xdeadbeefULL) * kFnvPrime;
    for (auto id : shadow_tech_ids) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    key = (key ^ 0xfeedfaceULL) * kFnvPrime;
    for (auto id : intersect_ids) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    key |= 0x8000000000000000ULL;

    auto cached = compiled_pipelines_.find(key);
    if (cached != compiled_pipelines_.end()) {
        return key;
    }

    string src;
    src += rt_compute_prelude_src;
    for (auto id : material_ids) {
        if (id == 0 || id > material_info_by_id.size()) continue;
        const auto& mi = material_info_by_id[id - 1];
        src += string_view("#include \"", 10);
        src += mi.include_name;
        src += string_view("\"\n", 2);
    }
    for (auto id : shadow_tech_ids) {
        if (id == 0 || id > shadow_tech_info_by_id.size()) continue;
        const auto& ti = shadow_tech_info_by_id[id - 1];
        src += string_view("#include \"", 10);
        src += ti.include_name;
        src += string_view("\"\n", 2);
    }
    for (auto id : intersect_ids) {
        // Visual-contributed ids start at 3 and index intersect_info_by_id[id - 3].
        if (id < 3 || id - 3 >= intersect_info_by_id.size()) continue;
        const auto& ii = intersect_info_by_id[id - 3];
        src += string_view("#include \"", 10);
        src += ii.include_name;
        src += string_view("\"\n", 2);
    }

    auto append_literal = [&src](const char* s) {
        src += string_view(s, std::strlen(s));
    };

    // Order matters:
    //   1) velk_eval_shadow dispatch (called from velk_pbr_shade).
    //   2) velk_pbr_shade helper (calls velk_eval_shadow; called from
    //      velk_resolve_fill for Lit materials).
    //   3) velk_resolve_fill dispatch (calls velk_pbr_shade).

    char buf[128];

    append_literal("float velk_eval_shadow(uint tech_id, uint light_idx, vec3 world_pos, vec3 world_normal) {\n");
    append_literal("    switch (tech_id) {\n");
    for (auto id : shadow_tech_ids) {
        if (id == 0 || id > shadow_tech_info_by_id.size()) continue;
        const auto& ti = shadow_tech_info_by_id[id - 1];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += ti.fn_name;
        append_literal("(light_idx, world_pos, world_normal);\n");
    }
    append_literal("        default: return 1.0;\n");
    append_literal("    }\n");
    append_literal("}\n");

    // Shared PBR shading helper — defined after velk_eval_shadow (which
    // it calls) and before velk_resolve_fill (which calls it for Lit
    // materials).
    src += rt_pbr_shade_src;

    append_literal("BrdfSample velk_resolve_fill(uint mid, EvalContext ctx) {\n");
    append_literal("    switch (mid) {\n");
    for (auto id : material_ids) {
        if (id == 0 || id > material_info_by_id.size()) continue;
        const auto& mi = material_info_by_id[id - 1];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: { MaterialEval e = ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += mi.fn_name;
        append_literal("(ctx);"
                       " if (e.lighting_mode == VELK_LIGHTING_STANDARD)"
                       " return velk_pbr_shade(e, ctx);"
                       " BrdfSample bs;"
                       " bs.emission = e.color;"
                       " bs.throughput = vec3(0.0);"
                       " bs.next_dir = vec3(0.0);"
                       " bs.terminate = true;"
                       " bs.sample_count_hint = 1u;"
                       " return bs; }\n");
    }
    append_literal("        default: { BrdfSample bs; bs.emission = ctx.base; bs.throughput = vec3(0.0); bs.next_dir = vec3(0.0); bs.terminate = true; bs.sample_count_hint = 1u; return bs; }\n");
    append_literal("    }\n");
    append_literal("}\n");

    // Shape-intersect dispatch. Built-in kinds 0/1/2 forward to the
    // prelude's rect/cube/sphere intersect functions; visual-
    // contributed kinds (3+) call their registered snippets.
    append_literal("bool intersect_shape(Ray ray, RtShape shape, out RayHit hit) {\n");
    append_literal("    switch (shape.shape_kind) {\n");
    append_literal("        case 1u: return intersect_cube(ray, shape, hit);\n");
    append_literal("        case 2u: return intersect_sphere(ray, shape, hit);\n");
    for (auto id : intersect_ids) {
        if (id < 3 || id - 3 >= intersect_info_by_id.size()) continue;
        const auto& ii = intersect_info_by_id[id - 3];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += ii.fn_name;
        append_literal("(ray, shape, hit);\n");
    }
    append_literal("        default: return intersect_rect(ray, shape, hit);\n");
    append_literal("    }\n");
    append_literal("}\n");

    src += rt_compute_main_src;

    uint64_t compiled = ctx.render_ctx->compile_compute_pipeline(string_view(src), key);
    if (compiled == 0) {
        return 0;
    }
    compiled_pipelines_[key] = true;
    return key;
}

void RayTracer::build_passes(ViewEntry& entry,
                             const SceneState& scene_state,
                             FrameContext& ctx,
                             vector<RenderPass>& out_passes)
{
    if (!ctx.backend || !ctx.render_ctx || !ctx.frame_buffer || !ctx.resources ||
        !ctx.pipeline_map) {
        return;
    }

    auto sstate_rt = read_state<IWindowSurface>(entry.surface);
    float sw_full = static_cast<float>(sstate_rt ? sstate_rt->size.x : 0);
    float sh_full = static_cast<float>(sstate_rt ? sstate_rt->size.y : 0);
    bool has_vp = entry.viewport.width > 0 && entry.viewport.height > 0;
    float vp_x_f = has_vp ? entry.viewport.x * sw_full : 0.f;
    float vp_y_f = has_vp ? entry.viewport.y * sh_full : 0.f;
    float vp_w_f = has_vp ? entry.viewport.width * sw_full : sw_full;
    float vp_h_f = has_vp ? entry.viewport.height * sh_full : sh_full;
    int vp_w = static_cast<int>(vp_w_f);
    int vp_h = static_cast<int>(vp_h_f);
    if (vp_w <= 0 || vp_h <= 0) {
        return;
    }

    // (Re)create storage output texture sized to the viewport.
    if (entry.rt_output_tex != 0 &&
        (entry.rt_width != vp_w || entry.rt_height != vp_h)) {
        ctx.resources->defer_texture_destroy(
            entry.rt_output_tex, ctx.present_counter + ctx.latency_frames);
        entry.rt_output_tex = 0;
    }
    if (entry.rt_output_tex == 0) {
        TextureDesc td{};
        td.width = vp_w;
        td.height = vp_h;
        td.format = PixelFormat::RGBA8;
        td.usage = TextureUsage::Storage;
        entry.rt_output_tex = ctx.backend->create_texture(td);
        entry.rt_width = vp_w;
        entry.rt_height = vp_h;
    }
    if (entry.rt_output_tex == 0) {
        return;
    }

    // Resolve the camera (if any) so we can read its render_path and env.
    auto camera = ::velk::find_attachment<ICamera>(entry.camera_element);

    // Camera view-projection + world position for ray generation.
    mat4 vp_mat;
    if (camera) {
        auto cam_es = read_state<IElement>(entry.camera_element);
        mat4 cam_world = cam_es ? cam_es->world_matrix : mat4::identity();
        vp_mat = camera->get_view_projection(cam_world,
                                             static_cast<float>(vp_w),
                                             static_cast<float>(vp_h));
    } else {
        float tmp[16];
        build_ortho_projection(tmp, static_cast<float>(vp_w),
                               static_cast<float>(vp_h));
        std::memcpy(vp_mat.m, tmp, sizeof(vp_mat.m));
    }
    auto inv_vp = mat4::inverse(vp_mat);
    float cam_px = 0.f, cam_py = 0.f, cam_pz = 0.f;
    if (auto es = read_state<IElement>(entry.camera_element)) {
        cam_px = es->world_matrix(0, 3);
        cam_py = es->world_matrix(1, 3);
        cam_pz = es->world_matrix(2, 3);
    }

    // Write this view's FrameGlobals so the RT compute can route the
    // shared EvalContext::globals pointer to material eval bodies —
    // same shape every driver uses. The flat inv_view_projection /
    // cam_pos / bvh_* fields also present in the RT push constant are
    // kept because existing bounce / primary code still references
    // them directly; evals should reach globals through ctx.globals.
    uint64_t globals_gpu_addr = 0;
    {
        FrameGlobals globals{};
        std::memcpy(globals.view_projection, vp_mat.m, sizeof(vp_mat.m));
        std::memcpy(globals.inverse_view_projection, inv_vp.m, sizeof(inv_vp.m));
        globals.viewport[0] = static_cast<float>(vp_w);
        globals.viewport[1] = static_cast<float>(vp_h);
        globals.viewport[2] = 1.0f / static_cast<float>(vp_w);
        globals.viewport[3] = 1.0f / static_cast<float>(vp_h);
        globals.cam_pos[0] = cam_px;
        globals.cam_pos[1] = cam_py;
        globals.cam_pos[2] = cam_pz;
        globals.bvh_root = ctx.bvh_root;
        globals.bvh_node_count = ctx.bvh_node_count;
        globals.bvh_shape_count = ctx.bvh_shape_count;
        globals.bvh_nodes_addr = ctx.bvh_nodes_addr;
        globals.bvh_shapes_addr = ctx.bvh_shapes_addr;
        globals_gpu_addr = ctx.frame_buffer->write(&globals, sizeof(globals));
    }
    entry.frame_globals_addr = globals_gpu_addr;

    // Primary rays composite in painter order, which requires a
    // separate flat shape buffer. The scene-wide BVH (in ctx) has
    // element-grouped order; it's the right structure for closest-hit
    // bounces + shadows but wrong for co-planar UI compositing where
    // every overlapping shape on a plane must still contribute.
    // Materials resolve through the shared snippet registry so the
    // primary buffer and BVH use the same material ids.
    vector<RtShape> shapes;
    enumerate_scene_shapes(scene_state, ctx.render_ctx, [&](ShapeSite& site) {
        auto mat = site.paint
            ? ctx.snippets->resolve_material(site.paint, ctx)
            : FrameSnippetRegistry::MaterialRef{};
        if (site.paint && mat.mat_id == 0) {
            return;
        }
        uint32_t tex_id = 0;
        if (site.draw_entry && site.draw_entry->texture_key != 0) {
            auto* surf = reinterpret_cast<ISurface*>(
                static_cast<uintptr_t>(site.draw_entry->texture_key));
            tex_id = ctx.resources->find_texture(surf);
            if (tex_id == 0) {
                uint64_t rt_id = get_render_target_id(surf);
                if (rt_id != 0) tex_id = static_cast<uint32_t>(rt_id);
            }
        }
        site.geometry.material_id = mat.mat_id;
        site.geometry.material_data_addr = mat.mat_addr;
        site.geometry.texture_id = tex_id;
        if (auto* analytic = interface_cast<IAnalyticShape>(site.visual)) {
            uint32_t kind = ctx.snippets->register_intersect(analytic, *ctx.render_ctx);
            if (kind != 0) site.geometry.shape_kind = kind;
        }
        shapes.push_back(site.geometry);
    });

    vector<GpuLight> lights;
    enumerate_scene_lights(scene_state, [&](LightSite& site) {
        if (auto tech = find_shadow_technique(site.light)) {
            site.base.flags[1] =
                ctx.snippets->register_shadow_tech(tech.get(), *ctx.render_ctx);
        }
        lights.push_back(site.base);
    });

    uint64_t lights_addr = 0;
    if (!lights.empty()) {
        lights_addr = ctx.frame_buffer->write(
            lights.data(), lights.size() * sizeof(GpuLight));
    }

    // Environment: must register its material BEFORE compiling the pipeline
    // so its fill snippet is composed in.
    uint32_t env_mat_id = 0;
    uint32_t env_tex_id = 0;
    uint64_t env_data_addr = 0;
    if (camera) {
        auto resolved = ensure_env_ready(*camera, ctx);
        if (resolved.env) {
            env_tex_id = resolved.texture_id;
            auto env_mat = resolved.env->get_material();
            if (env_mat) {
                auto env_prog = interface_pointer_cast<IProgram>(env_mat);
                auto env_ref = ctx.snippets->resolve_material(env_prog.get(), ctx);
                env_mat_id = env_ref.mat_id;
                env_data_addr = env_ref.mat_addr;
            }
        }
    }

    uint64_t rt_pipeline_key = ensure_pipeline(ctx);
    if (rt_pipeline_key == 0) {
        return;
    }
    auto pit = ctx.pipeline_map->find(rt_pipeline_key);
    if (pit == ctx.pipeline_map->end()) {
        return;
    }

    // Plane-grouped back-to-front painter sort for the primary buffer.
    // Shapes that share a plane stay in enumeration order via
    // stable_sort, preserving authored layering on a flat UI panel.
    // Shapes on different planes sort by NDC depth of a representative
    // origin so stacked 3D panels composite back-to-front.
    if (shapes.size() > 1) {
        auto plane_key = [](const RtShape& s) -> uint64_t {
            float ux = s.u_axis[0], uy = s.u_axis[1], uz = s.u_axis[2];
            float vx = s.v_axis[0], vy = s.v_axis[1], vz = s.v_axis[2];
            float nx_r = uy * vz - uz * vy;
            float ny_r = uz * vx - ux * vz;
            float nz_r = ux * vy - uy * vx;
            float nlen = std::sqrt(nx_r * nx_r + ny_r * ny_r + nz_r * nz_r);
            if (nlen < 1e-6f) nlen = 1.f;
            float nx = nx_r / nlen;
            float ny = ny_r / nlen;
            float nz = nz_r / nlen;
            float offset = s.origin[0] * nx + s.origin[1] * ny + s.origin[2] * nz;
            int32_t qnx = static_cast<int32_t>(std::round(nx * 1000.f));
            int32_t qny = static_cast<int32_t>(std::round(ny * 1000.f));
            int32_t qnz = static_cast<int32_t>(std::round(nz * 1000.f));
            int32_t qo  = static_cast<int32_t>(std::round(offset * 100.f));
            uint64_t h = 0xcbf29ce484222325ULL;
            auto mix = [&h](uint32_t v) { h = (h ^ v) * 0x100000001b3ULL; };
            mix(static_cast<uint32_t>(qnx));
            mix(static_cast<uint32_t>(qny));
            mix(static_cast<uint32_t>(qnz));
            mix(static_cast<uint32_t>(qo));
            return h;
        };

        vector<uint64_t> keys(shapes.size());
        std::unordered_map<uint64_t, float> plane_depth;
        for (size_t i = 0; i < shapes.size(); ++i) {
            keys[i] = plane_key(shapes[i]);
            if (plane_depth.count(keys[i])) continue;
            const auto& s = shapes[i];
            float x = s.origin[0], y = s.origin[1], z = s.origin[2];
            float cz = vp_mat(2, 0) * x + vp_mat(2, 1) * y + vp_mat(2, 2) * z + vp_mat(2, 3);
            float cw = vp_mat(3, 0) * x + vp_mat(3, 1) * y + vp_mat(3, 2) * z + vp_mat(3, 3);
            plane_depth[keys[i]] = (cw != 0.f) ? (cz / cw) : 0.f;
        }

        vector<size_t> order(shapes.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
            [&](size_t a, size_t b) {
                uint64_t ka = keys[a];
                uint64_t kb = keys[b];
                if (ka == kb) return false;
                return plane_depth[ka] > plane_depth[kb];
            });

        vector<RtShape> sorted_shapes(shapes.size());
        for (size_t i = 0; i < order.size(); ++i) sorted_shapes[i] = shapes[order[i]];
        shapes = std::move(sorted_shapes);
    }

    uint64_t shapes_addr = 0;
    if (!shapes.empty()) {
        shapes_addr = ctx.frame_buffer->write(
            shapes.data(), shapes.size() * sizeof(RtShape));
    }

    VELK_GPU_STRUCT PushC {
        float inv_vp[16];
        float cam_pos[4];
        uint32_t image_index;
        uint32_t width;
        uint32_t height;
        uint32_t shape_count;
        uint32_t env_material_id;
        uint32_t env_texture_id;
        uint32_t frame_counter;
        uint32_t _env_pad1;
        uint64_t shapes_addr;
        uint64_t bvh_shapes_addr;
        uint64_t bvh_nodes_addr;
        uint32_t bvh_root;
        uint32_t bvh_node_count;
        uint64_t env_data_addr;
        uint64_t lights_addr;
        uint32_t light_count;
        uint32_t _lights_pad;
        uint64_t globals_addr;
    };

    PushC pc{};
    std::memcpy(pc.inv_vp, inv_vp.m, sizeof(pc.inv_vp));
    pc.cam_pos[0] = cam_px;
    pc.cam_pos[1] = cam_py;
    pc.cam_pos[2] = cam_pz;
    pc.cam_pos[3] = 0.f;
    pc.image_index = entry.rt_output_tex;
    pc.width = static_cast<uint32_t>(vp_w);
    pc.height = static_cast<uint32_t>(vp_h);
    pc.shape_count = static_cast<uint32_t>(shapes.size());
    pc.env_material_id = env_mat_id;
    pc.env_texture_id = env_tex_id;
    pc.frame_counter = static_cast<uint32_t>(ctx.present_counter);
    pc.shapes_addr = shapes_addr;
    pc.bvh_shapes_addr = ctx.bvh_shapes_addr;
    pc.bvh_nodes_addr = ctx.bvh_nodes_addr;
    pc.bvh_root = ctx.bvh_root;
    pc.bvh_node_count = ctx.bvh_node_count;
    pc.env_data_addr = env_data_addr;
    pc.lights_addr = lights_addr;
    pc.light_count = static_cast<uint32_t>(lights.size());
    pc.globals_addr = globals_gpu_addr;

    RenderPass pass;
    pass.kind = PassKind::ComputeBlit;
    pass.compute.pipeline = pit->second;
    pass.compute.groups_x = (vp_w + 7) / 8;
    pass.compute.groups_y = (vp_h + 7) / 8;
    pass.compute.groups_z = 1;
    pass.compute.root_constants_size = sizeof(PushC);
    std::memcpy(pass.compute.root_constants, &pc, sizeof(PushC));
    pass.blit_source = entry.rt_output_tex;
    pass.blit_surface_id = entry.surface ? entry.surface->get_render_target_id() : 0;
    pass.blit_dst_rect = {vp_x_f, vp_y_f, vp_w_f, vp_h_f};
    out_passes.push_back(std::move(pass));
}

void RayTracer::on_view_removed(ViewEntry& entry, FrameContext& ctx)
{
    if (entry.rt_output_tex != 0 && ctx.resources) {
        ctx.resources->defer_texture_destroy(
            entry.rt_output_tex, ctx.present_counter + ctx.latency_frames);
        entry.rt_output_tex = 0;
    }
}

void RayTracer::shutdown(FrameContext& ctx)
{
    // Storage textures are released on view removal; nothing per-RT-class
    // to release beyond that (material maps hold string_views owned by
    // their respective materials).
    (void)ctx;
}

} // namespace velk::ui
