#include "view_preparer.h"

#include "batch_builder.h"
#include "default_batch.h"
#include "env_helper.h"
#include "pipeline_options_helpers.h"
#include "scene_collector.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_shadow_technique.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-scene/interface/intf_environment.h>
#include <velk-scene/interface/intf_visual.h>

#include <cstdlib>
#include <cstring>

namespace velk {

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

bool ViewPreparer::try_update_transforms(ViewEntry& entry,
                                         const SceneState& scene_state)
{
    auto cache_it = view_caches_.find(&entry);
    if (cache_it == view_caches_.end()) return false;
    auto& cache = cache_it->second;

    // Structural mutations always demand a full rebuild.
    if (!scene_state.removed_list.empty()) return false;
    if (cache.batches.empty()) return false;

    // Layout-only flag set is the safe transform-only signal. Any
    // Visual or DrawOrder bit reshapes the batch set.
    constexpr DirtyFlags kLayoutOnly = DirtyFlags::Layout;
    if ((scene_state.flags & ~kLayoutOnly) != DirtyFlags::None) return false;

    // Non-visual elements (cameras, lights, etc.) end up in the redraw
    // list when their transform changes but contribute no batches.
    // Skip them in both passes below; only visual-bearing elements
    // need slot updates.
    for (auto* elem : scene_state.redraw_list) {
        if (cache.rtt_roots.count(elem)) return false;
        if (!::velk::has_attachment<IVisual>(elem)) continue;
        if (!cache.element_slots.count(elem)) return false;
    }

    for (auto* elem : scene_state.redraw_list) {
        if (!::velk::has_attachment<IVisual>(elem)) continue;
        auto state = read_state<IElement>(elem);
        if (!state) return false;
        for (auto& slot : cache.element_slots[elem]) {
            mat4 world = state->world_matrix;
            world(0, 3) -= slot.offset_x;
            world(1, 3) -= slot.offset_y;
            slot.batch->update_instance_at(
                slot.instance_index,
                array_view<const uint8_t>(
                    reinterpret_cast<const uint8_t*>(world.m),
                    sizeof(world.m)));
        }
    }
    return true;
}

void ViewPreparer::prepare_batches(ViewEntry& entry, const SceneState& scene_state,
                                   BatchBuilder& batch_builder, FrameContext& ctx,
                                   RenderView& rv)
{
    auto& cache = view_caches_[&entry];
    if (entry.batches_dirty) {
        if (try_update_transforms(entry, scene_state)) {
            ++diag_fast_path_count;
            entry.batches_dirty = false;
        } else {
            ++diag_rebuild_count;
            batch_builder.rebuild_batches(scene_state, cache.batches,
                                          cache.element_slots, cache.rtt_roots);
            entry.batches_dirty = false;
        }
    }

    // Per-batch buffer upload: each batch owns its [args][count]
    // [instance_data] blob and implements IBuffer. ensure_buffer_storage
    // allocates / resizes the backing GpuBufferHandle; on a fresh allocation
    // we map it, copy the blob, and stamp the (handle, mapped) pair on
    // the batch so update_instance_at can write through later. Pre-
    // existing-handle batches with is_dirty (e.g. structural rebuilds
    // reusing the same hive slot) re-upload through their existing
    // mapping. ensure_buffer_storage's deferred-destroy on size change
    // handles the case where a batch's instance count grows.
    if (ctx.resources && ctx.backend) {
        auto upload = [&](vector<IBatch::Ptr>& batches) {
            for (auto& bp : batches) {
                if (!bp) continue;
                auto* batch = static_cast<impl::DefaultBatch*>(bp.get());
                if (!batch->is_dirty()) continue;
                size_t blob_size = batch->get_data_size();
                if (blob_size == 0) { batch->clear_dirty(); continue; }
                GpuBufferDesc desc{};
                desc.size = blob_size;
                desc.cpu_writable = true;
                auto* be = ctx.resources->ensure_buffer_storage(batch, desc);
                if (!be || !be->handle) continue;
                if (auto* dst = ctx.backend->map(be->handle)) {
                    std::memcpy(dst, batch->get_data(), blob_size);
                    batch->set_storage_mapping(be->handle, static_cast<uint8_t*>(dst));
                }
                batch->clear_dirty();
            }
        };
        upload(cache.batches);
        for (auto& rtp : batch_builder.render_target_passes()) {
            upload(rtp.batches);
        }
    }
    rv.batches = &cache.batches;
}

void ViewPreparer::prepare_camera(ViewEntry& entry, const IElement::Ptr& camera_element,
                                  FrameContext& /*ctx*/, RenderView& rv)
{
    auto sstate = read_state<IWindowSurface>(entry.surface);
    float sw = static_cast<float>(sstate ? sstate->size.x : 0);
    float sh = static_cast<float>(sstate ? sstate->size.y : 0);
    bool has_viewport = entry.viewport.width > 0 && entry.viewport.height > 0;
    float vp_x = has_viewport ? entry.viewport.x * sw : 0.f;
    float vp_y = has_viewport ? entry.viewport.y * sh : 0.f;
    float vp_w = has_viewport ? entry.viewport.width * sw : sw;
    float vp_h = has_viewport ? entry.viewport.height * sh : sh;
    rv.viewport = {vp_x, vp_y, vp_w, vp_h};
    rv.width = static_cast<int>(vp_w);
    rv.height = static_cast<int>(vp_h);
    if (vp_w <= 0 || vp_h <= 0) return;

    auto camera = ::velk::find_attachment<ICamera>(camera_element);
    if (camera) {
        auto cam_es = read_state<IElement>(camera_element);
        mat4 cam_world = cam_es ? cam_es->world_matrix : mat4::identity();
        rv.view_projection = camera->get_view_projection(cam_world, vp_w, vp_h);
        if (cam_es) {
            rv.cam_pos.x = cam_es->world_matrix(0, 3);
            rv.cam_pos.y = cam_es->world_matrix(1, 3);
            rv.cam_pos.z = cam_es->world_matrix(2, 3);
        }
        rv.frustum = ::velk::render::extract_frustum(rv.view_projection);
        rv.has_frustum = true;
    } else {
        float ortho[16];
        build_ortho_projection(ortho, vp_w, vp_h);
        std::memcpy(rv.view_projection.m, ortho, sizeof(ortho));
    }
    rv.inverse_view_projection = mat4::inverse(rv.view_projection);
}

void ViewPreparer::prepare_frame_globals(FrameContext& ctx, RenderView& rv)
{
    if (rv.width <= 0 || rv.height <= 0 || !ctx.frame_buffer) return;

    FrameGlobals globals{};
    std::memcpy(globals.view_projection, rv.view_projection.m, sizeof(rv.view_projection.m));
    std::memcpy(globals.inverse_view_projection, rv.inverse_view_projection.m,
                sizeof(rv.inverse_view_projection.m));
    globals.viewport[0] = static_cast<float>(rv.width);
    globals.viewport[1] = static_cast<float>(rv.height);
    globals.viewport[2] = 1.0f / globals.viewport[0];
    globals.viewport[3] = 1.0f / globals.viewport[1];
    globals.cam_pos[0] = rv.cam_pos.x;
    globals.cam_pos[1] = rv.cam_pos.y;
    globals.cam_pos[2] = rv.cam_pos.z;
    globals.bvh_root = rv.bvh_root;
    globals.bvh_node_count = rv.bvh_node_count;
    globals.bvh_shape_count = rv.bvh_shape_count;
    globals.bvh_nodes_addr = rv.bvh_nodes_addr;
    globals.bvh_shapes_addr = rv.bvh_shapes_addr;
    globals.present_counter = static_cast<uint32_t>(ctx.present_counter);
    rv.view_globals_address = ctx.frame_buffer->write(&globals, sizeof(globals));
}

void ViewPreparer::prepare_lights(const SceneState& scene_state, FrameContext& ctx,
                                  RenderView& rv)
{
    struct LightCollect {
        FrameContext& ctx;
        vector<GpuLight>& out;
    };
    LightCollect lc{ctx, rv.lights};
    enumerate_scene_lights(scene_state,
        +[](void* u, LightSite& site) {
            auto& s = *static_cast<LightCollect*>(u);
            if (auto tech = find_shadow_technique(site.light)) {
                site.base.flags[1] = s.ctx.snippets
                    ? s.ctx.snippets->register_shadow_tech(tech.get(), *s.ctx.render_ctx)
                    : 0;
            }
            s.out.push_back(site.base);
        }, &lc);
}

void ViewPreparer::prepare_shapes(const SceneState& scene_state, FrameContext& ctx,
                                  RenderView& rv)
{
    struct ShapeCollect {
        FrameContext& ctx;
        vector<RtShape>& shapes;
    };
    ShapeCollect sc{ctx, rv.shapes};
    enumerate_scene_shapes(scene_state, ctx.render_ctx,
        +[](void* u, ShapeSite& site) {
            auto& s = *static_cast<ShapeCollect*>(u);
            auto& ctx = s.ctx;
            if (!ctx.snippets || !ctx.resources) return;
            auto resolve_ctx = ctx.make_resolve_context();
            auto mat = site.paint
                ? ctx.snippets->resolve_material(site.paint, resolve_ctx)
                : IFrameSnippetRegistry::MaterialRef{};
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
            if (site.has_mesh_data && ctx.frame_buffer) {
                if (auto* dd = interface_cast<IDrawData>(site.mesh_primitive)) {
                    site.mesh_instance.mesh_static_addr =
                        ctx.snippets->resolve_data_buffer(dd, resolve_ctx);
                }
                site.geometry.mesh_data_addr = ctx.frame_buffer->write(
                    &site.mesh_instance, sizeof(site.mesh_instance));
            }
            s.shapes.push_back(site.geometry);
        }, &sc);
}

void ViewPreparer::prepare_env(ViewEntry& entry,
                               const IElement::Ptr& camera_element,
                               FrameContext& ctx, RenderView& rv)
{
    auto camera = ::velk::find_attachment<ICamera>(camera_element);
    if (!camera) return;

    auto resolved = ensure_env_ready(*camera, ctx);
    if (!resolved.env) return;

    rv.env.texture_id = resolved.texture_id;
    auto env_mat = resolved.env->get_material();
    if (!env_mat) return;

    auto env_prog = interface_pointer_cast<IProgram>(env_mat);
    if (ctx.snippets) {
        auto env_ref =
            ctx.snippets->resolve_material(env_prog.get(), ctx.make_resolve_context());
        rv.env.material_id = env_ref.mat_id;
        rv.env.data_addr = env_ref.mat_addr;
    }
    // Fallback for env materials without a snippet: serialise into the
    // frame scratch buffer. Single upload per frame.
    if (rv.env.data_addr == 0 && ctx.frame_buffer) {
        if (auto* dd = interface_cast<IDrawData>(env_prog.get())) {
            size_t sz = dd->get_draw_data_size();
            if (sz > 0) {
                void* scratch = std::malloc(sz);
                if (scratch) {
                    std::memset(scratch, 0, sz);
                    if (dd->write_draw_data(scratch, sz) == ReturnValue::Success) {
                        rv.env.data_addr = ctx.frame_buffer->write(scratch, sz);
                    }
                    std::free(scratch);
                }
            }
        }
    }

    // Forward-only env Batch (fullscreen quad with env material).
    // Built here so the forward path doesn't need to reach back into
    // ICamera / IEnvironment. Deferred and RT ignore this batch.
    // Cached per-view: only rebuilt when the env material changes;
    // otherwise we reuse the same IBatch::Ptr across frames so the
    // hive doesn't allocate one per frame for every environment-bearing
    // camera.
    if (resolved.surface && env_mat && ctx.render_ctx) {
        auto& cache = view_caches_[&entry];
        const void* mat_key = static_cast<const void*>(env_mat.get());
        if (!cache.env_batch || cache.env_material_key != mat_key) {
            auto env_batch_ptr = ::velk::instance().create<IBatch>(ClassId::DefaultBatch);
            if (env_batch_ptr) {
                auto* env_batch = static_cast<impl::DefaultBatch*>(env_batch_ptr.get());
                env_batch->set_pipeline_key(0);
                env_batch->set_texture_key(reinterpret_cast<uint64_t>(resolved.surface));
                env_batch->set_instance_stride(4);
                env_batch->set_instance_count(1);
                env_batch->mutable_instance_data().resize(4, 0);
                env_batch->set_material(env_mat);
                if (auto quad = ctx.render_ctx->get_mesh_builder().get_unit_quad()) {
                    auto prims = quad->get_primitives();
                    if (prims.size() > 0) {
                        env_batch->set_primitive(prims[0]);
                        // Lazy compile in build_draw_calls reads
                        // pipeline_options off the batch — set topology
                        // to match the quad and pull cull/blend/depth
                        // from the env material's options. Defaults
                        // (TriangleList, alpha blend) would mis-compile
                        // the strip-quad and draw only one triangle.
                        PipelineOptions po = pipeline_options_from_storage(
                            interface_cast<IObjectStorage>(env_mat.get()));
                        po.topology = to_backend_topology(prims[0]->get_topology());
                        env_batch->set_pipeline_options(po);
                    }
                }
                cache.env_batch = std::move(env_batch_ptr);
                cache.env_material_key = mat_key;
            }
        }
        // Refresh the per-view texture key in case the surface changed
        // but the material identity didn't (rare, but cheap). The
        // env_batch falls through to the per-frame staging fallback
        // in emit_draw_calls (no pool slice) — it's a single fullscreen
        // quad, the per-frame staging cost is trivial, and it sidesteps
        // the question of where env_batch lives across pool resets.
        if (cache.env_batch) {
            auto* env_batch = static_cast<impl::DefaultBatch*>(cache.env_batch.get());
            env_batch->set_texture_key(reinterpret_cast<uint64_t>(resolved.surface));
        }
        rv.env_batch = cache.env_batch;
    }
}

RenderView ViewPreparer::prepare(ViewEntry& entry,
                                 const IElement::Ptr& camera_element,
                                 const SceneState& scene_state,
                                 FrameContext& ctx,
                                 BatchBuilder& batch_builder,
                                 const IRenderPath::Needs& needs)
{
    VELK_PERF_SCOPE("renderer.view_prepare");
    RenderView rv;

    // Always-on: viewport / camera / BVH addrs / frame globals upload /
    // env. Cheap and used by every path.
    prepare_camera(entry, camera_element, ctx, rv);
    rv.bvh_nodes_addr = ctx.bvh_nodes_addr;
    rv.bvh_shapes_addr = ctx.bvh_shapes_addr;
    rv.bvh_root = ctx.bvh_root;
    rv.bvh_node_count = ctx.bvh_node_count;
    rv.bvh_shape_count = ctx.bvh_shape_count;
    prepare_frame_globals(ctx, rv);
    prepare_env(entry, camera_element, ctx, rv);

    // Path-gated: skip the heavy scene walks when the path doesn't
    // declare a need for them.
    if (needs.batches) prepare_batches(entry, scene_state, batch_builder, ctx, rv);
    if (needs.lights)  prepare_lights(scene_state, ctx, rv);
    if (needs.shapes)  prepare_shapes(scene_state, ctx, rv);

    return rv;
}

void ViewPreparer::on_view_removed(ViewEntry& entry)
{
    view_caches_.erase(&entry);
}

void ViewPreparer::clear()
{
    view_caches_.clear();
}

} // namespace velk
