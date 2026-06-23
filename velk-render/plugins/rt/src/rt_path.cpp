#include "rt_path.h"

#include <velk/api/perf.h>
#include <velk/api/velk.h>
#include <velk/string.h>

#include <velk-render/api/cached_view_pass.h>
#include <velk-render/frame/compute_shaders.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/plugin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace velk {

IGpuPipeline::Ptr RtPath::ensure_pipeline(FrameContext& ctx)
{
    if (!ctx.render_ctx || !ctx.snippets) {
        return {};
    }

    const auto& material_ids = ctx.snippets->frame_materials();
    const auto& shadow_tech_ids = ctx.snippets->frame_shadow_techs();
    const auto& intersect_ids = ctx.snippets->frame_intersects();

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

    // The weak pipeline cache is the source of truth: reuse the live
    // pipeline for this snippet combo if it's still held by a live RT pass,
    // otherwise compose + compile a fresh one.
    if (auto p = ctx.render_ctx->find_pipeline(
            PipelineCacheKey{key, PixelFormat::RGBA8, 0})) {
        return p;
    }

    string src = compose_rt_compute(*ctx.snippets);

    return ctx.render_ctx->compile_compute_pipeline(string_view(src), key);
}

void RtPath::build_passes(IViewEntry& entry,
                             const RenderView& render_view,
                             IRenderTarget::Ptr color_target,
                             FrameContext& ctx,
                             IRenderGraph& graph)
{
    if (!ctx.backend || !ctx.render_ctx || !ctx.frame_buffer || !ctx.resources) {
        return;
    }
    if (render_view.width <= 0 || render_view.height <= 0) {
        return;
    }
    int vp_w = render_view.width;
    int vp_h = render_view.height;

    auto [it, inserted] = view_states_.try_emplace(&entry);
    auto& vs = it->second;
    if (inserted) {
        entry.add_render_state_observer(this);
    }

    // Persistent allocation: keep the same Ptr across frames so
    // downstream PushC bindless ids and `add_write` resource refs stay
    // stable. Recreate only on size change (or first-time). Recreation
    // invalidates the cached RT pass (image_index in PushC changes).
    uvec2 want{static_cast<uint32_t>(vp_w), static_cast<uint32_t>(vp_h)};
    if (!vs.rt_output || vs.output_size != want) {
        TextureDesc td{};
        td.width = vp_w;
        td.height = vp_h;
        td.format = PixelFormat::RGBA8;
        td.usage = TextureUsage::Storage;
        vs.rt_output = graph.resources().create_render_texture(td);
        vs.output_size = want;
        vs.rt_dirty = true;
    }
    if (!vs.rt_output) {
        return;
    }

    // Persistent per-view lights buffer staged by ViewPreparer; address
    // is stable across frames so cached RT passes can embed it.
    uint64_t lights_addr = render_view.lights_addr;

    // Make a working copy of the shapes so we can plane-sort without
    // affecting other consumers of render_view.shapes (today nobody
    // else uses it, but RenderView is immutable from a path's POV).
    vector<RtShape> shapes(render_view.shapes);

    auto rt_pipeline = ensure_pipeline(ctx);
    if (!rt_pipeline) {
        return;
    }

    // Plane-grouped back-to-front painter sort for the primary buffer.
    // Shapes that share a plane stay in enumeration order via
    // stable_sort, preserving authored layering on a flat UI panel.
    // Shapes on different planes sort by NDC depth of a representative
    // origin so stacked 3D panels composite back-to-front.
    const mat4& vp_mat = render_view.view_projection;
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

    // Persistent per-view shapes buffer. Camera move re-sorts bytes,
    // PersistentBuffer signals changed → upload + new address. Static
    // frames are a memcmp + no-op. Real change → invalidate cached pass.
    auto shapes_staged = vs.shapes_buffer.upload(
        shapes.data(), shapes.size() * sizeof(RtShape), ctx);
    uint64_t shapes_addr = shapes_staged.address;
    if (shapes_staged.changed) vs.rt_dirty = true;

    // Catch BVH / shape-count drift that doesn't flow through view
    // notify (BVH lives at scene scope, not view).
    if (vs.rt_change.changed({
            render_view.bvh_nodes_addr,
            render_view.bvh_shapes_addr,
            shapes_addr,
            render_view.bvh_root,
            render_view.bvh_node_count,
            static_cast<uint32_t>(shapes.size())})) {
        vs.rt_dirty = true;
    }

    // Per-dispatch root struct mirroring the GLSL `RtRoot` buffer
    // reference (compute_shaders.h). Lives in `vs.root_buffer` and is
    // reached through an 8-byte BDA pushed as the only root constant.
    VELK_GPU_STRUCT RtRoot {
        uint64_t globals;          // FrameGlobals BDA
        float inv_vp[16];
        float cam_pos[4];
        uint32_t extras[4];        // image_index, width, height, shape_count
        uint32_t env[4];           // env_material_id, env_texture_id, _, _
        uint64_t shapes_addr;
        uint64_t bvh_shapes_addr;
        uint64_t bvh_nodes_addr;
        uint32_t bvh_root;
        uint32_t bvh_node_count;
        uint64_t env_data_addr;
        uint64_t lights_addr;
        uint32_t light_count;
        uint32_t _lights_pad;
    };

    RtRoot root{};
    root.globals = render_view.view_globals_address;
    std::memcpy(root.inv_vp, render_view.inverse_view_projection.m, sizeof(root.inv_vp));
    root.cam_pos[0] = render_view.cam_pos.x;
    root.cam_pos[1] = render_view.cam_pos.y;
    root.cam_pos[2] = render_view.cam_pos.z;
    root.cam_pos[3] = 0.f;
    root.extras[0] = static_cast<uint32_t>(vs.rt_output->get_gpu_handle(GpuResourceKey::Default));
    root.extras[1] = static_cast<uint32_t>(vp_w);
    root.extras[2] = static_cast<uint32_t>(vp_h);
    root.extras[3] = static_cast<uint32_t>(shapes.size());
    root.env[0] = render_view.env.material_id;
    root.env[1] = render_view.env.texture_id;
    root.env[2] = 0;
    root.env[3] = 0;
    root.shapes_addr = shapes_addr;
    root.bvh_shapes_addr = render_view.bvh_shapes_addr;
    root.bvh_nodes_addr = render_view.bvh_nodes_addr;
    root.bvh_root = render_view.bvh_root;
    root.bvh_node_count = render_view.bvh_node_count;
    root.env_data_addr = render_view.env.data_addr;
    root.lights_addr = lights_addr;
    root.light_count = static_cast<uint32_t>(render_view.lights.size());
    root._lights_pad = 0;

    if (!vs.root_buffer) {
        GpuBufferDesc bd{};
        bd.size = sizeof(RtRoot);
        bd.cpu_writable = true;
        vs.root_buffer = ctx.backend->create_gpu_buffer(bd);
        if (!vs.root_buffer) return;
        vs.rt_dirty = true;  // first-time alloc: cached secondary needs the new BDA
    }
    vs.root_buffer->update(0, sizeof(RtRoot), &root);
    const uint64_t root_addr = vs.root_buffer->gpu_address();

    // color_target is always an IGpuTexture-castable wrapper.
    IGpuTexture* rt_tex = graph.resources().find_texture(vs.rt_output.get());
    IGpuTexture* dst_tex = nullptr;
    if (color_target) {
        dst_tex = interface_cast<IGpuTexture>(color_target.get());
        if (!dst_tex) {
            dst_tex = graph.resources().find_texture(color_target.get());
            if (!dst_tex && ctx.resources) {
                dst_tex = ctx.resources->find_texture(color_target.get());
            }
        }
    }
    // Resize detection: stale-VkImage in cached cmd buffer.
    if (dst_tex != vs.last_dst_texture) {
        vs.rt_dirty = true;
        vs.last_dst_texture = dst_tex;
    }

    emit_cached_view_pass(
        vs.cached_rt_pass, vs.rt_dirty, render_view.view_globals_address, graph,
        [&](CachedPassRecording& rec) {
            DispatchCall dc{};
            dc.pipeline = rt_pipeline.get();
            dc.groups_x = (vp_w + 7) / 8;
            dc.groups_y = (vp_h + 7) / 8;
            dc.groups_z = 1;
            dc.root_constants_size = sizeof(uint64_t);
            std::memcpy(dc.root_constants, &root_addr, sizeof(uint64_t));

            if (auto cmd = ctx.backend->create_command_buffer()) {
                cmd->begin_recording();
                cmd->push_label("RtPath");
                cmd->record_dispatch(dc);
                if (rt_tex && dst_tex) {
                    cmd->record_blit_to_texture(*rt_tex, *dst_tex, render_view.viewport);
                }
                cmd->pop_label();
                cmd->end_recording();
                rec.cmd = std::move(cmd);
            }
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.rt_output));
            if (color_target) {
                rec.writes.push_back(interface_pointer_cast<IGpuResource>(color_target));
            }
            // Hold the RT compute pipeline strong (cache is weak).
            rec.held.push_back(std::move(rt_pipeline));
        });
}

RtPath::~RtPath()
{
    // Detach from every view we observed. The renderer's destruction
    // order keeps IViewEntry::Ptrs alive past path destruction, so
    // these calls are safe.
    for (auto& [view, _] : view_states_) {
        view->remove_render_state_observer(this);
    }
}

void RtPath::on_render_state_changed(::velk::IRenderState* source,
                                     ::velk::RenderStateChange /*flags*/)
{
    auto* view = interface_cast<IViewEntry>(source);
    if (!view) return;
    auto it = view_states_.find(view);
    if (it != view_states_.end()) {
        it->second.rt_dirty = true;
    }
}

void RtPath::on_view_removed(IViewEntry& entry, FrameContext& /*ctx*/)
{
    auto it = view_states_.find(&entry);
    if (it == view_states_.end()) return;
    entry.remove_render_state_observer(this);
    // Erase the view state; vs.rt_output's Ptr drops, resource
    // manager auto-defers the backend handle.
    view_states_.erase(it);
}

void RtPath::shutdown(FrameContext& /*ctx*/)
{
    for (auto& [view, _] : view_states_) {
        view->remove_render_state_observer(this);
    }
    view_states_.clear();
}

} // namespace velk
