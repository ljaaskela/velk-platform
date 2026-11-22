#include "batch_builder.h"

#include "default_batch.h"
#include "pipeline_options_helpers.h"
#include "visual/primitive_shaders.h"

#include <velk-scene/plugin.h>

#include <velk/api/attachment.h>
#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_object_storage.h>
#include <velk/string.h>

#include <velk-scene/instance_types.h>
#include <velk-scene/interface/trait/intf_clip.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_set>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/intf_shader_source.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/interface/material/intf_material_options.h>
#include <velk-scene/interface/intf_visual.h>

namespace {

uint64_t make_batch_key(uint64_t pipeline, uint64_t mesh, uint64_t texture)
{
    return (pipeline * 31 + mesh) * 31 + texture;
}

} // namespace

namespace velk {

void BatchBuilder::rebuild_commands(IElement* element, IRenderContext* render_ctx)
{
    VELK_PERF_SCOPE("renderer.rebuild_commands");
    auto& cache = element_cache_[element];
    cache.before_visuals.clear();
    cache.after_visuals.clear();
    cache.gpu_resources.clear();

    auto* storage = interface_cast<IObjectStorage>(element);
    if (!storage) {
        return;
    }

    auto state = read_state<IElement>(element);
    if (!state) {
        return;
    }

    const ::velk::size& local_size = state->size;

    for (size_t i = 0; i < storage->attachment_count(); ++i) {
        auto att = storage->get_attachment(i);
        auto* visual = interface_cast<IVisual>(att);
        if (!visual) {
            continue;
        }

        VisualCommands vc;
        {
            VELK_PERF_SCOPE("renderer.get_draw_entries");
            if (render_ctx) {
                vc.entries = visual->get_draw_entries(*render_ctx, local_size);
            }
        }

        // Resolve the visual's primitive. 3D visuals (cube, sphere,
        // future MeshVisual) populate `entry.primitive` themselves
        // inside `get_draw_entries`; 2D visuals leave it null and fall
        // back to the unit quad's primitive here.
        //
        // The unit quad's buffers are singletons that outlive every
        // element. They get destroyed when the render context goes
        // away, which is AFTER the renderer (Application destroys
        // renderer_ before render_ctx_). So we deliberately skip
        // add_gpu_resource_observer here: the buffer's destructor would
        // otherwise notify a dead renderer at process exit.
        IMeshPrimitive::Ptr quad_primitive;
        if (render_ctx) {
            auto quad = render_ctx->get_mesh_builder().get_unit_quad();
            if (quad) {
                auto prims = quad->get_primitives();
                if (prims.size() > 0) {
                    quad_primitive = prims[0];
                }
            }
        }
        // Surface every referenced primitive's buffer for the upload pass.
        // Includes the optional UV1 stream so its arena region exists by the
        // time batch_builder writes its base into DrawDataHeader::uv1_base.
        auto push_primitive_buffer = [&](IMeshPrimitive* p) {
            if (!p) return;
            if (auto buf = interface_pointer_cast<IBuffer>(p->get_buffer())) {
                cache.gpu_resources.push_back(buf);
            }
            if (auto uv1 = interface_pointer_cast<IBuffer>(p->get_uv1_buffer())) {
                cache.gpu_resources.push_back(uv1);
            }
            if (auto color = interface_pointer_cast<IBuffer>(p->get_color_buffer())) {
                cache.gpu_resources.push_back(color);
            }
        };
        push_primitive_buffer(quad_primitive.get());
        for (auto& entry : vc.entries) {
            if (!entry.primitive) {
                entry.primitive = quad_primitive;
            } else {
                push_primitive_buffer(entry.primitive.get());
            }
        }

        // Capture the visual's IShaderSource if it has one. Paths
        // splice the roles they need (Vertex / Fragment / Discard)
        // into their own driver templates at lazy-compile time.
        if (interface_cast<IShaderSource>(visual)) {
            vc.shader_source = interface_pointer_cast<IShaderSource>(att);
        }

        // Per-entry material resolution. Compilation is lazy in
        // build_draw_calls, so this pass only surfaces the material's
        // textures for upload registration and seeds entry.pipeline_override
        // from the material's stored handle (zero on the very first
        // frame it's used; build_draw_calls allocates and back-fills).
        {
            VELK_PERF_SCOPE("renderer.resolve_material");
            std::unordered_set<IProgram*> seen;
            for (auto& entry : vc.entries) {
                if (!entry.material || !render_ctx) continue;
                auto prog = entry.material;
                if (seen.insert(prog.get()).second) {
                    if (auto* mat = interface_cast<IMaterial>(prog.get())) {
                        // Multi-texture materials (e.g. StandardMaterial)
                        // attach their textures to the material. Surface
                        // the ones that carry uploadable pixel data so
                        // renderer's upload pass registers them. The
                        // manager observer subscribes itself inside
                        // register_*; we only need to track the buffer
                        // so the upload pass sees it.
                        for (auto* tex : mat->get_textures()) {
                            if (auto buf = ::velk::get_self<IBuffer>(tex)) {
                                cache.gpu_resources.push_back(buf);
                            }
                        }
                    }
                }
                entry.pipeline_override = prog->get_pipeline_handle(*render_ctx);
            }
        }

        for (auto& res : (render_ctx ? visual->get_gpu_resources(*render_ctx) : vector<IBuffer::Ptr>{})) {
            if (res) {
                cache.gpu_resources.push_back(res);
            }
        }

        auto v2d = read_state<IVisual2D>(visual);
        VisualPhase phase = v2d ? v2d->visual_phase : VisualPhase::BeforeChildren;
        if (phase == VisualPhase::AfterChildren) {
            cache.after_visuals.push_back(std::move(vc));
        } else {
            cache.before_visuals.push_back(std::move(vc));
        }
    }
}

void BatchBuilder::rebuild_batches(const SceneState& state,
                                   vector<IBatch::Ptr>& out_batches,
                                   ElementSlotMap& out_slots,
                                   std::unordered_set<IElement*>& out_rtt_roots,
                                   IRenderContext* render_ctx)
{
    VELK_PERF_SCOPE("renderer.rebuild_batches");
    out_batches.clear();
    out_slots.clear();
    out_rtt_roots.clear();
    // Note: render_target_passes_ is NOT cleared here — it's cleared
    // once per frame in reset_frame_state (called at Renderer::prepare
    // start). Each view's rebuild_batches appends; find_or_make_pass
    // dedups so an RTT element shared between views only emits once
    // per frame from build_shared_passes.

    auto resolve_texture = [](const IProgram* material, uint64_t fallback) -> uint64_t {
        return material ? reinterpret_cast<uintptr_t>(material) : fallback;
    };

    auto get_visuals = [](const ElementCache& cache, VisualPhase phase) -> const vector<VisualCommands>& {
        return phase == VisualPhase::AfterChildren ? cache.after_visuals : cache.before_visuals;
    };

    // 1 + the clip slot that applies to each element (its own or an ancestor's), from the walk.
    std::unordered_map<IElement*, uint32_t> element_clip;

    auto emit_visuals = [&](const vector<IElement*>& entries, VisualPhase phase,
                            vector<IBatch::Ptr>& target_batches,
                            float offset_x = 0.f, float offset_y = 0.f,
                            bool track_slots = true) {
        uint64_t last_bkey = 0;
        size_t max_visuals = 0;
        for (auto* elem : entries) {
            auto it = element_cache_.find(elem);
            if (it != element_cache_.end()) {
                max_visuals = std::max(max_visuals, get_visuals(it->second, phase).size());
            }
        }

        for (size_t pass = 0; pass < max_visuals; ++pass) {
            for (auto* elem : entries) {
                auto it = element_cache_.find(elem);
                if (it == element_cache_.end()) {
                    continue;
                }

                auto& visuals = get_visuals(it->second, phase);
                if (pass >= visuals.size()) {
                    continue;
                }

                auto elem_state = read_state<IElement>(elem);
                if (!elem_state) {
                    continue;
                }

                // Clipped instances carry their clip. A render target's subtree is drawn in
                // target-local space, which the world-space rectangles do not match, so it
                // is not clipped. An element wholly outside its clip is left out; it gets no
                // slot, so moving it rebuilds the batches and it comes back when it shows.
                uint32_t clip = 0;
                if (track_slots) {
                    if (auto found = element_clip.find(elem); found != element_clip.end()) {
                        clip = found->second;
                    }
                }
                if (clip != 0) {
                    const auto& r = clip_slots_[clip - 1].rect;
                    const auto& box = elem_state->world_aabb;
                    if (box.position.x >= r[2] || box.position.x + box.extent.width <= r[0]
                        || box.position.y >= r[3] || box.position.y + box.extent.height <= r[1]) {
                        continue;
                    }
                }
                const uint32_t clip_word = clip_bits(clip);

                // Every instance layout begins with a mat4 world_matrix
                // (see instance_types.h). We copy the element's transform
                // here and — for RTT sub-passes — subtract the RTT root's
                // world translation so children render in target-local
                // space. Everything downstream (rotation, 3D z) comes for
                // free because the full matrix is passed through.
                mat4 world = elem_state->world_matrix;
                world(0, 3) -= offset_x;
                world(1, 3) -= offset_y;

                auto& vc = visuals[pass];
                IBatch::Ptr cull_owner;
                for (auto& de : vc.entries) {
                    uint64_t pipeline = (de.pipeline_override != 0) ? de.pipeline_override : de.pipeline_key;
                    uint64_t texture = de.texture_key;
                    uint64_t prim_key = reinterpret_cast<uintptr_t>(de.primitive.get());

                    uint64_t bkey = make_batch_key(pipeline, prim_key,
                                                   resolve_texture(de.material.get(), texture));

                    if (target_batches.empty() || bkey != last_bkey || de.instance_source) {
                        auto batch_ptr = ::velk::instance().create<IBatch>(
                            ClassId::DefaultBatch);
                        if (!batch_ptr) continue;
                        auto* batch = static_cast<impl::DefaultBatch*>(batch_ptr.get());
                        batch->set_pipeline_key(pipeline);
                        batch->set_texture_key(texture);
                        batch->set_instance_stride(de.instance_size);
                        batch->set_material(de.material);
                        batch->set_primitive(de.primitive);
                        batch->set_shader_source(vc.shader_source);

                        // Capture pipeline options now so build_draw_calls
                        // can lazy-compile against any target_format
                        // without re-walking attachments. Material wins
                        // as the pipeline-source when present (it carries
                        // its own MaterialOptions); otherwise the visual
                        // owns the pipeline via IShaderSource.
                        IObjectStorage* opts_storage = nullptr;
                        if (de.material) {
                            opts_storage = interface_cast<IObjectStorage>(de.material.get());
                        } else if (vc.shader_source) {
                            opts_storage = interface_cast<IObjectStorage>(vc.shader_source.get());
                        }
                        PipelineOptions po = pipeline_options_from_storage(opts_storage);
                        if (de.primitive) {
                            po.topology = to_backend_topology(de.primitive->get_topology());
                        }
                        batch->set_pipeline_options(po);
                        // Lists are per view, so a render target's subtree stays out, and
                        // the order of blended triangles matters.
                        if (track_slots && de.primitive && !de.instance_source && po.blend_mode != BlendMode::Alpha
                            && de.instance_size == sizeof(ElementInstance)) {
                            batch->enable_lod_cull();
                        }

                        target_batches.push_back(std::move(batch_ptr));
                        last_bkey = bkey;
                    }

                    auto* batch = static_cast<impl::DefaultBatch*>(
                        target_batches.back().get());

                    // A batch of its own, over the visual's region. An empty slot list
                    // tells the transform path this element needs no batch rewrite.
                    if (auto source = interface_pointer_cast<IInstancedMeshInternal>(de.instance_source)) {
                        // Every slot: a region loaded by need grows, and what culling sizes
                        // by this must hold whatever comes in.
                        batch->set_instance_count(static_cast<uint32_t>(source->get_transforms().size()));
                        batch->set_instance_source(std::move(source));
                        batch->set_world_aabb(elem_state->world_aabb);
                        last_bkey = 0;
                        // A render target's subtree draws all its instances, since
                        // these lists are per view.
                        if (track_slots) {
                            batch->enable_instance_cull(cull_owner, de.instance_source_mesh);
                            if (!cull_owner) {
                                cull_owner = target_batches.back();
                            }
                            out_slots[elem];
                        }
                        continue;
                    }

                    auto& instance_data = batch->mutable_instance_data();

                    auto data_offset = instance_data.size();
                    uint32_t slot_index = batch->instance_count();
                    instance_data.resize(data_offset + de.instance_size);
                    std::memcpy(instance_data.data() + data_offset, de.instance_data,
                                de.instance_size);

                    // Overwrite the leading mat4 world_matrix slot. The
                    // visual leaves it zero-initialised; we fill it per
                    // instance so shaders get the correct transform.
                    std::memcpy(instance_data.data() + data_offset, world.m, sizeof(world.m));
                    // The element's object id, the same as in its BVH records.
                    if (de.instance_size == sizeof(ElementInstance) && state.scene) {
                        auto* instance = reinterpret_cast<ElementInstance*>(instance_data.data() + data_offset);
                        instance->params[3] = state.scene->get_object_ids(*elem, 1);
                    }
                    if (de.instance_size == sizeof(ElementInstance)) {
                        auto* instance = reinterpret_cast<ElementInstance*>(instance_data.data() + data_offset);
                        std::memcpy(&instance->offset.w, &clip_word, sizeof(clip_word));
                    }

                    batch->set_instance_count(slot_index + 1);
                    batch->set_world_aabb(aabb::merge(batch->world_aabb(), elem_state->world_aabb));

                    if (track_slots) {
                        out_slots[elem].push_back(ElementSlot{
                            batch, slot_index, offset_x, offset_y});
                    }
                }
            }
        }
    };

    // Walk the scene tree and route elements into main vs RTT subtree
    // lists. `main_*` keeps top-level elements; each RenderTarget-marked
    // subtree produces its own RenderTargetPassData with pre- and post-
    // order element lists. Sibling z-sort matches the previous Scene-
    // side visual_list build.
    vector<IElement*> main_before;
    vector<IElement*> main_after;

    auto* scene = state.scene;

    // Returns a fresh pass entry for `rt_elem`, or nullptr if some
    // earlier view's rebuild this frame already populated this element
    // (same scene walked by two views → same subtree). The nullptr
    // answer makes the walk skip re-appending into the existing entry,
    // so the RTT pass draws the subtree exactly once.
    auto find_or_make_pass = [&](IElement* rt_elem) -> RenderTargetPassData* {
        for (auto& rtp : render_target_passes_) {
            if (rtp.element == rt_elem) return nullptr;
        }
        render_target_passes_.push_back({rt_elem, {}, {}, {}});
        return &render_target_passes_.back();
    };

    // Every element is recorded in the ambient (main) lists. An
    // element inside an active RTT subtree is ALSO recorded into that
    // pass's lists so the subtree renders into the target texture.
    // An element carrying a RenderCache trait starts its own pass that
    // covers itself + all descendants, so a textured rect sampling its
    // own RTT renders correctly into both.
    std::function<void(const IObject::Ptr&,
                       vector<IElement*>&, vector<IElement*>&,
                       RenderTargetPassData*, uint32_t)> walk;
    walk = [&](const IObject::Ptr& obj,
               vector<IElement*>& ambient_before,
               vector<IElement*>& ambient_after,
               RenderTargetPassData* active_pass,
               uint32_t clip) {
        auto elem_ptr = interface_pointer_cast<IElement>(obj);
        if (!elem_ptr) return;
        auto* elem = elem_ptr.get();

        // A clip element's own visuals are cut at its box, its descendants at its clip_rect.
        if (auto c = ::velk::find_attachment<IClip>(obj); c) {
            auto cs = read_state<IClip>(c);
            if (cs && cs->enabled) {
                const uint32_t own = clip_slot(obj, clip, false);
                element_clip[elem] = own;
                clip = clip_slot(obj, own, true);
            }
        }
        if (clip != 0 && !element_clip.count(elem)) {
            element_clip[elem] = clip;
        }

        RenderTargetPassData* inner_pass = active_pass;
        if (elem->has_render_traits()) {
            inner_pass = find_or_make_pass(elem);
            out_rtt_roots.insert(elem);
        }

        ambient_before.push_back(elem);
        if (inner_pass) inner_pass->before_entries.push_back(elem);

        if (scene) {
            auto kids = scene->children_of(obj);
            std::sort(kids.begin(), kids.end(),
                [](const IObject::Ptr& a, const IObject::Ptr& b) {
                    auto ra = read_state<IElement>(a);
                    auto rb = read_state<IElement>(b);
                    int32_t za = ra ? ra->z_index : 0;
                    int32_t zb = rb ? rb->z_index : 0;
                    return za < zb;
                });
            for (auto& kid : kids) {
                walk(kid, ambient_before, ambient_after, inner_pass, clip);
            }
        }

        ambient_after.push_back(elem);
        if (inner_pass) inner_pass->after_entries.push_back(elem);
    };

    if (scene) {
        walk(scene->root(), main_before, main_after, nullptr, 0);
    }
    compute_clip_rects();

    // Batch render target passes (offset by the element's world position).
    // Skip slot tracking for RTT subtrees: their batches live on the
    // RenderTargetPassData, not in out_batches, so the per-view fast path
    // can't reach them. Any dirty element inside an RTT subtree forces a
    // full rebuild via the rtt_roots check on the fast path.
    for (auto& rtp : render_target_passes_) {
        auto es = read_state<IElement>(rtp.element);
        float ox = es ? es->world_matrix(0, 3) : 0.f;
        float oy = es ? es->world_matrix(1, 3) : 0.f;
        emit_visuals(rtp.before_entries, VisualPhase::BeforeChildren, rtp.batches, ox, oy, /*track_slots=*/false);
        emit_visuals(rtp.after_entries, VisualPhase::AfterChildren, rtp.batches, ox, oy, /*track_slots=*/false);
    }

    emit_visuals(main_before, VisualPhase::BeforeChildren, out_batches);
    emit_visuals(main_after, VisualPhase::AfterChildren, out_batches);

    // The view's quads (IBatch::draws_quads): the unit quad with the shaders every 3D mesh has,
    // no material, back faces too, an instance per quad in the view's quad list. Anywhere.
    auto quad = render_ctx && scene && scene->get_default_bvh() ? render_ctx->get_mesh_builder().get_unit_quad() : nullptr;
    if (quad && !quad->get_primitives().empty()) {
        if (!quad_source_) {
            quad_source_ = ::velk::instance().create<IShaderSource>(ClassId::Visual::Mesh);
            // A mesh visual's 3D pipeline state, which it otherwise takes on when first drawn.
            if (quad_source_) {
                ::velk::find_or_create_attachment<IMaterialOptions>(quad_source_.get(), ClassId::MaterialOptions);
            }
        }
        auto batch_ptr = quad_source_ ? ::velk::instance().create<IBatch>(ClassId::DefaultBatch) : nullptr;
        if (batch_ptr) {
            auto* batch = static_cast<impl::DefaultBatch*>(batch_ptr.get());
            const auto& primitive = quad->get_primitives()[0];
            batch->set_pipeline_key(impl::kPrimitive3DPipelineKey);
            batch->set_primitive(primitive);
            batch->set_shader_source(quad_source_);
            PipelineOptions po = pipeline_options_from_storage(interface_cast<IObjectStorage>(quad_source_.get()));
            po.topology = to_backend_topology(primitive->get_topology());
            po.cull_mode = CullMode::None;
            batch->set_pipeline_options(po);
            batch->set_draws_quads();
            aabb everywhere;
            everywhere.position = {-1e30f, -1e30f, -1e30f};
            everywhere.extent = {2e30f, 2e30f, 2e30f};
            batch->set_world_aabb(everywhere);
            out_batches.push_back(std::move(batch_ptr));
        }
    }

    // Per-batch finalize: pack each batch's [args(32)][count(16)]
    // [instance_data] blob into the composed storage IBuffer and mark
    // it dirty. The renderer's standard buffer-upload pipeline (run by
    // ViewPreparer::prepare_batches) allocates the backing GpuBufferHandle
    // via ensure_buffer_storage and copies the blob.
    auto finalize_batches = [](vector<IBatch::Ptr>& batches) {
        for (auto& batch_ptr : batches) {
            auto* batch = static_cast<impl::DefaultBatch*>(batch_ptr.get());
            // Already-finalized batches (RTT batches dedup across views
            // via find_or_make_pass; previously-uploaded batches retain
            // their inner IBuffer) skip re-finalizing.
            if (batch->storage_buffer() != nullptr) continue;

            // Sanity: stride * count must match instance_data exactly.
            // Inconsistency would land bogus indirect args on GPU.
            const size_t expected = static_cast<size_t>(batch->instance_stride())
                                    * static_cast<size_t>(batch->instance_count());
            if (!batch->has_instance_source() && batch->instance_data().size() != expected) {
                VELK_LOG(W,
                         "DefaultBatch::finalize: SKIP inconsistent shape "
                         "stride=%u count=%u idata=%zu (expected %zu)",
                         batch->instance_stride(), batch->instance_count(),
                         batch->instance_data().size(), expected);
                continue;
            }

            auto primitive = batch->primitive();
            if (!primitive) continue;
            auto buffer = primitive->get_buffer();
            const bool indexed = buffer && buffer->get_ibo_size() > 0;
            uint32_t prim_count = indexed ? primitive->get_index_count()
                                          : primitive->get_vertex_count();
            batch->finalize_storage(prim_count, indexed);
        }
    };
    for (auto& rtp : render_target_passes_) finalize_batches(rtp.batches);
    finalize_batches(out_batches);
}

void BatchBuilder::set_clip_arena(IGpuArena* arena)
{
    clip_arena_ = arena;
    if (clip_arena_ && !clip_region_.valid()) {
        clip_region_ = clip_arena_->alloc(kMaxClips * 4 * sizeof(float));
    }
}

uint32_t BatchBuilder::clip_bits(uint32_t clip) const
{
    if (clip == 0 || !clip_region_.valid()) {
        return 0;
    }
    return static_cast<uint32_t>(clip_region_.offset() / (4 * sizeof(float))) + clip;
}

uint32_t BatchBuilder::clip_slot(const IObject::Ptr& obj, uint32_t parent, bool content)
{
    auto* elem = interface_cast<IElement>(obj);
    auto& slot_of = clip_slot_of_[content ? 1 : 0];
    uint32_t slot = 0;
    if (auto it = slot_of.find(elem); it != slot_of.end()) {
        slot = it->second;
    } else {
        // Reuse the slot of an element that is gone, else add one.
        slot = static_cast<uint32_t>(clip_slots_.size());
        for (uint32_t i = 0; i < clip_slots_.size(); ++i) {
            if (clip_slots_[i].element.expired()) {
                slot = i;
                break;
            }
        }
        if (slot == clip_slots_.size()) {
            if (slot >= kMaxClips) {
                VELK_LOG(W, "BatchBuilder: more than %u clips; the rest do not clip", kMaxClips);
                return parent;
            }
            clip_slots_.emplace_back();
        }
        for (auto& map : clip_slot_of_) {
            for (auto it = map.begin(); it != map.end();) {
                it = it->second == slot ? map.erase(it) : std::next(it);
            }
        }
        slot_of[elem] = slot;
    }
    clip_slots_[slot].element = obj;
    clip_slots_[slot].content = content;
    clip_slots_[slot].parent = parent;
    return slot + 1;
}

void BatchBuilder::compute_clip_rects()
{
    vector<uint8_t> done(clip_slots_.size(), 0);
    std::function<void(uint32_t)> compute = [&](uint32_t s) {
        if (done[s]) {
            return;
        }
        done[s] = 1;
        auto& slot = clip_slots_[s];
        float r[4] = {0.f, 0.f, 0.f, 0.f};
        auto obj = slot.element.lock();
        auto clip = obj ? ::velk::find_attachment<IClip>(obj) : nullptr;
        if (auto state = read_state<IElement>(obj); state && clip) {
            // The clip rectangle's corners in world space; their x and y extent is the rectangle.
            const auto& m = state->world_matrix;
            const rect local = slot.content ? clip->clip_rect(state->size)
                                            : rect{0.f, 0.f, state->size.width, state->size.height};
            const float x0 = local.x;
            const float y0 = local.y;
            const float x1 = local.x + local.width;
            const float y1 = local.y + local.height;
            const float xs[4] = {x0, x1, x0, x1};
            const float ys[4] = {y0, y0, y1, y1};
            for (int i = 0; i < 4; ++i) {
                const float x = m(0, 0) * xs[i] + m(0, 1) * ys[i] + m(0, 3);
                const float y = m(1, 0) * xs[i] + m(1, 1) * ys[i] + m(1, 3);
                if (i == 0 || x < r[0]) r[0] = x;
                if (i == 0 || y < r[1]) r[1] = y;
                if (i == 0 || x > r[2]) r[2] = x;
                if (i == 0 || y > r[3]) r[3] = y;
            }
        }
        const uint32_t parent = slot.parent;
        if (parent != 0 && parent - 1 < clip_slots_.size() && parent - 1 != s) {
            compute(parent - 1);
            const auto& p = clip_slots_[parent - 1].rect;
            r[0] = std::max(r[0], p[0]);
            r[1] = std::max(r[1], p[1]);
            r[2] = std::min(r[2], p[2]);
            r[3] = std::min(r[3], p[3]);
        }
        std::memcpy(slot.rect, r, sizeof(r));
    };
    for (uint32_t s = 0; s < clip_slots_.size(); ++s) {
        compute(s);
    }
}

void BatchBuilder::update_clips()
{
    if (clip_slots_.empty() || !clip_arena_ || !clip_region_.valid()) {
        return;
    }
    compute_clip_rects();
    for (uint32_t s = 0; s < clip_slots_.size(); ++s) {
        auto& slot = clip_slots_[s];
        if (slot.uploaded && std::memcmp(slot.rect, slot.written, sizeof(slot.rect)) == 0) {
            continue;
        }
        clip_arena_->update_at(clip_region_.offset() + s * sizeof(slot.rect), slot.rect, sizeof(slot.rect));
        std::memcpy(slot.written, slot.rect, sizeof(slot.rect));
        slot.uploaded = true;
    }
}

// build_draw_calls and build_gbuffer_draw_calls moved to
// IRenderContext (impl in velk-render/src/render_context.cpp).
// BatchBuilder now only handles scene-walking (rebuild_commands /
// rebuild_batches) plus the per-frame material-cache lifecycle.

} // namespace velk
