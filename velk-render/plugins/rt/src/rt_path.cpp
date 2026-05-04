#include "rt_path.h"

#include <velk/api/perf.h>
#include <velk/api/velk.h>
#include <velk/string.h>

#include <velk-render/frame/compute_shaders.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/plugin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace velk {

uint64_t RtPath::ensure_pipeline(FrameContext& ctx)
{
    if (!ctx.render_ctx || !ctx.snippets) {
        return 0;
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

    auto cached = compiled_pipelines_.find(key);
    if (cached != compiled_pipelines_.end()) {
        return key;
    }

    string src = compose_rt_compute(*ctx.snippets);

    uint64_t compiled = ctx.render_ctx->compile_compute_pipeline(string_view(src), key);
    if (compiled == 0) {
        return 0;
    }
    compiled_pipelines_[key] = true;
    return key;
}

void RtPath::build_passes(ViewEntry& entry,
                             const RenderView& render_view,
                             IRenderTarget::Ptr color_target,
                             FrameContext& ctx,
                             IRenderGraph& graph)
{
    if (!ctx.backend || !ctx.render_ctx || !ctx.frame_buffer || !ctx.resources ||
        !ctx.pipeline_map) {
        return;
    }
    if (render_view.width <= 0 || render_view.height <= 0) {
        return;
    }
    int vp_w = render_view.width;
    int vp_h = render_view.height;

    auto& vs = view_states_[&entry];

    // Per-frame allocation: drop the prior Ptr (manager parks the
    // handle on its pool) and request a fresh one. Steady-state hits
    // the pool once the parked handle's GPU work retires.
    {
        TextureDesc td{};
        td.width = vp_w;
        td.height = vp_h;
        td.format = PixelFormat::RGBA8;
        td.usage = TextureUsage::Storage;
        vs.rt_output = graph.resources().create_render_texture(td);
    }
    if (!vs.rt_output) {
        return;
    }

    uint64_t lights_addr = 0;
    if (!render_view.lights.empty()) {
        lights_addr = ctx.frame_buffer->write(
            render_view.lights.data(),
            render_view.lights.size() * sizeof(GpuLight));
    }

    // Make a working copy of the shapes so we can plane-sort without
    // affecting other consumers of render_view.shapes (today nobody
    // else uses it, but RenderView is immutable from a path's POV).
    vector<RtShape> shapes(render_view.shapes);

    uint64_t rt_pipeline_key = ensure_pipeline(ctx);
    if (rt_pipeline_key == 0) {
        return;
    }
    auto pit = ctx.pipeline_map->find(
        PipelineCacheKey{rt_pipeline_key, PixelFormat::Surface, 0});
    if (pit == ctx.pipeline_map->end()) {
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
        uint32_t _env_pad0;
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
    };

    PushC pc{};
    std::memcpy(pc.inv_vp, render_view.inverse_view_projection.m, sizeof(pc.inv_vp));
    pc.cam_pos[0] = render_view.cam_pos.x;
    pc.cam_pos[1] = render_view.cam_pos.y;
    pc.cam_pos[2] = render_view.cam_pos.z;
    pc.cam_pos[3] = 0.f;
    pc.image_index = static_cast<uint32_t>(vs.rt_output->get_gpu_handle(GpuResourceKey::Default));
    pc.width = static_cast<uint32_t>(vp_w);
    pc.height = static_cast<uint32_t>(vp_h);
    pc.shape_count = static_cast<uint32_t>(shapes.size());
    pc.env_material_id = render_view.env.material_id;
    pc.env_texture_id = render_view.env.texture_id;
    pc.shapes_addr = shapes_addr;
    pc.bvh_shapes_addr = render_view.bvh_shapes_addr;
    pc.bvh_nodes_addr = render_view.bvh_nodes_addr;
    pc.bvh_root = render_view.bvh_root;
    pc.bvh_node_count = render_view.bvh_node_count;
    pc.env_data_addr = render_view.env.data_addr;
    pc.lights_addr = lights_addr;
    pc.light_count = static_cast<uint32_t>(render_view.lights.size());

    DispatchCall dc{};
    dc.pipeline = pit->second;
    dc.groups_x = (vp_w + 7) / 8;
    dc.groups_y = (vp_h + 7) / 8;
    dc.groups_z = 1;
    dc.root_constants_size = sizeof(PushC);
    std::memcpy(dc.root_constants, &pc, sizeof(PushC));

    auto gp = ::velk::instance().create<IRenderPass>(ClassId::DefaultRenderPass);
    if (!gp) return;
    gp->add_op(ops::Dispatch{dc});
    gp->add_op(ops::BlitToSurface{
        static_cast<TextureId>(vs.rt_output->get_gpu_handle(GpuResourceKey::Default)),
        color_target ? color_target->get_gpu_handle(GpuResourceKey::Default) : 0,
        render_view.viewport});
    gp->add_write(interface_pointer_cast<IGpuResource>(vs.rt_output));
    if (color_target) gp->add_write(interface_pointer_cast<IGpuResource>(color_target));
    gp->set_view_globals(render_view.view_globals_buffer,
                          render_view.view_globals_offset,
                          render_view.view_globals_range);
    graph.add_pass(std::move(gp));
}

void RtPath::on_view_removed(ViewEntry& entry, FrameContext& /*ctx*/)
{
    // Erase the view state; vs.rt_output's Ptr drops, resource
    // manager auto-defers the backend handle.
    view_states_.erase(&entry);
}

void RtPath::shutdown(FrameContext& /*ctx*/)
{
    view_states_.clear();
}

} // namespace velk
