#include "ray_tracer.h"

#include "default_ui_shaders.h"
#include "env_helper.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>
#include <velk/string.h>

#include <velk-render/interface/intf_material.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-ui/interface/intf_camera.h>
#include <velk-ui/interface/intf_environment.h>
#include <velk-ui/interface/intf_visual.h>

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

uint32_t RayTracer::register_material(IProgram* prog, FrameContext& ctx)
{
    if (!prog || !ctx.render_ctx) return 0;
    auto fill = prog->get_fill_src();
    auto fn = prog->get_fill_fn_name();
    auto inc = prog->get_fill_include_name();
    if (fill.empty() || fn.empty() || inc.empty()) return 0;
    auto* obj = interface_cast<IObject>(prog);
    if (!obj) return 0;
    Uid uid = obj->get_class_uid();
    uint64_t key = uid.hi ^ uid.lo;
    auto it = material_id_by_class_.find(key);
    if (it != material_id_by_class_.end()) {
        return it->second;
    }
    // Register the material's fill snippet as a shader include so the
    // composed shader can `#include "<name>"` and get proper line-number
    // diagnostics in compile errors.
    ctx.render_ctx->register_shader_include(inc, fill);
    // Let the material register any further include dependencies
    // (e.g. the text material's velk_text.glsl).
    prog->register_fill_includes(*ctx.render_ctx);
    uint32_t id = static_cast<uint32_t>(material_info_by_id_.size()) + 1;
    material_info_by_id_.push_back({fn, inc});
    material_id_by_class_[key] = id;
    return id;
}

uint64_t RayTracer::ensure_pipeline(const vector<uint32_t>& material_ids, FrameContext& ctx)
{
    if (!ctx.render_ctx) {
        return 0;
    }

    // Pipeline cache key: FNV-1a on the sorted material-id list.
    constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
    constexpr uint64_t kRtTag = 0x5274436f6d702000ULL;
    uint64_t key = kFnvBasis ^ kRtTag;
    for (auto id : material_ids) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    // Sentinel bit keeps RT keys well away from the render context's
    // auto-assign counter range (PipelineKey::CustomBase..).
    key |= 0x8000000000000000ULL;

    auto cached = compiled_pipelines_.find(key);
    if (cached != compiled_pipelines_.end()) {
        return key;
    }

    // Compose source: prelude + #include each active material's snippet
    // + generated dispatch switch + main.
    string src;
    src += rt_compute_prelude_src;
    for (auto id : material_ids) {
        if (id == 0 || id > material_info_by_id_.size()) continue;
        const auto& mi = material_info_by_id_[id - 1];
        src += string_view("#include \"", 10);
        src += mi.include_name;
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
        src += mi.fill_fn_name;
        append_literal("(ctx);\n");
    }
    append_literal("        default: { BrdfSample bs; bs.emission = ctx.base; bs.throughput = vec3(0.0); bs.next_dir = vec3(0.0); bs.terminate = true; return bs; }\n");
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
        vp_mat = camera->get_view_projection(*entry.camera_element,
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

    struct RtShape {
        float origin[4];     // xyz world corner
        float u_axis[4];     // xyz = local x dir * width
        float v_axis[4];     // xyz = local y dir * height
        float w_axis[4];     // xyz = local z dir * depth (cube only)
        float color[4];
        float params[4];     // x = corner radius (rect) or sphere radius
        uint32_t material_id;
        uint32_t texture_id;
        uint32_t shape_param;
        uint32_t shape_kind; // 0 = rect, 1 = cube, 2 = sphere
        uint64_t material_data_addr;
        uint64_t _tail_pad;
    };
    static_assert(sizeof(RtShape) == 128, "RtShape layout mismatch");

    vector<RtShape> shapes;
    shapes.reserve(scene_state.visual_list.size());
    frame_materials_.clear();

    for (auto& ve : scene_state.visual_list) {
        if (ve.type != VisualEntry::Element || !ve.element) continue;
        auto es = read_state<IElement>(ve.element);
        if (!es) continue;
        auto* storage = interface_cast<IObjectStorage>(ve.element);
        if (!storage) continue;
        float ox = es->world_matrix(0, 3);
        float oy = es->world_matrix(1, 3);
        float oz = es->world_matrix(2, 3);
        float ux = es->world_matrix(0, 0);
        float uy = es->world_matrix(1, 0);
        float uz = es->world_matrix(2, 0);
        float vx = es->world_matrix(0, 1);
        float vy = es->world_matrix(1, 1);
        float vz = es->world_matrix(2, 1);
        float wx = es->world_matrix(0, 2);
        float wy = es->world_matrix(1, 2);
        float wz = es->world_matrix(2, 2);
        float ew = es->size.width;
        float eh = es->size.height;
        float ed = es->size.depth;
        for (size_t j = 0; j < storage->attachment_count(); ++j) {
            auto* visual = interface_cast<IVisual>(storage->get_attachment(j));
            if (!visual) continue;
            auto vs = read_state<IVisual>(visual);
            if (!vs) continue;

            uint32_t mat_id = 0;
            uint64_t mat_addr = 0;
            if (vs->paint) {
                auto prog_ptr = vs->paint.get<IProgram>();
                IProgram* prog = prog_ptr.get();
                mat_id = register_material(prog, ctx);
                if (mat_id == 0) {
                    continue;
                }
                size_t sz = prog->gpu_data_size();
                if (sz > 0) {
                    void* scratch = std::malloc(sz);
                    if (scratch) {
                        std::memset(scratch, 0, sz);
                        if (prog->write_gpu_data(scratch, sz) == ReturnValue::Success) {
                            mat_addr = ctx.frame_buffer->write(scratch, sz);
                        }
                        std::free(scratch);
                    }
                }
                bool seen = false;
                for (auto id : frame_materials_) {
                    if (id == mat_id) { seen = true; break; }
                }
                if (!seen) frame_materials_.push_back(mat_id);
            }

            uint32_t shape_kind = visual->get_shape_kind();

            // Cube and sphere have no raster draw entries; we emit a single
            // shape directly from the element's 3D transform + size.
            if (shape_kind != 0) {
                RtShape s{};
                s.origin[0] = ox; s.origin[1] = oy; s.origin[2] = oz;
                s.u_axis[0] = ux * ew; s.u_axis[1] = uy * ew; s.u_axis[2] = uz * ew;
                s.v_axis[0] = vx * eh; s.v_axis[1] = vy * eh; s.v_axis[2] = vz * eh;
                s.w_axis[0] = wx * ed; s.w_axis[1] = wy * ed; s.w_axis[2] = wz * ed;
                s.color[0] = vs->color.r;
                s.color[1] = vs->color.g;
                s.color[2] = vs->color.b;
                s.color[3] = vs->color.a;
                if (shape_kind == 2) {
                    // Sphere inscribed in AABB: radius = half of min extent.
                    s.params[0] = std::min({ew, eh, ed}) * 0.5f;
                }
                s.material_id = mat_id;
                s.material_data_addr = mat_addr;
                s.shape_kind = shape_kind;
                shapes.push_back(s);
                continue;
            }

            // Rect path: iterate draw entries (one shape per entry).
            float radius = 0.f;
            if (!visual->get_intersect_src().empty()) {
                radius = std::min(std::min(ew, eh) * 0.5f, 12.f);
            }

            rect local_rect{0, 0, ew, eh};
            auto entries = visual->get_draw_entries(local_rect);
            for (auto& dentry : entries) {
                // Instance layout (instance_types.h):
                //   [ 0..63 ] mat4 world_matrix (raster only; ignored here
                //             because RT already has world axes from the
                //             element's world_matrix above)
                //   [64..71] vec2 pos
                //   [72..79] vec2 size
                //   [80..95] vec4 color
                //   [96..99] uint glyph_index   (TextInstance only)
                constexpr size_t kPosOff = 64;
                constexpr size_t kSizeOff = 72;
                constexpr size_t kColorOff = 80;
                constexpr size_t kGlyphIdxOff = 96;
                if (dentry.instance_size < kSizeOff + 8) continue;
                float local_px = 0, local_py = 0, sz_w = 0, sz_h = 0;
                std::memcpy(&local_px, dentry.instance_data + kPosOff, 4);
                std::memcpy(&local_py, dentry.instance_data + kPosOff + 4, 4);
                std::memcpy(&sz_w, dentry.instance_data + kSizeOff, 4);
                std::memcpy(&sz_h, dentry.instance_data + kSizeOff + 4, 4);

                float cr = 0, cg = 0, cb = 0, ca = 1;
                if (dentry.instance_size >= kColorOff + 16) {
                    std::memcpy(&cr, dentry.instance_data + kColorOff, 4);
                    std::memcpy(&cg, dentry.instance_data + kColorOff + 4, 4);
                    std::memcpy(&cb, dentry.instance_data + kColorOff + 8, 4);
                    std::memcpy(&ca, dentry.instance_data + kColorOff + 12, 4);
                } else {
                    cr = vs->color.r; cg = vs->color.g;
                    cb = vs->color.b; ca = vs->color.a;
                }

                uint32_t shape_param = 0;
                if (dentry.instance_size >= kGlyphIdxOff + 4) {
                    std::memcpy(&shape_param, dentry.instance_data + kGlyphIdxOff, 4);
                }

                uint32_t tex_id = 0;
                if (dentry.texture_key != 0) {
                    auto* surf = reinterpret_cast<ISurface*>(
                        static_cast<uintptr_t>(dentry.texture_key));
                    tex_id = ctx.resources->find_texture(surf);
                }

                float sx = ox + ux * local_px + vx * local_py;
                float sy = oy + uy * local_px + vy * local_py;
                float sz = oz + uz * local_px + vz * local_py;

                RtShape s{};
                s.origin[0] = sx;  s.origin[1] = sy;  s.origin[2] = sz;
                s.u_axis[0] = ux * sz_w; s.u_axis[1] = uy * sz_w; s.u_axis[2] = uz * sz_w;
                s.v_axis[0] = vx * sz_h; s.v_axis[1] = vy * sz_h; s.v_axis[2] = vz * sz_h;
                s.color[0] = cr; s.color[1] = cg; s.color[2] = cb; s.color[3] = ca;
                s.params[0] = radius;
                s.material_id = mat_id;
                s.texture_id = tex_id;
                s.shape_param = shape_param;
                s.material_data_addr = mat_addr;
                shapes.push_back(s);
            }
        }
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
                    size_t sz = prog->gpu_data_size();
                    if (sz > 0) {
                        void* scratch = std::malloc(sz);
                        if (scratch) {
                            std::memset(scratch, 0, sz);
                            if (prog->write_gpu_data(scratch, sz) == ReturnValue::Success) {
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
    uint64_t rt_pipeline_key = ensure_pipeline(frame_materials_, ctx);
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
    };
    static_assert(sizeof(PushC) == 128, "PushC layout mismatch");

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
