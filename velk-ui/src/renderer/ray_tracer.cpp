#include "ray_tracer.h"

#include "default_ui_shaders.h"
#include "env_helper.h"
#include "scene_collector.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>
#include <velk/string.h>

#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/intf_material.h>
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

uint32_t RayTracer::register_shadow_tech(IShadowTechnique* tech, FrameContext& ctx)
{
    if (!tech || !ctx.render_ctx) return 0;
    auto fn = tech->get_snippet_fn_name();
    auto src = tech->get_snippet_source();
    if (fn.empty() || src.empty()) return 0;
    auto* obj = interface_cast<IObject>(tech);
    if (!obj) return 0;
    Uid uid = obj->get_class_uid();
    uint64_t key = uid.hi ^ uid.lo;
    auto it = shadow_tech_id_by_class_.find(key);
    if (it != shadow_tech_id_by_class_.end()) {
        return it->second;
    }
    // The shader include filename is derived from fn_name + ".glsl".
    string include_name;
    include_name.append(fn);
    include_name.append(string_view(".glsl", 5));
    ctx.render_ctx->register_shader_include(include_name, src);
    tech->register_snippet_includes(*ctx.render_ctx);
    uint32_t id = static_cast<uint32_t>(shadow_tech_info_by_id_.size()) + 1;
    shadow_tech_info_by_id_.push_back({fn, std::move(include_name)});
    shadow_tech_id_by_class_[key] = id;
    return id;
}

uint32_t RayTracer::register_material(IProgram* prog, FrameContext& ctx)
{
    if (!prog || !ctx.render_ctx) return 0;
    auto* snippet = interface_cast<IShaderSnippet>(prog);
    if (!snippet) return 0;
    auto fn = snippet->get_snippet_fn_name();
    auto src = snippet->get_snippet_source();
    if (fn.empty() || src.empty()) return 0;
    auto* obj = interface_cast<IObject>(prog);
    if (!obj) return 0;
    Uid uid = obj->get_class_uid();
    uint64_t key = uid.hi ^ uid.lo;
    auto it = material_id_by_class_.find(key);
    if (it != material_id_by_class_.end()) {
        return it->second;
    }
    // The shader include filename is derived from fn_name + ".glsl".
    string include_name;
    include_name.append(fn);
    include_name.append(string_view(".glsl", 5));
    // Register the material's fill snippet as a shader include so the
    // composed shader can `#include "<name>"` and get proper line-number
    // diagnostics in compile errors.
    ctx.render_ctx->register_shader_include(include_name, src);
    // Let the material register any further include dependencies
    // (e.g. the text material's velk_text.glsl).
    snippet->register_snippet_includes(*ctx.render_ctx);
    uint32_t id = static_cast<uint32_t>(material_info_by_id_.size()) + 1;
    material_info_by_id_.push_back({fn, std::move(include_name)});
    material_id_by_class_[key] = id;
    return id;
}

uint64_t RayTracer::ensure_pipeline(const vector<uint32_t>& material_ids,
                                    const vector<uint32_t>& shadow_tech_ids,
                                    FrameContext& ctx)
{
    if (!ctx.render_ctx) {
        return 0;
    }

    // Pipeline cache key: FNV-1a across the sorted material-id and
    // shadow-tech-id lists. Different technique combos compose to
    // different shaders, so they need distinct cache entries.
    constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
    constexpr uint64_t kRtTag = 0x5274436f6d702000ULL;
    uint64_t key = kFnvBasis ^ kRtTag;
    for (auto id : material_ids) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    // Separator so a material id X can't collide with a shadow id X.
    key = (key ^ 0xdeadbeefULL) * kFnvPrime;
    for (auto id : shadow_tech_ids) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    // Sentinel bit keeps RT keys well away from the render context's
    // auto-assign counter range (PipelineKey::CustomBase..).
    key |= 0x8000000000000000ULL;

    auto cached = compiled_pipelines_.find(key);
    if (cached != compiled_pipelines_.end()) {
        return key;
    }

    // Compose source: prelude + #include each active material & shadow
    // technique snippet + generated dispatch switches + main.
    string src;
    src += rt_compute_prelude_src;
    for (auto id : material_ids) {
        if (id == 0 || id > material_info_by_id_.size()) continue;
        const auto& mi = material_info_by_id_[id - 1];
        src += string_view("#include \"", 10);
        src += mi.include_name;
        src += string_view("\"\n", 2);
    }
    for (auto id : shadow_tech_ids) {
        if (id == 0 || id > shadow_tech_info_by_id_.size()) continue;
        const auto& ti = shadow_tech_info_by_id_[id - 1];
        src += string_view("#include \"", 10);
        src += ti.include_name;
        src += string_view("\"\n", 2);
    }

    auto append_literal = [&src](const char* s) {
        src += string_view(s, std::strlen(s));
    };

    append_literal("BrdfSample velk_resolve_fill(uint mid, FillContext ctx) {\n");
    append_literal("    switch (mid) {\n");
    char buf[128];
    for (auto id : material_ids) {
        if (id == 0 || id > material_info_by_id_.size()) continue;
        const auto& mi = material_info_by_id_[id - 1];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += mi.fn_name;
        append_literal("(ctx);\n");
    }
    append_literal("        default: { BrdfSample bs; bs.emission = ctx.base; bs.throughput = vec3(0.0); bs.next_dir = vec3(0.0); bs.terminate = true; return bs; }\n");
    append_literal("    }\n");
    append_literal("}\n");

    // Shadow dispatch: tech_id 0 means "no shadow" (fully lit).
    append_literal("float velk_eval_shadow(uint tech_id, uint light_idx, vec3 world_pos, vec3 world_normal) {\n");
    append_literal("    switch (tech_id) {\n");
    for (auto id : shadow_tech_ids) {
        if (id == 0 || id > shadow_tech_info_by_id_.size()) continue;
        const auto& ti = shadow_tech_info_by_id_[id - 1];
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
    ICamera* camera = nullptr;
    if (auto* storage = interface_cast<IObjectStorage>(entry.camera_element)) {
        camera = interface_cast<ICamera>(storage->find_attachment<ICamera>());
    }

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

    vector<RtShape> shapes;
    shapes.reserve(scene_state.visual_list.size());
    frame_materials_.clear();

    // Cache of paint-program -> (material_id, material_data_addr). The
    // same visual may emit multiple rect draw entries; we only want to
    // register + write each material once per frame.
    struct MaterialEntry {
        IProgram* prog;
        uint32_t mat_id;
        uint64_t mat_addr;
    };
    vector<MaterialEntry> material_cache;

    enumerate_scene_shapes(scene_state, [&](ShapeSite& site) {
        uint32_t mat_id = 0;
        uint64_t mat_addr = 0;
        if (site.paint) {
            MaterialEntry* entry = nullptr;
            for (auto& me : material_cache) {
                if (me.prog == site.paint) { entry = &me; break; }
            }
            if (!entry) {
                uint32_t id = register_material(site.paint, ctx);
                if (id == 0) return;
                uint64_t addr = 0;
                if (auto* dd = interface_cast<IDrawData>(site.paint)) {
                    size_t sz = dd->get_draw_data_size();
                    if (sz > 0) {
                        void* scratch = std::malloc(sz);
                        if (scratch) {
                            std::memset(scratch, 0, sz);
                            if (dd->write_draw_data(scratch, sz) == ReturnValue::Success) {
                                addr = ctx.frame_buffer->write(scratch, sz);
                            }
                            std::free(scratch);
                        }
                    }
                }
                material_cache.push_back({site.paint, id, addr});
                entry = &material_cache.back();
                bool seen = false;
                for (auto fm : frame_materials_) {
                    if (fm == id) { seen = true; break; }
                }
                if (!seen) frame_materials_.push_back(id);
            }
            mat_id = entry->mat_id;
            mat_addr = entry->mat_addr;
        }

        uint32_t tex_id = 0;
        if (site.draw_entry && site.draw_entry->texture_key != 0) {
            auto* surf = reinterpret_cast<ISurface*>(
                static_cast<uintptr_t>(site.draw_entry->texture_key));
            tex_id = ctx.resources->find_texture(surf);
            // RTT textures aren't tracked in resources_; their bindless
            // id lives on the IRenderTarget. Fall back to that, mirroring
            // what batch_builder does for the raster path.
            if (tex_id == 0) {
                uint64_t rt_id = get_render_target_id(surf);
                if (rt_id != 0) {
                    tex_id = static_cast<uint32_t>(rt_id);
                }
            }
        }

        site.geometry.material_id = mat_id;
        site.geometry.material_data_addr = mat_addr;
        site.geometry.texture_id = tex_id;
        shapes.push_back(site.geometry);
    });

    vector<GpuLight> lights;
    frame_shadow_techs_.clear();
    enumerate_scene_lights(scene_state, [&](LightSite& site) {
        if (auto* tech = find_shadow_technique(site.light)) {
            uint32_t id = register_shadow_tech(tech, ctx);
            site.base.flags[1] = id;
            if (id != 0) {
                bool seen = false;
                for (auto fs : frame_shadow_techs_) {
                    if (fs == id) { seen = true; break; }
                }
                if (!seen) frame_shadow_techs_.push_back(id);
            }
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
                IProgram* prog = env_prog.get();
                env_mat_id = register_material(prog, ctx);
                if (env_mat_id != 0) {
                    auto* dd = interface_cast<IDrawData>(prog);
                    size_t sz = dd ? dd->get_draw_data_size() : 0;
                    if (sz > 0) {
                        void* scratch = std::malloc(sz);
                        if (scratch) {
                            std::memset(scratch, 0, sz);
                            if (dd->write_draw_data(scratch, sz) == ReturnValue::Success) {
                                env_data_addr = ctx.frame_buffer->write(scratch, sz);
                            }
                            std::free(scratch);
                        }
                    }
                    bool seen = false;
                    for (auto id : frame_materials_) {
                        if (id == env_mat_id) { seen = true; break; }
                    }
                    if (!seen) frame_materials_.push_back(env_mat_id);
                }
            }
        }
    }

    std::sort(frame_materials_.begin(), frame_materials_.end());
    std::sort(frame_shadow_techs_.begin(), frame_shadow_techs_.end());
    uint64_t rt_pipeline_key = ensure_pipeline(frame_materials_, frame_shadow_techs_, ctx);
    if (rt_pipeline_key == 0) {
        return;
    }
    auto pit = ctx.pipeline_map->find(rt_pipeline_key);
    if (pit == ctx.pipeline_map->end()) {
        return;
    }

    // Plane-grouped back-to-front sort for alpha compositing. Shapes that
    // share a plane (same normal + same offset, up to a quantisation
    // tolerance) stay in enumeration order via stable_sort, preserving
    // authored layering on a flat UI panel regardless of camera angle.
    // Shapes on different planes sort by NDC depth of a representative
    // origin, so stacked 3D panels composite back-to-front.
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

        // Per-shape plane key and per-plane depth (NDC z of the first
        // shape encountered on that plane).
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
                return plane_depth[ka] > plane_depth[kb]; // farther first
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

    struct PushC {
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
        uint64_t env_data_addr;
        // Lights appended for the hybrid-lighting compositor. light_count
        // = 0 means no dynamic lights; env-based lighting still applies.
        uint64_t lights_addr;
        uint32_t light_count;
        uint32_t _lights_pad;
    };
    static_assert(sizeof(PushC) == 144, "PushC layout mismatch");

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
    pc.env_data_addr = env_data_addr;
    pc.lights_addr = lights_addr;
    pc.light_count = static_cast<uint32_t>(lights.size());

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
