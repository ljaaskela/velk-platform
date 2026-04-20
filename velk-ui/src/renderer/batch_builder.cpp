#include "batch_builder.h"

#include "default_ui_shaders.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/interface/intf_raster_shader.h>
#include <velk-ui/interface/intf_visual.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <velk/string.h>

namespace {

uint64_t make_batch_key(uint64_t pipeline, uint64_t texture)
{
    return pipeline * 31 + texture;
}

} // namespace

namespace velk::ui {

void BatchBuilder::rebuild_commands(IElement* element, IGpuResourceObserver* observer,
                                    IRenderContext* render_ctx)
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

    rect local_rect = {0, 0, state->size.width, state->size.height};

    for (size_t i = 0; i < storage->attachment_count(); ++i) {
        auto att = storage->get_attachment(i);
        auto* visual = interface_cast<IVisual>(att);
        if (!visual) {
            continue;
        }

        // Ensure the visual's built-in pipeline is compiled. Empty shader
        // sources on the visual fall back to the renderer's registered
        // defaults (see IRenderContext::compile_pipeline). Visuals that
        // don't contribute their own shader (TextureVisual, cube/sphere)
        // simply don't implement IRasterShader.
        if (render_ctx) {
            if (auto* rs = interface_cast<IRasterShader>(visual)) {
                uint64_t key = rs->get_raster_pipeline_key();
                if (key != 0 && render_ctx->pipeline_map().find(key) == render_ctx->pipeline_map().end()) {
                    auto src = rs->get_raster_source(IRasterShader::Target::Forward);
                    render_ctx->compile_pipeline(src.fragment, src.vertex, key);
                }
            }
        }

        VisualCommands vc;
        {
            VELK_PERF_SCOPE("renderer.get_draw_entries");
            vc.entries = visual->get_draw_entries(local_rect);
        }

        // Capture an optional per-visual `velk_visual_discard` snippet.
        // Deferred gbuffer fragments call this at the top of main(); the
        // composer in build_gbuffer_draw_calls splices the snippet in.
        // The key perturbation is folded from the visual's class uid so
        // two visuals sharing a material still get distinct pipelines.
        if (auto* snippet = interface_cast<IShaderSnippet>(visual)) {
            if (!snippet->get_snippet_source().empty()) {
                vc.visual_discard = interface_pointer_cast<IShaderSnippet>(att);
                if (auto* obj = interface_cast<IObject>(visual)) {
                    Uid uid = obj->get_class_uid();
                    vc.discard_key_perturb = uid.lo ^ uid.hi;
                }
            }
        }

        auto vstate = read_state<IVisual>(visual);

        {
            VELK_PERF_SCOPE("renderer.resolve_material");
            if (render_ctx && vstate && vstate->paint) {
                auto prog = vstate->paint.get<IProgram>();
                if (prog) {
                    // Materials with an IMaterial eval body get their
                    // forward pipeline composed from the shared driver
                    // template + the material's eval_src + vertex_src.
                    // The compiled handle is stored on the material via
                    // set_pipeline_handle so subsequent get_pipeline_handle
                    // calls short-circuit.
                    if (auto* mat = interface_cast<IMaterial>(prog.get());
                        mat && prog->get_pipeline_handle(*render_ctx) == 0) {
                        auto eval_src = mat->get_eval_src();
                        auto vertex_src = mat->get_vertex_src();
                        auto eval_fn = mat->get_eval_fn_name();
                        if (!eval_src.empty() && !vertex_src.empty() && !eval_fn.empty()) {
                            mat->register_eval_includes(*render_ctx);
                            string frag = compose_eval_fragment(
                                forward_fragment_driver_template, eval_src, eval_fn,
                                mat->get_forward_discard_threshold());
                            uint64_t h = render_ctx->compile_pipeline(
                                string_view(frag), vertex_src);
                            if (h) {
                                prog->set_pipeline_handle(h);
                            }
                        }
                    }
                    uint64_t handle = prog->get_pipeline_handle(*render_ctx);
                    if (handle) {
                        vc.pipeline_override = handle;
                        vc.material = std::move(prog);
                    }
                }
            }
        }

        for (auto& res : visual->get_gpu_resources()) {
            if (!res) {
                continue;
            }
            if (observer) {
                res->add_gpu_resource_observer(observer);
            }
            cache.gpu_resources.push_back(IBuffer::WeakPtr(res));
        }

        VisualPhase phase = vstate ? vstate->visual_phase : VisualPhase::BeforeChildren;
        if (phase == VisualPhase::AfterChildren) {
            cache.after_visuals.push_back(std::move(vc));
        } else {
            cache.before_visuals.push_back(std::move(vc));
        }
    }
}

void BatchBuilder::rebuild_batches(const SceneState& state, vector<Batch>& out_batches)
{
    VELK_PERF_SCOPE("renderer.rebuild_batches");
    out_batches.clear();
    // Note: render_target_passes_ is NOT cleared here — it's cleared
    // once per frame in reset_frame_state (called at Renderer::prepare
    // start). Each view's rebuild_batches appends; find_or_make_pass
    // dedups so an RTT element shared between views only emits once
    // per frame from build_shared_passes.

    auto resolve_texture = [](const IProgram::Ptr& material, uint64_t fallback) -> uint64_t {
        return material ? reinterpret_cast<uintptr_t>(material.get()) : fallback;
    };

    auto get_visuals = [](const ElementCache& cache, VisualPhase phase) -> const vector<VisualCommands>& {
        return phase == VisualPhase::AfterChildren ? cache.after_visuals : cache.before_visuals;
    };

    auto emit_visuals = [&](const vector<IElement*>& entries, VisualPhase phase,
                            vector<Batch>& target_batches,
                            float offset_x = 0.f, float offset_y = 0.f) {
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
                for (auto& de : vc.entries) {
                    uint64_t pipeline = (vc.pipeline_override != 0) ? vc.pipeline_override : de.pipeline_key;
                    uint64_t texture = de.texture_key;

                    uint64_t bkey = make_batch_key(pipeline, resolve_texture(vc.material, texture));

                    if (target_batches.empty() || bkey != last_bkey) {
                        Batch batch;
                        batch.pipeline_key = pipeline;
                        batch.texture_key = texture;
                        batch.instance_stride = de.instance_size;
                        batch.material = vc.material;
                        batch.visual_discard = vc.visual_discard;
                        batch.discard_key_perturb = vc.discard_key_perturb;
                        target_batches.push_back(std::move(batch));
                        last_bkey = bkey;
                    }

                    auto& batch = target_batches.back();

                    auto data_offset = batch.instance_data.size();
                    batch.instance_data.resize(data_offset + de.instance_size);
                    std::memcpy(batch.instance_data.data() + data_offset, de.instance_data,
                                de.instance_size);

                    // Overwrite the leading mat4 world_matrix slot. The
                    // visual leaves it zero-initialised; we fill it per
                    // instance so shaders get the correct transform.
                    std::memcpy(batch.instance_data.data() + data_offset, world.m, sizeof(world.m));

                    batch.instance_count++;
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
                       RenderTargetPassData*)> walk;
    walk = [&](const IObject::Ptr& obj,
               vector<IElement*>& ambient_before,
               vector<IElement*>& ambient_after,
               RenderTargetPassData* active_pass) {
        auto elem_ptr = interface_pointer_cast<IElement>(obj);
        if (!elem_ptr) return;
        auto* elem = elem_ptr.get();

        RenderTargetPassData* inner_pass = active_pass;
        if (elem->has_render_traits()) {
            inner_pass = find_or_make_pass(elem);
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
                walk(kid, ambient_before, ambient_after, inner_pass);
            }
        }

        ambient_after.push_back(elem);
        if (inner_pass) inner_pass->after_entries.push_back(elem);
    };

    if (scene) {
        walk(scene->root(), main_before, main_after, nullptr);
    }

    // Batch render target passes (offset by the element's world position)
    for (auto& rtp : render_target_passes_) {
        auto es = read_state<IElement>(rtp.element);
        float ox = es ? es->world_matrix(0, 3) : 0.f;
        float oy = es ? es->world_matrix(1, 3) : 0.f;
        emit_visuals(rtp.before_entries, VisualPhase::BeforeChildren, rtp.batches, ox, oy);
        emit_visuals(rtp.after_entries, VisualPhase::AfterChildren, rtp.batches, ox, oy);
    }

    emit_visuals(main_before, VisualPhase::BeforeChildren, out_batches);
    emit_visuals(main_after, VisualPhase::AfterChildren, out_batches);
}

void BatchBuilder::build_draw_calls(const vector<Batch>& batches, vector<DrawCall>& out_calls,
                                    FrameDataManager& frame_data, GpuResourceManager& resources,
                                    uint64_t globals_gpu_addr,
                                    const std::unordered_map<uint64_t, PipelineId>* pipeline_map,
                                    IRenderContext* render_ctx,
                                    IGpuResourceObserver* observer)
{
    VELK_PERF_SCOPE("renderer.build_draw_calls");

    for (auto& batch : batches) {
        uint64_t instances_addr =
            frame_data.write(batch.instance_data.data(), batch.instance_data.size());
        if (!instances_addr) {
            continue;
        }

        uint32_t texture_id = 0;
        if (batch.texture_key != 0) {
            auto* tex = reinterpret_cast<ISurface*>(batch.texture_key);
            texture_id = resources.find_texture(tex);
            if (texture_id == 0) {
                uint64_t rt_id = get_render_target_id(tex);
                if (rt_id != 0) {
                    texture_id = static_cast<uint32_t>(rt_id);
                }
            }
        }

        DrawDataHeader header{};
        header.globals_address = globals_gpu_addr;
        header.instances_address = instances_addr;
        header.texture_id = texture_id;
        header.instance_count = batch.instance_count;

        uint64_t material_addr = write_material_once(batch.material.get(), frame_data);

        constexpr size_t kMaterialPtrSize = sizeof(uint64_t);
        size_t total_size = sizeof(DrawDataHeader) + kMaterialPtrSize;

        auto reservation = frame_data.reserve(total_size);
        if (!reservation.ptr) {
            continue;
        }

        auto* dst = static_cast<uint8_t*>(reservation.ptr);
        uint64_t draw_data_addr = reservation.gpu_addr;

        std::memcpy(dst, &header, sizeof(header));
        std::memcpy(dst + sizeof(DrawDataHeader), &material_addr, kMaterialPtrSize);

        if (!pipeline_map) {
            continue;
        }
        uint64_t effective_pipeline_key = batch.pipeline_key;
        if (effective_pipeline_key == 0 && batch.material && render_ctx) {
            effective_pipeline_key = batch.material->get_pipeline_handle(*render_ctx);
        }
        auto pit = pipeline_map->find(effective_pipeline_key);
        if (pit == pipeline_map->end()) {
            continue;
        }

        // Lazy-register the program's pipeline for deferred destruction on
        // program destruction. Idempotent; subscribes observer only once.
        if (batch.material) {
            if (resources.register_pipeline(batch.material.get(), pit->second) && observer) {
                batch.material->add_gpu_resource_observer(observer);
            }
        }

        DrawCall call{};
        call.pipeline = pit->second;
        call.vertex_count = 4;
        call.instance_count = batch.instance_count;
        call.root_constants_size = sizeof(uint64_t);
        std::memcpy(call.root_constants, &draw_data_addr, sizeof(uint64_t));

        out_calls.push_back(call);
    }
}

void BatchBuilder::build_gbuffer_draw_calls(const vector<Batch>& batches,
                                            vector<DrawCall>& out_calls,
                                            FrameDataManager& frame_data,
                                            GpuResourceManager& resources,
                                            uint64_t globals_gpu_addr,
                                            IRenderContext* render_ctx,
                                            RenderTargetGroup target_group,
                                            IGpuResourceObserver* observer)
{
    VELK_PERF_SCOPE("renderer.build_gbuffer_draw_calls");

    if (!render_ctx || target_group == 0) {
        return;
    }

    for (auto& batch : batches) {
        uint64_t instances_addr =
            frame_data.write(batch.instance_data.data(), batch.instance_data.size());
        if (!instances_addr) {
            continue;
        }

        uint32_t texture_id = 0;
        if (batch.texture_key != 0) {
            auto* tex = reinterpret_cast<ISurface*>(batch.texture_key);
            texture_id = resources.find_texture(tex);
            if (texture_id == 0) {
                uint64_t rt_id = get_render_target_id(tex);
                if (rt_id != 0) {
                    texture_id = static_cast<uint32_t>(rt_id);
                }
            }
        }

        DrawDataHeader header{};
        header.globals_address = globals_gpu_addr;
        header.instances_address = instances_addr;
        header.texture_id = texture_id;
        header.instance_count = batch.instance_count;

        uint64_t material_addr = write_material_once(batch.material.get(), frame_data);

        constexpr size_t kMaterialPtrSize = sizeof(uint64_t);
        size_t total_size = sizeof(DrawDataHeader) + kMaterialPtrSize;

        auto reservation = frame_data.reserve(total_size);
        if (!reservation.ptr) {
            continue;
        }
        auto* dst = static_cast<uint8_t*>(reservation.ptr);
        uint64_t draw_data_addr = reservation.gpu_addr;
        std::memcpy(dst, &header, sizeof(header));
        std::memcpy(dst + sizeof(DrawDataHeader), &material_addr, kMaterialPtrSize);

        // Resolve the G-buffer pipeline variant. Base key is the forward
        // pipeline key (stable hash on visual class / material); perturb
        // by the visual's discard-snippet class so two visuals sharing
        // the same material still get distinct gbuffer pipelines with
        // the right `velk_visual_discard` spliced in.
        uint64_t forward_key = batch.pipeline_key;
        if (forward_key == 0 && batch.material) {
            forward_key = batch.material->get_pipeline_handle(*render_ctx);
        }
        if (forward_key == 0) {
            continue;
        }
        uint64_t gbuffer_key = forward_key ^ batch.discard_key_perturb;

        PipelineId gpid = 0;
        auto& gmap = render_ctx->gbuffer_pipeline_map();
        auto git = gmap.find(gbuffer_key);
        if (git != gmap.end()) {
            gpid = git->second;
        } else {
            string_view vsrc;
            string_view base_fsrc;
            string composed_fsrc;

            // Materials provide a velk_eval_<name> body; the deferred
            // driver + material eval_src + vertex_src compose into the
            // G-buffer fragment below. Materials without eval_src fall
            // through to `default_gbuffer_fragment_src` (flat unlit
            // albedo).
            if (batch.material) {
                if (auto* mat = interface_cast<IMaterial>(batch.material.get());
                    mat && !mat->get_eval_src().empty()
                    && !mat->get_vertex_src().empty()
                    && !mat->get_eval_fn_name().empty()) {
                    mat->register_eval_includes(*render_ctx);
                    composed_fsrc = compose_eval_fragment(
                        deferred_fragment_driver_template,
                        mat->get_eval_src(),
                        mat->get_eval_fn_name(),
                        mat->get_deferred_discard_threshold());
                    vsrc = mat->get_vertex_src();
                    base_fsrc = string_view(composed_fsrc);
                }
            }
            if (base_fsrc.empty()) {
                base_fsrc = default_gbuffer_fragment_src;
            }

            // Compose fragment = material's (or default) source + the
            // visual's discard snippet body, OR an empty stub when the
            // visual opts out. Every deferred fragment forward-declares
            // `void velk_visual_discard()` and calls it at the top of
            // main; the composer supplies the definition here.
            string composed;
            composed.append(base_fsrc);
            composed.append(string_view("\n", 1));
            if (batch.visual_discard) {
                composed.append(batch.visual_discard->get_snippet_source());
            } else {
                composed.append(string_view("void velk_visual_discard() {}\n", 30));
            }

            gpid = render_ctx->compile_gbuffer_pipeline(
                string_view(composed), vsrc, gbuffer_key, target_group);
        }
        if (gpid == 0) {
            continue;
        }

        (void)resources; // reserved for future per-resource registration
        (void)observer;

        DrawCall call{};
        call.pipeline = gpid;
        call.vertex_count = 4;
        call.instance_count = batch.instance_count;
        call.root_constants_size = sizeof(uint64_t);
        std::memcpy(call.root_constants, &draw_data_addr, sizeof(uint64_t));

        out_calls.push_back(call);
    }
}

uint64_t BatchBuilder::write_material_once(IProgram* prog, FrameDataManager& frame_data)
{
    if (!prog) return 0;
    auto it = frame_material_addrs_.find(prog);
    if (it != frame_material_addrs_.end()) return it->second;

    uint64_t addr = 0;
    if (auto* dd = interface_cast<IDrawData>(prog)) {
        size_t sz = dd->get_draw_data_size();
        if (sz > 0 && (sz % 16) != 0) {
            VELK_LOG(E,
                     "Renderer: material get_draw_data_size (%zu) is not 16-byte aligned. "
                     "Use VELK_GPU_STRUCT for your material data.",
                     sz);
        }
        if (sz > 0) {
            void* scratch = std::malloc(sz);
            if (scratch) {
                std::memset(scratch, 0, sz);
                if (dd->write_draw_data(scratch, sz) == ReturnValue::Success) {
                    addr = frame_data.write(scratch, sz);
                }
                std::free(scratch);
            }
        }
    }
    frame_material_addrs_[prog] = addr;
    return addr;
}

} // namespace velk::ui
