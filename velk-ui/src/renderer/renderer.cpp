#include "renderer.h"

#include "default_ui_shaders.h"
#include "scene_bvh.h"
#include "scene_collector.h"

#include <velk/api/any.h>
#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_object_storage.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-ui/interface/intf_environment.h>
#include <velk-ui/interface/intf_visual.h>

#ifdef VELK_RENDER_DEBUG
#define RENDER_LOG(...) VELK_LOG(I, __VA_ARGS__)
#else
#define RENDER_LOG(...) ((void)0)
#endif

namespace velk::ui {

constexpr string_view velk_ui_glsl = R"(
// Minimum viable per-instance record. Carries just the element's world
// transform; any visual type that fits this schema (no local rect, no
// extra per-instance fields) can use ElementInstanceData directly.
struct ElementInstance {
    mat4 world_matrix;
};

// Per-instance data for quad-shaped visuals. `world_matrix` is the
// element's world transform (filled in by the batch builder). (pos,
// size) is the rect's local-space footprint; vertex shaders compute
// gl_Position = vp * world_matrix * vec4(pos + q*size, 0, 1).
struct RectInstance {
    mat4 world_matrix;
    vec2 pos;
    vec2 size;
    vec4 color;
};

struct TextInstance {
    mat4 world_matrix;
    vec2 pos;
    vec2 size;
    vec4 color;
    uint glyph_index;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

layout(buffer_reference, std430) readonly buffer ElementInstanceData {
    ElementInstance data[];
};

layout(buffer_reference, std430) readonly buffer RectInstanceData {
    RectInstance data[];
};

layout(buffer_reference, std430) readonly buffer TextInstanceData {
    TextInstance data[];
};

// ===== Material evaluation types =====
// Shared across raster (forward / deferred fragment drivers) and RT
// (compute shader `velk_resolve_fill` dispatch). Every material
// provides a `velk_eval_<name>(EvalContext) -> MaterialEval` function;
// per-path drivers convert the MaterialEval into the output shape
// required by that path (frag_color / G-buffer attachments / BrdfSample).

const uint VELK_LIGHTING_UNLIT   = 0u;  // Pass color straight through, no shading.
const uint VELK_LIGHTING_STANDARD = 1u; // Full PBR lighting via velk_pbr_shade.

// Everything a material eval function receives. Bundles instance + hit
// context so adding a new per-hit input stays cheap.
struct EvalContext {
    uint64_t data_addr;    // material's per-draw GPU data pointer
    uint texture_id;       // bindless texture slot (0 if unused)
    uint shape_param;      // per-shape material slot (e.g. glyph index)
    vec2 uv;               // hit / fragment uv (0..1 across the shape)
    vec4 base;             // shape base color (instance tint)
    vec3 ray_dir;          // incoming ray / view direction (0 if unavailable)
    vec3 normal;           // surface normal at hit (world space)
    vec3 hit_pos;          // world-space hit point (or frag world position)
};

// Canonical material output. Produced by velk_eval_<name> once per
// shading point and consumed by every path-specific driver.
struct MaterialEval {
    vec4 color;                   // base rgb + alpha (post factor*texture)
    vec3 normal;                  // world-space shading normal (default: ctx.normal)
    float metallic;               // 0..1
    float roughness;               // 0..1
    vec3 emissive;                // emissive contribution, added unlit
    float occlusion;              // 0..1, modulates ambient/diffuse
    vec3 specular_color_factor;   // KHR_materials_specular: dielectric F0 tint
    float specular_factor;        // KHR_materials_specular: Fresnel weight
    uint lighting_mode;           // VELK_LIGHTING_*
};

// Returns a MaterialEval prefilled with spec-correct defaults. Materials
// build on top of this so newly added fields get sensible values without
// every material needing to track them explicitly.
MaterialEval velk_default_material_eval() {
    MaterialEval e;
    e.color = vec4(1.0);
    e.normal = vec3(0.0, 0.0, 1.0);
    e.metallic = 0.0;
    e.roughness = 1.0;
    e.emissive = vec3(0.0);
    e.occlusion = 1.0;
    e.specular_color_factor = vec3(1.0);
    e.specular_factor = 1.0;
    e.lighting_mode = VELK_LIGHTING_UNLIT;
    return e;
}
)";

void Renderer::set_backend(const IRenderBackend::Ptr& backend, IRenderContext* ctx)
{
    backend_ = backend;
    render_ctx_ = ctx;

    if (!backend_) {
        return;
    }

    pipeline_map_ = &ctx->pipeline_map();

    // Register velk-ui shader include for UI instance types

    ctx->register_shader_include("velk-ui.glsl", velk_ui_glsl);

    // Register default shaders. Visuals and materials that do not provide
    // their own shader sources fall back to these (see compile_pipeline:
    // empty source -> registered default). Built-in UI pipelines are now
    // compiled lazily by batch_builder on first sight of a new visual type.
    ctx->set_default_vertex_shader(ctx->compile_shader(default_vertex_src, ShaderStage::Vertex));
    ctx->set_default_fragment_shader(ctx->compile_shader(default_fragment_src, ShaderStage::Fragment));

    // Deferred G-buffer default vertex. The default fragment isn't
    // pre-compiled: its standalone source declares a `velk_visual_discard`
    // forward decl whose definition only arrives when batch_builder's
    // composer appends the visual's discard snippet (or an empty stub).
    // The composer always supplies a complete fragment, so the default
    // fragment slot in IRenderContext stays unused.
    ctx->set_default_gbuffer_vertex_shader(
        ctx->compile_shader(default_gbuffer_vertex_src, ShaderStage::Vertex));

    frame_buffer_.init();
    for (auto& slot : frame_slots_) {
        frame_buffer_.init_slot(slot.buffer, *backend_);
    }
}

void Renderer::add_view(const IElement::Ptr& camera_element, const IWindowSurface::Ptr& surface,
                        const rect& viewport)
{
    if (!(camera_element && surface)) {
        VELK_LOG(E, "Renderer::add_view: camera_element and surface must be valid");
        return;
    }

    // Create the backend surface if this is the first view using it
    if (get_render_target_id(surface) == 0 && backend_) {
        auto state = read_state<IWindowSurface>(surface);
        if (state) {
            SurfaceDesc desc{};
            desc.width = state->size.x;
            desc.height = state->size.y;
            desc.update_rate = state->update_rate;
            desc.target_fps = state->target_fps;
            uint64_t sid = backend_->create_surface(desc);
            if (auto* rt = interface_cast<IRenderTarget>(surface)) {
                rt->set_render_target_id(sid);
            }
        }
    }
    views_.push_back({camera_element, surface, viewport});
}

void Renderer::remove_view(const IElement::Ptr& camera_element, const IWindowSurface::Ptr& surface)
{
    for (auto it = views_.begin(); it != views_.end(); ++it) {
        if (it->camera_element == camera_element && it->surface == surface) {
            if (backend_) {
                FrameContext ctx = make_frame_context();
                rasterizer_.on_view_removed(*it, ctx);
                ray_tracer_.on_view_removed(*it, ctx);
                deferred_lighter_.on_view_removed(*it, ctx);
                backend_->destroy_surface(get_render_target_id(it->surface));
            }
            views_.erase(it);
            return;
        }
    }
}

FrameContext Renderer::make_frame_context()
{
    FrameContext ctx{};
    ctx.backend = backend_.get();
    ctx.render_ctx = render_ctx_;
    ctx.frame_buffer = &frame_buffer_;
    ctx.resources = &resources_;
    ctx.batch_builder = &batch_builder_;
    ctx.snippets = &snippets_;
    ctx.pipeline_map = pipeline_map_;
    ctx.observer = this;
    ctx.present_counter = present_counter_;
    ctx.latency_frames = kGpuLatencyFrames;
    return ctx;
}

bool Renderer::view_matches(const ViewEntry& entry, const FrameDesc& desc) const
{
    if (desc.views.empty()) {
        return true;
    }
    for (auto& vd : desc.views) {
        if (vd.surface != entry.surface) {
            continue;
        }
        if (vd.cameras.empty()) {
            return true;
        }
        for (auto& cam : vd.cameras) {
            if (cam == entry.camera_element) {
                return true;
            }
        }
    }
    return false;
}

Renderer::FrameSlot* Renderer::claim_frame_slot()
{
    auto is_slot_free = [&](const FrameSlot& s) {
        return !s.ready && (s.presented_at == 0 || present_counter_ - s.presented_at >= kGpuLatencyFrames);
    };

    FrameSlot* slot = nullptr;
    {
        std::unique_lock<std::mutex> lock(slot_mutex_);
        slot_cv_.wait(lock, [&] {
            for (auto& s : frame_slots_) {
                if (is_slot_free(s)) {
                    return true;
                }
            }
            return false;
        });
        for (auto& s : frame_slots_) {
            if (is_slot_free(s)) {
                slot = &s;
                break;
            }
        }
    }

    slot->id = next_frame_id_++;
    slot->passes.clear();
    return slot;
}

std::unordered_map<IScene*, SceneState> Renderer::consume_scenes(const FrameDesc& desc)
{
    std::unordered_map<IScene*, SceneState> consumed;
    for (auto& entry : views_) {
        if (!view_matches(entry, desc)) {
            continue;
        }

        auto scene_ptr = entry.camera_element->get_scene();
        auto* scene = interface_cast<IScene>(scene_ptr);
        if (!scene || consumed.count(scene)) {
            continue;
        }

        // Handle surface resize
        auto sstate = read_state<IWindowSurface>(entry.surface);
        if (sstate) {
            if (sstate->size.x != entry.cached_width || sstate->size.y != entry.cached_height) {
                entry.cached_width = sstate->size.x;
                entry.cached_height = sstate->size.y;
                backend_->resize_surface(get_render_target_id(entry.surface), sstate->size.x, sstate->size.y);
                entry.batches_dirty = true;
                RENDER_LOG("render: surface resized to %dx%d", sstate->size.x, sstate->size.y);
            }
        }

        auto state = scene->consume_state();
        bool has_changes = !state.redraw_list.empty() || !state.removed_list.empty();

        // A change only warrants a batch rebuild if a visual-bearing
        // element actually changed. Non-visual changes (camera
        // transform, lights, etc.) legitimately land in redraw_list
        // but don't affect any rendered batch, so flagging views
        // dirty for them would force a full rebuild_batches every pan
        // frame.
        bool has_visual_changes = !state.removed_list.empty();
        if (!has_visual_changes) {
            for (auto* elem : state.redraw_list) {
                if (::velk::has_attachment<IVisual>(elem)) {
                    has_visual_changes = true;
                    break;
                }
            }
        }

        // Evict removed elements
        {
            FrameContext ctx = make_frame_context();
            for (auto& removed : state.removed_list) {
                batch_builder_.evict(removed.get());
                rasterizer_.on_element_removed(removed.get(), ctx);
                ray_tracer_.on_element_removed(removed.get(), ctx);
            }
        }

        // Rebuild draw commands for changed elements
        for (auto* element : state.redraw_list) {
            batch_builder_.rebuild_commands(element, this, render_ctx_);
        }

        // Upload dirty GPU resources
        bool resources_uploaded = false;
        if (has_changes) {
            for (auto& [elem, cache] : batch_builder_.element_cache()) {
                for (auto& weak : cache.gpu_resources) {
                    auto buf_ptr = weak.lock();
                    auto* buf = buf_ptr.get();
                    if (!buf || !buf->is_dirty()) {
                        continue;
                    }

                    if (auto* surf = interface_cast<ISurface>(buf)) {
                        auto sz = surf->get_dimensions();
                        int tw = static_cast<int>(sz.x);
                        int th = static_cast<int>(sz.y);
                        const uint8_t* pixels = buf->get_data();
                        if (pixels && tw > 0 && th > 0) {
                            TextureId tid = resources_.find_texture(surf);
                            if (tid == 0) {
                                TextureDesc tdesc{};
                                tdesc.width = tw;
                                tdesc.height = th;
                                tdesc.format = surf->format();
                                tid = backend_->create_texture(tdesc);
                                resources_.register_texture(surf, tid);
                            }
                            backend_->upload_texture(tid, pixels, tw, th);
                            buf->clear_dirty();
                            resources_uploaded = true;
                        }
                    } else {
                        size_t bsize = buf->get_data_size();
                        const uint8_t* bytes = buf->get_data();
                        if (!bytes || bsize == 0) {
                            continue;
                        }
                        auto* be = resources_.find_buffer(buf);
                        bool need_alloc = (be == nullptr);
                        if (!need_alloc && be->size != bsize) {
                            resources_.defer_buffer_destroy(be->handle, present_counter_ + kGpuLatencyFrames);
                            resources_.unregister_buffer(buf);
                            be = nullptr;
                            need_alloc = true;
                        }
                        if (need_alloc) {
                            GpuBufferDesc bdesc{};
                            bdesc.size = bsize;
                            bdesc.cpu_writable = true;
                            GpuResourceManager::BufferEntry bentry{};
                            bentry.handle = backend_->create_buffer(bdesc);
                            bentry.size = bsize;
                            resources_.register_buffer(buf, bentry);
                            be = resources_.find_buffer(buf);
                            buf->set_gpu_address(backend_->gpu_address(bentry.handle));
                        }
                        if (auto* dst = backend_->map(be->handle)) {
                            std::memcpy(dst, bytes, bsize);
                        }
                        buf->clear_dirty();
                        resources_uploaded = true;
                    }
                }
            }
        }

        if (has_visual_changes || resources_uploaded) {
            for (auto& v : views_) {
                auto sp = v.camera_element->get_scene();
                if (interface_cast<IScene>(sp) == scene) {
                    v.batches_dirty = true;
                }
            }
        }

        consumed[scene] = std::move(state);
    }
    return consumed;
}

void Renderer::build_frame_passes(const FrameDesc& desc,
                                  std::unordered_map<IScene*, SceneState>& consumed_scenes, FrameSlot& slot)
{
    static constexpr int kMaxRecordRetries = 3;
    for (int attempt = 0;; ++attempt) {
        frame_buffer_.begin_frame(slot.buffer);
        slot.passes.clear();

        FrameContext ctx = make_frame_context();

        // Scene-wide BVH. Each scene has a SceneBvh attachment on its
        // root (installed lazily on first frame); every view on that
        // scene reads addresses from the same attachment. The shape
        // callback resolves material + texture + custom intersect via
        // the shared snippet registry so primary-buffer shapes and
        // BVH shapes carry identical ids.
        //
        // M2: rebuild runs every frame; M3 will skip it when nothing
        // dirty has changed.
        std::unordered_map<IScene*, impl::SceneBvh*> scene_bvhs;
        for (auto& [scene, state] : consumed_scenes) {
            impl::SceneBvh* bvh = nullptr;
            auto cit = scene_bvh_cache_.find(scene);
            if (cit != scene_bvh_cache_.end()) {
                bvh = cit->second;
            } else {
                // First time we see this scene. Create + attach a
                // SceneBvh on the scene root, then cache the typed
                // pointer here so future frames skip the interface
                // round-trip.
                auto* root = interface_cast<IObjectStorage>(scene->root().get());
                if (!root) {
                    continue;
                }
                auto new_bvh_obj = ::velk::ext::make_object<impl::SceneBvh>();
                // The make_object signature returns IObject::Ptr whose
                // raw bytes alias the concrete SceneBvh allocation (see
                // ext::make_object). The void*-roundtrip mirrors what
                // make_object itself used to construct the pointer.
                bvh = static_cast<impl::SceneBvh*>(
                    static_cast<void*>(new_bvh_obj.get()));
                root->add_attachment(new_bvh_obj);
                scene_bvh_cache_[scene] = bvh;
            }

            // Dirty if the change affects scene geometry: an element
            // was removed, or a redrawn element carries an IVisual
            // attachment (i.e. its geometry/shape set may have moved
            // or changed). Pure camera / light transform updates land
            // in redraw_list too but don't affect BVH topology, so we
            // skip a rebuild for them. Parent transform changes are
            // still caught because the layout solver propagates the
            // dirty flag into all descendant visuals.
            bool dirty = !state.removed_list.empty();
            if (!dirty) {
                for (auto* elem : state.redraw_list) {
                    if (::velk::has_attachment<IVisual>(elem)) {
                        dirty = true;
                        break;
                    }
                }
            }
            bvh->rebuild(scene, frame_buffer_, dirty, [&](ShapeSite& site) {
                auto mat = site.paint
                    ? snippets_.resolve_material(site.paint, ctx)
                    : FrameSnippetRegistry::MaterialRef{};
                uint32_t tex_id = 0;
                if (site.draw_entry && site.draw_entry->texture_key != 0) {
                    auto* surf = reinterpret_cast<ISurface*>(
                        static_cast<uintptr_t>(site.draw_entry->texture_key));
                    tex_id = resources_.find_texture(surf);
                    if (tex_id == 0) {
                        uint64_t rt_id = get_render_target_id(surf);
                        if (rt_id != 0) tex_id = static_cast<uint32_t>(rt_id);
                    }
                }
                site.geometry.material_id = mat.mat_id;
                site.geometry.material_data_addr = mat.mat_addr;
                site.geometry.texture_id = tex_id;

                if (auto* analytic = interface_cast<IAnalyticShape>(site.visual)) {
                    uint32_t kind = snippets_.register_intersect(analytic, *render_ctx_);
                    if (kind != 0) site.geometry.shape_kind = kind;
                }
            });
            scene_bvhs[scene] = bvh;
        }

        // Per-view passes go into a temp so the shared passes (RTT, etc.)
        // can be emitted first into slot.passes; shared must render before
        // any view pass that samples their output.
        vector<RenderPass> view_passes;
        for (auto& entry : views_) {
            if (!view_matches(entry, desc)) {
                continue;
            }

            auto scene_ptr = entry.camera_element->get_scene();
            auto* scene = interface_cast<IScene>(scene_ptr);
            if (!scene) {
                continue;
            }

            auto sit = consumed_scenes.find(scene);
            if (sit == consumed_scenes.end()) {
                continue;
            }

            auto bit = scene_bvhs.find(scene);
            if (bit != scene_bvhs.end() && bit->second) {
                auto* b = bit->second;
                ctx.bvh_nodes_addr = b->nodes_addr();
                ctx.bvh_shapes_addr = b->shapes_addr();
                ctx.bvh_root = b->get_root_index();
                ctx.bvh_node_count = b->get_node_count();
                ctx.bvh_shape_count = b->get_shape_count();
            } else {
                ctx.bvh_nodes_addr = 0;
                ctx.bvh_shapes_addr = 0;
                ctx.bvh_root = 0;
                ctx.bvh_node_count = 0;
                ctx.bvh_shape_count = 0;
            }

            // Pick the sub-renderer based on the camera's render_path.
            auto camera = ::velk::find_attachment<ICamera>(entry.camera_element);
            RenderPath render_path = RenderPath::Forward;
            if (camera) {
                auto cam_state = read_state<ICamera>(camera);
                if (cam_state) {
                    render_path = cam_state->render_path;
                }
            }

            if (render_path == RenderPath::RayTrace) {
                ray_tracer_.build_passes(entry, sit->second, ctx, view_passes);
            } else {
                rasterizer_.build_passes(entry, sit->second, ctx, view_passes);
                // Deferred lighting consumes the G-buffer filled by the
                // rasterizer. Must appear after in the pass list so the
                // G-buffer attachments have been written + transitioned
                // to SHADER_READ_ONLY_OPTIMAL before the compute samples.
                deferred_lighter_.build_passes(entry, sit->second, ctx, view_passes);
            }
        }

        rasterizer_.build_shared_passes(ctx, slot.passes);
        ray_tracer_.build_shared_passes(ctx, slot.passes);
        for (auto& p : view_passes) {
            slot.passes.push_back(std::move(p));
        }

        // Debug overlays: tail-appended so they blit on top of whatever
        // the view passes produced on the target surface.
        for (auto& ov : debug_overlays_) {
            if (!ov.surface || ov.texture_id == 0) continue;
            RenderPass op;
            op.kind = PassKind::Blit;
            op.blit_source = ov.texture_id;
            op.blit_surface_id = ov.surface->get_render_target_id();
            op.blit_dst_rect = ov.dst_rect;
            slot.passes.push_back(std::move(op));
        }

        if (!frame_buffer_.overflowed()) {
            break;
        }
        if (attempt >= kMaxRecordRetries) {
            VELK_LOG(
                E, "Renderer: frame buffer overflow after %d retries, dropping frame", kMaxRecordRetries);
            break;
        }
        frame_buffer_.grow(*backend_);
    } // retry loop
}

Frame Renderer::prepare(const FrameDesc& desc)
{
    if (!backend_) {
        return {};
    }
    VELK_PERF_EVENT(Render);
    VELK_PERF_SCOPE("renderer.prepare");

    // Drain any GPU resources whose safe window has elapsed.
    resources_.drain_deferred(*backend_, present_counter_);

    auto* slot = claim_frame_slot();
    active_slot_ = slot;

    // If this slot's buffer is undersized (it was in-flight during a
    // previous regrow and was skipped), grow it now that it's safe.
    frame_buffer_.ensure_slot(slot->buffer, *backend_);

    // Prepare the staging buffer once for all views
    frame_buffer_.ensure_capacity(*backend_);

    if ((slot->id % 10000) == 0) {
        VELK_LOG(I,
                 "Renderer: frame %llu, frame buffer %zu KB, peak usage %zu KB",
                 static_cast<unsigned long long>(slot->id),
                 frame_buffer_.get_buffer_size() / 1024,
                 frame_buffer_.get_peak_usage() / 1024);
    }

    auto consumed_scenes = consume_scenes(desc);

    batch_builder_.reset_frame_state();
    snippets_.begin_frame();
    build_frame_passes(desc, consumed_scenes, *slot);

    active_slot_ = nullptr;
    slot->ready = true;
    return Frame{slot->id};
}

void Renderer::present(Frame frame)
{
    if (!backend_ || frame.id == 0) {
        VELK_PERF_EVENT(Present);
        return;
    }
    {
        VELK_PERF_SCOPE("renderer.present");

        std::lock_guard<std::mutex> lock(slot_mutex_);

        // Find the frame being presented to determine which surfaces it targets
        FrameSlot* target = nullptr;
        for (auto& s : frame_slots_) {
            if (s.ready && s.id == frame.id) {
                target = &s;
                break;
            }
        }
        if (!target) {
            slot_cv_.notify_one();
            return;
        }

        // Discard older unpresented frames that target overlapping surfaces
        for (auto& s : frame_slots_) {
            if (!s.ready || s.id >= frame.id) {
                continue;
            }
            // Remove passes that overlap with the frame being presented
            for (auto it = s.passes.begin(); it != s.passes.end();) {
                bool overlaps = false;
                uint64_t it_id = get_render_target_id(it->target.target);
                for (auto& tp : target->passes) {
                    uint64_t tp_id = get_render_target_id(tp.target.target);
                    if (it_id == tp_id) {
                        overlaps = true;
                        break;
                    }
                }
                if (overlaps) {
                    it = s.passes.erase(it);
                } else {
                    ++it;
                }
            }
            // If all passes were removed, mark slot as not ready but preserve presented_at
            // so the buffer isn't reused before the GPU is done with it
            if (s.passes.empty()) {
                s.ready = false;
                s.presented_at = present_counter_ + 1;
            }
        }

        // Submit all passes within a single frame
        {
            VELK_PERF_SCOPE("renderer.wait_vsync");
            backend_->begin_frame();
        }
        bool had_texture_pass = false;
        for (size_t i = 0; i < target->passes.size(); ++i) {
            auto& pass = target->passes[i];

            if (pass.kind == PassKind::ComputeBlit) {
                // Ray-traced view or deferred lighting: dispatch compute
                // then blit output texture onto the swapchain image.
                // Barrier target is ComputeShader because this pass
                // reads prior texture writes from compute, not fragment.
                if (had_texture_pass) {
                    backend_->barrier(PipelineStage::ColorOutput, PipelineStage::ComputeShader);
                    had_texture_pass = false;
                }
                backend_->dispatch({&pass.compute, 1});
                backend_->blit_to_surface(pass.blit_source, pass.blit_surface_id, pass.blit_dst_rect);
                continue;
            }

            if (pass.kind == PassKind::Blit) {
                // Bare blit from a texture (e.g. a G-buffer attachment)
                // to a surface subrect. Used for debug overlays on top
                // of whatever was already drawn/blitted to the surface.
                if (had_texture_pass) {
                    backend_->barrier(PipelineStage::ColorOutput, PipelineStage::ComputeShader);
                    had_texture_pass = false;
                }
                backend_->blit_to_surface(pass.blit_source, pass.blit_surface_id, pass.blit_dst_rect);
                continue;
            }

            if (pass.kind == PassKind::Compute) {
                // Pure compute dispatch (e.g. deferred lighting). Output
                // consumed by a later pass via sampled image / storage;
                // no surface blit here.
                if (had_texture_pass) {
                    backend_->barrier(PipelineStage::ColorOutput, PipelineStage::ComputeShader);
                    had_texture_pass = false;
                }
                backend_->dispatch({&pass.compute, 1});
                continue;
            }

            if (pass.kind == PassKind::GBufferFill) {
                // Deferred G-buffer fill: raster-draw into an MRT group.
                // Produces no surface output on its own; the compute
                // lighting pass samples these attachments later.
                if (had_texture_pass) {
                    backend_->barrier(PipelineStage::ColorOutput, PipelineStage::FragmentShader);
                    had_texture_pass = false;
                }
                backend_->begin_pass(pass.gbuffer_group);
                backend_->submit({pass.draw_calls.data(), pass.draw_calls.size()}, pass.viewport);
                backend_->end_pass();
                // Attachments transition to SHADER_READ_ONLY_OPTIMAL via
                // the group's render pass finalLayout; a sampling pass
                // can read them immediately once a pipeline barrier is
                // inserted (done by the lighting pass in B.3).
                had_texture_pass = true;
                continue;
            }

            auto* rt = pass.target.target.get();
            uint64_t pass_target_id = rt ? rt->get_render_target_id() : 0;
            bool is_texture = rt && rt->get_type() == GpuResourceType::Texture;

            // Insert barrier when transitioning from texture passes to surface pass
            if (!is_texture && had_texture_pass) {
                backend_->barrier(PipelineStage::ColorOutput, PipelineStage::FragmentShader);
                had_texture_pass = false;
            }

            backend_->begin_pass(pass_target_id);

            RENDER_LOG(
                "present: submitting %zu draw calls to target %llu", pass.draw_calls.size(), pass_target_id);
            {
                VELK_PERF_SCOPE("renderer.submit");
                backend_->submit({pass.draw_calls.data(), pass.draw_calls.size()}, pass.viewport);
            }

            backend_->end_pass();

            if (is_texture) {
                had_texture_pass = true;
            }
        }
        {
            VELK_PERF_SCOPE("renderer.end_frame");
            backend_->end_frame();
        }
        present_counter_++;
        target->ready = false;
        target->presented_at = present_counter_;
        target->passes.clear();

        slot_cv_.notify_one();
    }
    VELK_PERF_EVENT(Present);
}

void Renderer::render()
{
    present(prepare({}));
}

void Renderer::set_max_frames_in_flight(uint32_t count)
{
    if (count < 1) {
        count = 1;
    }
    std::lock_guard<std::mutex> lock(slot_mutex_);
    auto old_size = frame_slots_.size();
    frame_slots_.resize(count);
    for (auto i = old_size; i < count; ++i) {
        frame_buffer_.init_slot(frame_slots_[i].buffer, *backend_);
    }
    slot_cv_.notify_all();
}

void Renderer::add_debug_overlay(const IWindowSurface::Ptr& surface,
                                  TextureId texture_id,
                                  const rect& dst_rect)
{
    debug_overlays_.push_back({surface, texture_id, dst_rect});
}

void Renderer::clear_debug_overlays()
{
    debug_overlays_.clear();
}

TextureId Renderer::get_gbuffer_attachment(const IElement::Ptr& camera_element,
                                            const IWindowSurface::Ptr& surface,
                                            uint32_t attachment_index) const
{
    for (auto& v : views_) {
        if (v.camera_element.get() != camera_element.get()) continue;
        if (v.surface.get() != surface.get()) continue;
        if (v.gbuffer_group == 0 || !backend_) return 0;
        return backend_->get_render_target_group_attachment(v.gbuffer_group, attachment_index);
    }
    return 0;
}

void Renderer::on_gpu_resource_destroyed(IGpuResource* resource)
{
    resources_.on_resource_destroyed(resource, present_counter_, kGpuLatencyFrames);
}

void Renderer::shutdown()
{
    if (backend_) {
        // Detach from any GPU resources we are still observing so their
        // dtors (which may run later, on any thread) cannot reach a dead
        // renderer.
        for (auto& [elem, cache] : batch_builder_.element_cache()) {
            for (auto& weak : cache.gpu_resources) {
                if (auto res = weak.lock()) {
                    res->remove_gpu_resource_observer(this);
                }
            }
        }
        // Unregister from environment textures (not tracked via element cache).
        resources_.unregister_env_observers(this);

        resources_.shutdown(*backend_);

        // Per-sub-renderer cleanup (RTT textures, RT storage textures, etc.).
        {
            FrameContext ctx = make_frame_context();
            for (auto& entry : views_) {
                rasterizer_.on_view_removed(entry, ctx);
                ray_tracer_.on_view_removed(entry, ctx);
                deferred_lighter_.on_view_removed(entry, ctx);
            }
            rasterizer_.shutdown(ctx);
            ray_tracer_.shutdown(ctx);
            deferred_lighter_.shutdown(ctx);
        }

        for (auto& entry : views_) {
            backend_->destroy_surface(get_render_target_id(entry.surface));
        }

        for (auto& slot : frame_slots_) {
            if (slot.buffer.handle) {
                backend_->destroy_buffer(slot.buffer.handle);
                slot.buffer.handle = 0;
            }
        }

        backend_ = nullptr;
    }

    views_.clear();
    batch_builder_.clear();
    pipeline_map_ = nullptr;
}


} // namespace velk::ui
