#include "rasterizer.h"

#include "env_helper.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-render/gbuffer.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-ui/interface/intf_environment.h>
#include <velk-ui/interface/intf_render_to_texture.h>

#include <algorithm>
#include <cstring>

namespace velk::ui {

namespace {

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

void Rasterizer::prepend_environment_batch(ICamera& camera, ViewEntry& entry, FrameContext& ctx)
{
    auto resolved = ensure_env_ready(camera, ctx);
    if (!resolved.env || !resolved.surface) {
        return;
    }

    auto material = resolved.env->get_material();
    if (!material) {
        return;
    }

    // Synthetic front batch so the environment renders before all scene
    // geometry. The env vertex shader generates a fullscreen quad from
    // vertex index; instance data is ignored but instance_count must be 1
    // to produce a draw call.
    BatchBuilder::Batch env_batch;
    env_batch.pipeline_key = 0; // material override supplies the pipeline
    env_batch.texture_key = reinterpret_cast<uint64_t>(resolved.surface);
    env_batch.instance_stride = 4;
    env_batch.instance_count = 1;
    env_batch.instance_data.resize(4, 0);
    env_batch.material = std::move(material);

    entry.batches.insert(entry.batches.begin(), std::move(env_batch));
}

void Rasterizer::build_passes(ViewEntry& entry,
                              const SceneState& scene_state,
                              FrameContext& ctx,
                              vector<RenderPass>& out_passes)
{
    if (!ctx.backend || !ctx.frame_buffer || !ctx.batch_builder || !ctx.pipeline_map) {
        return;
    }

    auto camera = ::velk::find_attachment<ICamera>(entry.camera_element);

    bool deferred_view = false;
    if (camera) {
        if (auto cs = read_state<ICamera>(camera)) {
            deferred_view = (cs->render_path == RenderPath::Deferred);
        }
    }

    if (entry.batches_dirty) {
        ctx.batch_builder->rebuild_batches(scene_state, entry.batches);
        if (camera && !deferred_view) {
            prepend_environment_batch(*camera, entry, ctx);
        }
        entry.batches_dirty = false;
    }

    // Ensure each RTT subtree's backend texture exists and its
    // render_target_id is set BEFORE the main view pass's draw calls
    // get built — otherwise a TextureVisual sampling from an RTT sees
    // id=0 on frame one and the DrawCall has the zero address baked
    // in. This is idempotent across frames.
    ensure_render_targets(ctx);

    auto sstate = read_state<IWindowSurface>(entry.surface);
    float sw = static_cast<float>(sstate ? sstate->size.x : 0);
    float sh = static_cast<float>(sstate ? sstate->size.y : 0);
    bool has_viewport = entry.viewport.width > 0 && entry.viewport.height > 0;
    float vp_w = has_viewport ? entry.viewport.width * sw : sw;
    float vp_h = has_viewport ? entry.viewport.height * sh : sh;

    uint64_t globals_gpu_addr = 0;
    if (vp_w > 0 && vp_h > 0) {
        FrameGlobals globals{};
        mat4 vp_mat;
        if (camera) {
            auto cam_es = read_state<IElement>(entry.camera_element);
            mat4 cam_world = cam_es ? cam_es->world_matrix : mat4::identity();
            vp_mat = camera->get_view_projection(cam_world, vp_w, vp_h);
        } else {
            build_ortho_projection(globals.view_projection, vp_w, vp_h);
            std::memcpy(vp_mat.m, globals.view_projection, sizeof(vp_mat.m));
        }
        std::memcpy(globals.view_projection, vp_mat.m, sizeof(vp_mat.m));
        auto inv_vp = mat4::inverse(vp_mat);
        std::memcpy(globals.inverse_view_projection, inv_vp.m, sizeof(inv_vp.m));
        globals.viewport[0] = vp_w;
        globals.viewport[1] = vp_h;
        globals.viewport[2] = 1.0f / vp_w;
        globals.viewport[3] = 1.0f / vp_h;
        if (camera) {
            auto cam_es = read_state<IElement>(entry.camera_element);
            if (cam_es) {
                globals.cam_pos[0] = cam_es->world_matrix(0, 3);
                globals.cam_pos[1] = cam_es->world_matrix(1, 3);
                globals.cam_pos[2] = cam_es->world_matrix(2, 3);
            }
        }
        globals.bvh_root = ctx.bvh_root;
        globals.bvh_node_count = ctx.bvh_node_count;
        globals.bvh_shape_count = ctx.bvh_shape_count;
        globals.bvh_nodes_addr = ctx.bvh_nodes_addr;
        globals.bvh_shapes_addr = ctx.bvh_shapes_addr;
        globals_gpu_addr = ctx.frame_buffer->write(&globals, sizeof(globals));
    }
    // Stash so the DeferredLighter (and any future sub-renderer) can
    // reach the canonical GlobalData for this view via a pointer.
    entry.frame_globals_addr = globals_gpu_addr;

    if (deferred_view) {
        emit_deferred_gbuffer_pass(entry, ctx, static_cast<int>(vp_w),
                                   static_cast<int>(vp_h), globals_gpu_addr, out_passes);
    } else {
        float vp_x = has_viewport ? entry.viewport.x * sw : 0;
        float vp_y = has_viewport ? entry.viewport.y * sh : 0;
        emit_forward_pass(entry, ctx, globals_gpu_addr,
                          {vp_x, vp_y, vp_w, vp_h}, out_passes);
    }
}

void Rasterizer::emit_forward_pass(ViewEntry& entry, FrameContext& ctx,
                                   uint64_t globals_gpu_addr,
                                   const rect& viewport,
                                   vector<RenderPass>& out_passes)
{
    vector<DrawCall> draw_calls;
    ctx.batch_builder->build_draw_calls(entry.batches,
                                        draw_calls,
                                        *ctx.frame_buffer,
                                        *ctx.resources,
                                        globals_gpu_addr,
                                        ctx.pipeline_map,
                                        ctx.render_ctx,
                                        ctx.observer);

    RenderPass pass;
    pass.target.target = interface_pointer_cast<IRenderTarget>(entry.surface);
    pass.viewport = viewport;
    pass.draw_calls = std::move(draw_calls);
    out_passes.push_back(std::move(pass));
}

void Rasterizer::emit_deferred_gbuffer_pass(ViewEntry& entry, FrameContext& ctx,
                                            int width, int height,
                                            uint64_t globals_gpu_addr,
                                            vector<RenderPass>& out_passes)
{
    auto gbuffer_group = ensure_gbuffer(entry, width, height, ctx);
    if (gbuffer_group == 0) return;

    vector<DrawCall> gbuffer_draw_calls;
    ctx.batch_builder->build_gbuffer_draw_calls(entry.batches,
                                                gbuffer_draw_calls,
                                                *ctx.frame_buffer,
                                                *ctx.resources,
                                                globals_gpu_addr,
                                                ctx.render_ctx,
                                                gbuffer_group,
                                                ctx.observer);

    RenderPass g_pass;
    g_pass.kind = PassKind::GBufferFill;
    g_pass.gbuffer_group = gbuffer_group;
    g_pass.viewport = {0, 0, static_cast<float>(entry.gbuffer_width),
                       static_cast<float>(entry.gbuffer_height)};
    g_pass.draw_calls = std::move(gbuffer_draw_calls);
    out_passes.push_back(std::move(g_pass));
}

void Rasterizer::ensure_render_targets(FrameContext& ctx)
{
    if (!ctx.backend || !ctx.batch_builder || !ctx.resources) {
        return;
    }
    for (auto& rtp : ctx.batch_builder->render_target_passes()) {
        auto& rte = render_target_entries_[rtp.element];
        if (!rte.target) {
            if (auto rtt = ::velk::find_attachment<IRenderToTexture>(rtp.element)) {
                if (auto rtt_state = read_state<IRenderToTexture>(rtt)) {
                    rte.target = rtt_state->render_target.get<IRenderTarget>();
                }
            }
        }
        if (!rte.target) continue;

        int w{1}, h{1};
        if (auto es = read_state<IElement>(rtp.element)) {
            w = std::max(static_cast<int>(es->size.width), 1);
            h = std::max(static_cast<int>(es->size.height), 1);
        }
        if (rte.texture_id != 0 && (rte.width != w || rte.height != h)) {
            ctx.resources->defer_texture_destroy(
                rte.texture_id, ctx.present_counter + ctx.latency_frames);
            rte.texture_id = 0;
        }
        if (rte.texture_id == 0) {
            TextureDesc tdesc{};
            tdesc.width = w;
            tdesc.height = h;
            tdesc.format = PixelFormat::RGBA8;
            tdesc.usage = TextureUsage::RenderTarget;
            rte.texture_id = ctx.backend->create_texture(tdesc);
            rte.width = w;
            rte.height = h;
            if (rte.texture_id != 0) {
                rte.target->set_render_target_id(static_cast<uint64_t>(rte.texture_id));
            }
        }
    }
}

void Rasterizer::build_shared_passes(FrameContext& ctx, vector<RenderPass>& out_passes)
{
    if (!ctx.backend || !ctx.frame_buffer || !ctx.batch_builder || !ctx.pipeline_map) {
        return;
    }

    // Render-to-texture passes: one per element with a RenderCache trait
    // whose subtree has been batched into batch_builder_.render_target_passes()
    // during the per-view rebuild_batches calls. Texture allocation +
    // `render_target_id` wiring already happened in ensure_render_targets()
    // called from build_passes (so main-pass build_draw_calls sees the
    // correct id); here we just emit the draw pass into the target.
    for (auto& rtp : ctx.batch_builder->render_target_passes()) {
        auto it = render_target_entries_.find(rtp.element);
        if (it == render_target_entries_.end()) continue;
        auto& rte = it->second;
        if (!rte.target || rte.texture_id == 0) continue;

        FrameGlobals rt_globals{};
        build_ortho_projection(
            rt_globals.view_projection, static_cast<float>(rte.width), static_cast<float>(rte.height));
        rt_globals.viewport[0] = static_cast<float>(rte.width);
        rt_globals.viewport[1] = static_cast<float>(rte.height);
        rt_globals.viewport[2] = 1.0f / static_cast<float>(rte.width);
        rt_globals.viewport[3] = 1.0f / static_cast<float>(rte.height);
        rt_globals.bvh_root = ctx.bvh_root;
        rt_globals.bvh_node_count = ctx.bvh_node_count;
        rt_globals.bvh_shape_count = ctx.bvh_shape_count;
        rt_globals.bvh_nodes_addr = ctx.bvh_nodes_addr;
        rt_globals.bvh_shapes_addr = ctx.bvh_shapes_addr;
        uint64_t globals_gpu_addr = ctx.frame_buffer->write(&rt_globals, sizeof(rt_globals));

        vector<DrawCall> draw_calls;
        ctx.batch_builder->build_draw_calls(rtp.batches,
                                            draw_calls,
                                            *ctx.frame_buffer,
                                            *ctx.resources,
                                            globals_gpu_addr,
                                            ctx.pipeline_map,
                                            ctx.render_ctx,
                                            ctx.observer);

        RenderPass rt_pass{};
        rt_pass.target.target = rte.target;
        rt_pass.viewport = {0, 0, static_cast<float>(rte.width), static_cast<float>(rte.height)};
        rt_pass.draw_calls = std::move(draw_calls);
        out_passes.push_back(std::move(rt_pass));
        rte.dirty = false;
    }
}

RenderTargetGroup Rasterizer::ensure_gbuffer(ViewEntry& view, int width, int height,
                                             FrameContext& ctx)
{
    if (width <= 0 || height <= 0 || !ctx.backend) {
        return 0;
    }
    if (view.gbuffer_group != 0 &&
        view.gbuffer_width == width && view.gbuffer_height == height) {
        return view.gbuffer_group;
    }
    // Size changed or first allocation: drop the old group and build anew.
    if (view.gbuffer_group != 0) {
        ctx.backend->destroy_render_target_group(view.gbuffer_group);
        view.gbuffer_group = 0;
    }
    view.gbuffer_group = ctx.backend->create_render_target_group(
        array_view<const PixelFormat>(kGBufferFormats,
                                      static_cast<uint32_t>(GBufferAttachment::Count)),
        width, height);
    if (view.gbuffer_group != 0) {
        view.gbuffer_width = width;
        view.gbuffer_height = height;
    }
    return view.gbuffer_group;
}

void Rasterizer::on_view_removed(ViewEntry& view, FrameContext& ctx)
{
    // Raster per-view state (batches vector) lives on ViewEntry and is
    // released when the entry itself is destroyed. The G-buffer group is
    // Rasterizer-owned per view, so release it here.
    if (view.gbuffer_group != 0 && ctx.backend) {
        ctx.backend->destroy_render_target_group(view.gbuffer_group);
        view.gbuffer_group = 0;
        view.gbuffer_width = 0;
        view.gbuffer_height = 0;
    }
}

void Rasterizer::on_element_removed(IElement* elem, FrameContext& ctx)
{
    auto rit = render_target_entries_.find(elem);
    if (rit == render_target_entries_.end()) {
        return;
    }
    if (rit->second.texture_id != 0 && ctx.resources) {
        ctx.resources->defer_texture_destroy(
            rit->second.texture_id, ctx.present_counter + ctx.latency_frames);
    }
    render_target_entries_.erase(rit);
}

void Rasterizer::shutdown(FrameContext& ctx)
{
    for (auto& [key, rte] : render_target_entries_) {
        if (rte.texture_id != 0 && ctx.backend) {
            ctx.backend->destroy_texture(rte.texture_id);
        }
    }
    render_target_entries_.clear();
}

} // namespace velk::ui
