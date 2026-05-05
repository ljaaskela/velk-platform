#include "renderer.h"

#include "scene_bvh.h"
#include "scene_collector.h"

#include <velk-render/frame/raster_shaders.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/plugin.h>

#include <velk/api/any.h>
#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_object_storage.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-scene/interface/intf_environment.h>
#include <velk-scene/interface/intf_visual.h>

#ifdef VELK_RENDER_DEBUG
#define RENDER_LOG(...) VELK_LOG(I, __VA_ARGS__)
#else
#define RENDER_LOG(...) ((void)0)
#endif

namespace velk {

constexpr string_view velk_ui_glsl = R"(
// Universal per-instance record shared by every visual (rect,
// rounded rect, text glyphs, textures, images, env, cube, sphere,
// meshes, and future glTF primitives).
//
//   world_matrix — element transform, filled by batch_builder.
//   offset.xyz   — local offset (glyph pos for text; 0 otherwise).
//   size.xyz     — extents (size.z = 0 for 2D visuals).
//   color        — visual tint.
//   params.x     — shape_param (glyph index / material slot);
//                  yzw reserved.
//
// Vertex shaders compute:
//   gl_Position = vp * world_matrix * vec4(offset + v.position * size, 1);
struct ElementInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(buffer_reference, std430) readonly buffer ElementInstanceData {
    ElementInstance data[];
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
// context so adding a new per-hit input stays cheap. Evals that need
// view-level state (camera position, viewport, BVH, present_counter)
// read directly from the `view_globals` UBO declared in velk.glsl.
struct EvalContext {
    uint64_t data_addr;    // material's per-draw GPU data pointer
    uint texture_id;       // bindless texture slot (0 if unused)
    uint shape_param;      // per-shape material slot (e.g. glyph index)
    vec2 uv;               // hit / fragment uv (TEXCOORD_0, 0..1 across the shape)
    vec2 uv1;              // TEXCOORD_1; equals `uv` when the primitive has no second UV set or the shading point is on an analytic RT shape
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

Renderer::~Renderer()
{
    Renderer::shutdown();
}

uint64_t Renderer::consume_last_prepare_gpu_wait_ns()
{
    uint64_t v = last_prepare_gpu_wait_ns_;
    last_prepare_gpu_wait_ns_ = 0;
    return v;
}

void Renderer::set_backend(const IRenderBackend::Ptr& backend, IRenderContext* ctx)
{
    backend_ = backend;
    render_ctx_ = ctx;

    if (!backend_) {
        return;
    }

    pipeline_map_ = &ctx->pipeline_map();

    diag_enabled_ = false;

    // Instantiate the per-renderer plumbing through the type registry so
    // allocation participates in the hive. Held via interface Ptr; raw
    // typed pointers cache the concrete cast for slot/management methods
    // not on the interface (FrameDataManager) and for stable raw access.
    resources_ = instance().create<IGpuResourceManager>(ClassId::GpuResourceManager);
    if (auto internal = interface_cast<IGpuResourceManagerInternal>(resources_)) {
        internal->init(backend_.get());
    }
    snippets_ = instance().create<IFrameSnippetRegistry>(ClassId::FrameSnippetRegistry);

    frame_buffer_ = instance().create<IFrameDataManager>(ClassId::FrameDataManager);

    // Register velk-ui shader include for UI instance types

    ctx->register_shader_include("velk-ui.glsl", velk_ui_glsl);

    // Register default shaders. Visuals and materials that do not provide
    // their own shader sources fall back to these (see compile_pipeline:
    // empty source -> registered default). Built-in UI pipelines are now
    // compiled lazily by batch_builder on first sight of a new visual type.
    ctx->set_default_vertex_shader(ctx->compile_shader(default_vertex_src, ShaderStage::Vertex));
    ctx->set_default_fragment_shader(ctx->compile_shader(default_fragment_src, ShaderStage::Fragment));

    frame_buffer_->init();
    for (auto& slot : frame_slots_) {
        frame_buffer_->init_slot(slot.buffer, *backend_, *resources_);
    }

    // One-shot upload of every context-owned default buffer. Draws
    // whose primitive does not supply a given optional vertex stream
    // point their DrawData slot at the corresponding default so the
    // shader can always dereference it safely.
    auto upload_default = [&](IBuffer::Ptr buf_ptr) {
        auto* buf = buf_ptr.get();
        if (!buf) return;
        size_t bsize = buf->get_data_size();
        const uint8_t* bytes = buf->get_data();
        if (!bytes || bsize == 0 || resources_->find_buffer(buf)) return;
        GpuBufferDesc bdesc{};
        bdesc.size = bsize;
        bdesc.cpu_writable = true;
        auto* be = resources_->ensure_buffer_storage(buf, bdesc);
        if (!be) return;
        if (auto* dst = backend_->map(be->handle)) {
            std::memcpy(dst, bytes, bsize);
        }
        buf->clear_dirty();
    };
    upload_default(render_ctx_->get_default_buffer(DefaultBufferType::Uv1));
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
            if (auto* rt = interface_cast<IRenderTarget>(surface)) {
                desc.depth = rt->get_depth_format();
            }
            uint64_t sid = backend_->create_surface(desc);
            if (auto* rt = interface_cast<IRenderTarget>(surface)) {
                rt->set_gpu_handle(GpuResourceKey::Default, sid);
            }
        }
    }
    views_.push_back(ViewSlot{camera_element, ViewEntry{surface, viewport}});
}

void Renderer::remove_view(const IElement::Ptr& camera_element, const IWindowSurface::Ptr& surface)
{
    for (auto it = views_.begin(); it != views_.end(); ++it) {
        if (it->camera_element == camera_element && it->entry.surface == surface) {
            if (backend_) {
                FrameContext ctx = make_frame_context();
                auto cam_trait = ::velk::find_attachment<ICamera>(it->camera_element);
                ctx.view_camera_trait = cam_trait.get();
                for (auto* pipeline : seen_pipelines_) {
                    pipeline->on_view_removed(it->entry, ctx);
                }
                ctx.view_camera_trait = nullptr;
                view_preparer_.on_view_removed(it->entry);
                backend_->destroy_surface(get_render_target_id(it->entry.surface));
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
    ctx.frame_buffer = frame_buffer_.get();
    ctx.resources = resources_.get();
    ctx.snippets = snippets_.get();
    ctx.material_cache = &material_cache_;
    ctx.pipeline_map = pipeline_map_;
    ctx.defer_marker = backend_ ? backend_->pending_frame_completion_marker() : 0;
    ctx.present_counter = present_counter_;
    // ctx.target_format is set per-camera by IViewPipeline::emit before
    // the path runs; the renderer-level FrameContext leaves it at the
    // FrameContext default (Surface). Pre-emit consumers (snippet
    // resolution etc.) don't compile pipelines so the field is unused
    // there.
    return ctx;
}

bool Renderer::view_matches(const ViewSlot& slot, const FrameDesc& desc) const
{
    if (desc.views.empty()) {
        return true;
    }
    for (auto& vd : desc.views) {
        if (vd.surface != slot.entry.surface) {
            continue;
        }
        if (vd.cameras.empty()) {
            return true;
        }
        for (auto& cam : vd.cameras) {
            if (cam == slot.camera_element) {
                return true;
            }
        }
    }
    return false;
}

Renderer::FrameSlot* Renderer::claim_frame_slot()
{
    auto is_slot_free = [&](const FrameSlot& s) {
        return !s.ready;
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

    // Block until the GPU has actually finished this slot's previous
    // use. Replaces the prior CPU-counter heuristic that broke at
    // unlimited frame rates (CPU outran GPU and trampled live buffers).
    // The wait can be substantial under vsync; time it so the perf
    // overlay can charge it as GPU wait, not CPU work.
    if (backend_ && slot->gpu_completion_marker != 0) {
        auto wait_start = std::chrono::steady_clock::now();
        backend_->wait_for_frame_completion(slot->gpu_completion_marker);
        auto wait_end = std::chrono::steady_clock::now();
        last_prepare_gpu_wait_ns_ += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                wait_end - wait_start).count());
    }

    slot->id = next_frame_id_++;
    if (!slot->graph) {
        slot->graph = instance().create<IRenderGraph>(ClassId::RenderGraph);
        if (auto internal = interface_cast<IRenderGraphInternal>(slot->graph)) {
            internal->init(backend_.get());
        }
    }
    slot->graph->clear();
    return slot;
}

std::unordered_map<IScene*, SceneState> Renderer::consume_scenes(const FrameDesc& desc)
{
    std::unordered_map<IScene*, SceneState> consumed;
    for (auto& slot : views_) {
        if (!view_matches(slot, desc)) {
            continue;
        }
        auto& entry = slot.entry;

        auto scene_ptr = slot.camera_element->get_scene();
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
                render_target_cache_.on_element_removed(removed.get(), ctx);
            }
        }

        // Rebuild draw commands for changed elements. Pipelines are
        // not pre-compiled here; build_draw_calls / build_gbuffer_draw_calls
        // compile lazily on cache miss against the path's target_format.
        for (auto* element : state.redraw_list) {
            batch_builder_.rebuild_commands(element, render_ctx_);
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
                            TextureDesc tdesc{};
                            tdesc.width = tw;
                            tdesc.height = th;
                            tdesc.format = surf->format();
                            tdesc.sampler = surf->get_sampler_desc();
                            TextureId tid = resources_->ensure_texture_storage(surf, tdesc);
                            if (tid != 0) {
                                backend_->upload_texture(tid, pixels, tw, th);
                                buf->clear_dirty();
                                resources_uploaded = true;
                            }
                        }
                    } else {
                        size_t bsize = buf->get_data_size();
                        const uint8_t* bytes = buf->get_data();
                        if (!bytes || bsize == 0) {
                            continue;
                        }
                        GpuBufferDesc bdesc{};
                        bdesc.size = bsize;
                        bdesc.cpu_writable = true;
                        // IMeshBuffer carries an IBO half that needs
                        // INDEX_BUFFER usage so it can be bound for
                        // indexed draws.
                        if (auto* mb = interface_cast<IMeshBuffer>(buf)) {
                            bdesc.index_buffer = mb->get_ibo_size() > 0;
                        }
                        auto* be = resources_->ensure_buffer_storage(buf, bdesc);
                        if (!be) continue;
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
            for (auto& s : views_) {
                auto sp = s.camera_element->get_scene();
                if (interface_cast<IScene>(sp) == scene) {
                    s.entry.batches_dirty = true;
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
        frame_buffer_->begin_frame(slot.buffer);
        if (!slot.graph) {
            slot.graph = instance().create<IRenderGraph>(ClassId::RenderGraph);
            if (auto internal = interface_cast<IRenderGraphInternal>(slot.graph)) {
                internal->init(backend_.get());
            }
        }
        slot.graph->clear();

        // The retry loop reuses caches that hold per-frame_data
        // addresses. Attempt 1's cached addresses point into the OLD
        // frame_data buffer; after grow, those addresses still resolve
        // to OLD memory (which is deferred-destroyed but alive until
        // the submit completes). Strictly correct, but skipping the
        // retry-side reset hides bugs and wastes the OLD buffer's
        // contents. Clear them so attempt 2 re-uploads cleanly into
        // the NEW frame_data.
        material_cache_.clear();
        snippets_->begin_frame();

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
            // Force a full re-walk so the BVH cb fires for every
            // primitive and the per-instance log block runs. Clearing
            // the cache also bypasses the SceneBvh::rebuild AABB-hash
            // shortcut that would otherwise downgrade dirty back to
            // false on a static scene.
            if (log_bvh_next_) {
                dirty = true;
                bvh->force_full_rebuild();
            }
            struct BvhBuildState {
                Renderer*               self;
                FrameContext&           ctx;
                bool                    log;
            };
            BvhBuildState bvh_state{this, ctx, log_bvh_next_};
            bvh->rebuild(scene, render_ctx_, *frame_buffer_, dirty,
                +[](void* u, ShapeSite& site) {
                    auto& s = *static_cast<BvhBuildState*>(u);
                    auto& ctx = s.ctx;
                    auto& self = *s.self;
                    auto resolve_ctx = ctx.make_resolve_context();
                    auto mat = site.paint
                        ? self.snippets_->resolve_material(site.paint, resolve_ctx)
                        : IFrameSnippetRegistry::MaterialRef{};
                    uint32_t tex_id = 0;
                    if (site.draw_entry && site.draw_entry->texture_key != 0) {
                        auto* surf = reinterpret_cast<ISurface*>(
                            static_cast<uintptr_t>(site.draw_entry->texture_key));
                        tex_id = self.resources_->find_texture(surf);
                        if (tex_id == 0) {
                            uint64_t rt_id = get_render_target_id(surf);
                            if (rt_id != 0) tex_id = static_cast<uint32_t>(rt_id);
                        }
                    }
                    site.geometry.material_id = mat.mat_id;
                    site.geometry.material_data_addr = mat.mat_addr;
                    site.geometry.texture_id = tex_id;

                    if (auto* analytic = interface_cast<IAnalyticShape>(site.visual)) {
                        uint32_t kind = self.snippets_->register_intersect(analytic, *self.render_ctx_);
                        if (kind != 0) site.geometry.shape_kind = kind;
                    }

                    // Mesh-kind shapes: resolve the per-mesh static data
                    // address (stable across frames; cached on the cached
                    // shape's MeshInstanceData). The per-frame instance
                    // record itself is uploaded by SceneBvh during the
                    // re-publish pass. We just write a placeholder addr
                    // here for the first frame; SceneBvh's per-frame patch
                    // overwrites it on every subsequent frame.
                    if (site.has_mesh_data) {
                        if (auto* dd = interface_cast<IDrawData>(site.mesh_primitive)) {
                            site.mesh_instance.mesh_static_addr =
                                self.snippets_->resolve_data_buffer(dd, resolve_ctx);
                        }
                        site.geometry.mesh_data_addr = self.frame_buffer_->write(
                            &site.mesh_instance, sizeof(site.mesh_instance));

                        if (s.log && site.mesh_primitive) {
                            auto* mp = site.mesh_primitive;
                            auto buf = mp->get_buffer();
                            uint64_t buffer_addr = buf ? buf->get_gpu_handle(GpuResourceKey::Default) : 0;
                            uint32_t i_count = mp->get_index_count();
                            uint32_t v_stride = mp->get_vertex_stride();
                            uint32_t triangle_count = i_count / 3u;
                            uint32_t ibo_offset = static_cast<uint32_t>(
                                buf ? buf->get_ibo_offset() : 0);
                            VELK_LOG(I, "BVH cb: inst=%p mesh_static_addr=0x%016llx "
                                        "buffer_addr=0x%016llx ibo_offset=0x%08x "
                                        "triangle_count=%u v_stride=%u",
                                     (void*)mp,
                                     (unsigned long long)site.mesh_instance.mesh_static_addr,
                                     (unsigned long long)buffer_addr,
                                     ibo_offset, triangle_count, v_stride);
                        }
                    }
                }, &bvh_state);
            scene_bvhs[scene] = bvh;
            // One-shot consumed: clear so subsequent frames don't spam.
            log_bvh_next_ = false;
        }

        // Two-phase view processing so RTT passes can sit ahead of view
        // passes in the single slot.graph without shuffling between two
        // graphs. Phase 1 prepares each view (rebuild_batches accumulates
        // RTT subtrees into render_target_passes_); Phase 2 emits view
        // pipelines into the graph after RTT passes are already in.
        prepared_views_.clear();

        // Phase 1: prepare each matching view.
        for (auto& view_slot : views_) {
            if (!view_matches(view_slot, desc)) {
                continue;
            }

            auto scene_ptr = view_slot.camera_element->get_scene();
            auto* scene = interface_cast<IScene>(scene_ptr);
            if (!scene) {
                continue;
            }
            auto sit = consumed_scenes.find(scene);
            if (sit == consumed_scenes.end()) {
                continue;
            }

            // Discover view pipelines attached to the camera trait.
            auto camera_trait = ::velk::find_attachment<ICamera>(view_slot.camera_element);
            pipelines_scratch_.clear();
            if (auto* storage = interface_cast<IObjectStorage>(camera_trait.get())) {
                AttachmentQuery q;
                q.interfaceUid = IViewPipeline::UID;
                for (auto& a : storage->find_attachments(q)) {
                    if (auto p = interface_pointer_cast<IViewPipeline>(a)) {
                        pipelines_scratch_.emplace_back(std::move(p));
                    }
                }
            }
            if (pipelines_scratch_.empty()) {
                continue;
            }

            // BVH state for this view's scene. Set on ctx temporarily so
            // pipeline `needs()` and view_preparer that read it observe
            // the correct values for this view.
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
            ctx.view_camera_trait = camera_trait.get();

            IRenderPath::Needs needs;
            for (auto& p : pipelines_scratch_) {
                auto n = p->needs(ctx);
                needs.batches |= n.batches;
                needs.shapes  |= n.shapes;
                needs.lights  |= n.lights;
            }

            prepared_views_.emplace_back();
            auto& pv = prepared_views_.back();
            pv.slot = &view_slot;
            pv.camera_trait = camera_trait;
            pv.pipelines = std::move(pipelines_scratch_);
            pv.bvh_nodes_addr = ctx.bvh_nodes_addr;
            pv.bvh_shapes_addr = ctx.bvh_shapes_addr;
            pv.bvh_root = ctx.bvh_root;
            pv.bvh_node_count = ctx.bvh_node_count;
            pv.bvh_shape_count = ctx.bvh_shape_count;
            pv.render_view = view_preparer_.prepare(view_slot.entry,
                                                    view_slot.camera_element,
                                                    sit->second, ctx,
                                                    batch_builder_,
                                                    needs);

            ctx.view_camera_trait = nullptr;
        }

        // RTT textures must exist + carry the right render_target_id
        // BEFORE any draw_calls bake those ids into draw data. Run once
        // on the union of all views' render_target_passes_ now that
        // every view's batches have been rebuilt above.
        render_target_cache_.ensure(ctx, batch_builder_);

        // RTT subtree passes go into slot.graph first. Bindless texture
        // reads from materials are graph-invisible (Tier 1), so the
        // RTT-before-views ordering is enforced by emit order here.
        render_target_cache_.emit_passes(ctx, batch_builder_, *slot.graph);

        // Phase 2: emit each prepared view's pipelines into slot.graph.
        for (auto& pv : prepared_views_) {
            ctx.bvh_nodes_addr = pv.bvh_nodes_addr;
            ctx.bvh_shapes_addr = pv.bvh_shapes_addr;
            ctx.bvh_root = pv.bvh_root;
            ctx.bvh_node_count = pv.bvh_node_count;
            ctx.bvh_shape_count = pv.bvh_shape_count;
            ctx.view_camera_trait = pv.camera_trait.get();

            IRenderTarget::Ptr color_target =
                interface_pointer_cast<IRenderTarget>(pv.slot->entry.surface);
            for (auto& p : pv.pipelines) {
                seen_pipelines_.insert(p.get());
                p->emit(pv.slot->entry, pv.render_view, color_target, ctx, *slot.graph);
            }
            ctx.view_camera_trait = nullptr;
        }

        // Debug overlays: tail-appended so they blit on top of whatever
        // the view passes produced on the target surface.
        for (auto& ov : debug_overlays_) {
            if (!ov.surface || ov.texture_id == 0) continue;
            auto gp = instance().create<IRenderPass>(ClassId::DefaultRenderPass);
            if (!gp) continue;
            gp->add_op(ops::BlitToSurface{
                ov.texture_id,
                ov.surface->get_gpu_handle(GpuResourceKey::Default),
                ov.dst_rect});
            slot.graph->add_pass(std::move(gp));
        }

        if (!frame_buffer_->overflowed()) {
            break;
        }
        if (attempt >= kMaxRecordRetries) {
            VELK_LOG(
                E, "Renderer: frame buffer overflow after %d retries, dropping frame", kMaxRecordRetries);
            break;
        }
        frame_buffer_->grow(*backend_, *resources_);
    } // retry loop
}

Frame Renderer::prepare(const FrameDesc& desc)
{
    if (!backend_) {
        return {};
    }
    VELK_PERF_EVENT(Render);
    VELK_PERF_SCOPE("renderer.prepare");

    last_prepare_gpu_wait_ns_ = 0;

    // Drain any GPU resources whose safe window has elapsed. Both the
    // renderer's persistent manager and each frame slot's graph-owned
    // transient manager get a chance to release handles whose GPU
    // completion markers have resolved.
    drain_deferred(resources_.get(), *backend_);
    for (auto& slot : frame_slots_) {
        if (slot.graph) {
            drain_deferred(&slot.graph->resources(), *backend_);
        }
    }

    auto* slot = claim_frame_slot();
    active_slot_ = slot;

    // If this slot's buffer is undersized (it was in-flight during a
    // previous regrow and was skipped), grow it now that it's safe.
    frame_buffer_->ensure_slot(slot->buffer, *backend_, *resources_);

    // Prepare the staging buffer once for all views
    frame_buffer_->ensure_capacity(*backend_, *resources_);

    if ((slot->id % 10000) == 0) {
        VELK_LOG(I,
                 "Renderer: frame %llu, frame buffer %zu KB, peak usage %zu KB",
                 static_cast<unsigned long long>(slot->id),
                 frame_buffer_->get_buffer_size() / 1024,
                 frame_buffer_->get_peak_usage() / 1024);
    }

    auto consumed_scenes = consume_scenes(desc);

    batch_builder_.reset_frame_state();
    material_cache_.clear();
    snippets_->begin_frame();
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

        // Discard older unpresented frames that target overlapping
        // surfaces. Compares the writes lists: if any write Ptr in a
        // pending slot's pass matches a write in the frame being
        // presented, the slot's pass is dropped. Tier 2 graph deps will
        // subsume this.
        auto pass_writes_target = [](const IRenderPass& pass) -> uint64_t {
            for (auto& w : pass.writes()) {
                if (!w) continue;
                uint64_t id = get_render_target_id(w);
                if (id != 0) return id;
            }
            return 0;
        };
        for (auto& s : frame_slots_) {
            if (!s.ready || s.id >= frame.id) {
                continue;
            }
            if (!s.graph || !target->graph) continue;
            auto& s_passes = s.graph->passes();
            auto& t_passes = target->graph->passes();
            for (auto it = s_passes.begin(); it != s_passes.end();) {
                bool overlaps = false;
                if (!*it) { it = s_passes.erase(it); continue; }
                uint64_t it_id = pass_writes_target(**it);
                if (it_id != 0) {
                    for (auto& tp : t_passes) {
                        if (tp && pass_writes_target(*tp) == it_id) {
                            overlaps = true;
                            break;
                        }
                    }
                }
                if (overlaps) {
                    it = s_passes.erase(it);
                } else {
                    ++it;
                }
            }
            // If all passes were removed, mark slot as not ready but preserve presented_at
            // so the buffer isn't reused before the GPU is done with it
            if (s_passes.empty()) {
                s.ready = false;
                s.presented_at = present_counter_ + 1;
            }
        }

        // Submit all passes within a single frame
        {
            VELK_PERF_SCOPE("renderer.wait_vsync");
            backend_->begin_frame();
        }
        target->graph->compile();
        {
            VELK_PERF_SCOPE("renderer.submit");
            target->graph->execute(*backend_);
        }
        {
            VELK_PERF_SCOPE("renderer.end_frame");
            backend_->end_frame();
        }
        present_counter_++;
        target->ready = false;
        target->presented_at = present_counter_;
        target->gpu_completion_marker = backend_->frame_completion_marker();
        target->graph->clear();

        slot_cv_.notify_one();
    }
    VELK_PERF_EVENT(Present);

    if (diag_enabled_) {
        ++diag_.frames;
        constexpr uint64_t kDiagPeriod = 60;  // ~1s at 60fps
        if (diag_.frames % kDiagPeriod == 0) log_diagnostics();
    }
}

void Renderer::log_diagnostics()
{
    // Pull rebuild/fast-path counters out of the view preparer for this
    // window and reset them — those are accumulated across all views.
    uint64_t rebuilds   = view_preparer_.diag_rebuild_count;
    uint64_t fast_paths = view_preparer_.diag_fast_path_count;
    view_preparer_.diag_rebuild_count   = 0;
    view_preparer_.diag_fast_path_count = 0;

    // Resource manager / frame data sizes — anything that monotonically
    // grows over time will surface here as a steadily-increasing number.
    auto* mgr_internal = interface_cast<IGpuResourceManagerInternal>(resources_.get());
    size_t deferred_buffers = mgr_internal ? mgr_internal->deferred_buffer_count() : 0;
    size_t deferred_textures = mgr_internal ? mgr_internal->deferred_texture_count() : 0;
    size_t deferred_groups = mgr_internal ? mgr_internal->deferred_group_count() : 0;

    size_t fd_buffer_kb = frame_buffer_ ? frame_buffer_->get_buffer_size() / 1024 : 0;
    size_t fd_peak_kb = frame_buffer_ ? frame_buffer_->get_peak_usage() / 1024 : 0;

    size_t total_passes = 0;
    for (auto& s : frame_slots_) {
        if (s.graph) total_passes += s.graph->passes().size();
    }

    VELK_LOG(I,
             "render.diag frames=%llu rebuilds=%llu fast_path=%llu "
             "views=%zu batches=%zu element_slots=%zu "
             "fd_size_kb=%zu fd_peak_kb=%zu "
             "deferred(b=%zu t=%zu g=%zu) "
             "graph_passes=%zu seen_pipelines=%zu "
             "scratch=%zu views=%zu ",

             static_cast<unsigned long long>(diag_.frames),
             static_cast<unsigned long long>(rebuilds),
             static_cast<unsigned long long>(fast_paths),
             view_preparer_.view_count(),
             view_preparer_.total_batches(),
             view_preparer_.total_element_slots(),
             fd_buffer_kb,
             fd_peak_kb,
             deferred_buffers,
             deferred_textures,
             deferred_groups,
             total_passes,
             seen_pipelines_.size(),
             pipelines_scratch_.size(),
             prepared_views_.size());
    VELK_LOG(I, "bvh=%zu", scene_bvh_cache_.size());
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
        frame_buffer_->init_slot(frame_slots_[i].buffer, *backend_, *resources_);
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

IGpuResource::Ptr Renderer::get_named_output(const IElement::Ptr& camera_element,
                                              const IWindowSurface::Ptr& surface,
                                              string_view name) const
{
    for (auto& s : views_) {
        if (s.camera_element.get() != camera_element.get()) continue;
        if (s.entry.surface.get() != surface.get()) continue;
        auto cam_trait = ::velk::find_attachment<ICamera>(s.camera_element);
        auto path = cam_trait
                        ? ::velk::find_attachment<IRenderPath>(cam_trait.get())
                        : IRenderPath::Ptr{};
        if (!path) return {};
        return path->find_named_output(name, const_cast<ViewEntry*>(&s.entry));
    }
    return {};
}


void Renderer::shutdown()
{
    if (backend_) {
        // Drain the GPU before destroying anything: subsequent
        // backend->destroy_* calls assume nothing is still in use.
        backend_->wait_idle();

        // Detach the resource manager from every GPU resource still
        // observing it so their dtors (which may run later, on any
        // thread) cannot reach a manager whose backend is gone.
        // Env resources are unregistered inside the manager's own
        // shutdown(); element-cache resources are unregistered here
        // since the manager doesn't track that set itself.
        auto* obs = interface_cast<IGpuResourceObserver>(resources_.get());
        if (obs) {
            for (auto& [elem, cache] : batch_builder_.element_cache()) {
                for (auto& weak : cache.gpu_resources) {
                    if (auto res = weak.lock()) {
                        res->remove_gpu_resource_observer(obs);
                    }
                }
            }
        }

        // Cleanup the resource manager (destroys backing GPU resources, unsubscribes IGpuResource objects)
        resources_.reset();

        // Per-sub-renderer cleanup (RTT textures, RT storage textures, etc.).
        {
            FrameContext ctx = make_frame_context();
            for (auto& s : views_) {
                auto cam_trait = ::velk::find_attachment<ICamera>(s.camera_element);
                ctx.view_camera_trait = cam_trait.get();
                for (auto* pipeline : seen_pipelines_) {
                    pipeline->on_view_removed(s.entry, ctx);
                }
                ctx.view_camera_trait = nullptr;
            }
            for (auto* pipeline : seen_pipelines_) {
                pipeline->shutdown(ctx);
            }
            seen_pipelines_.clear();
            render_target_cache_.shutdown(ctx);
            view_preparer_.clear();
        }

        for (auto& s : views_) {
            backend_->destroy_surface(get_render_target_id(s.entry.surface));
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


} // namespace velk
