#include "batch_builder.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>
#include <velk-render/gpu_data.h>
#include <velk-ui/interface/intf_visual.h>

#include <cstring>

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

        VisualCommands vc;
        {
            VELK_PERF_SCOPE("renderer.get_draw_entries");
            vc.entries = visual->get_draw_entries(local_rect);
        }

        auto vstate = read_state<IVisual>(visual);

        {
            VELK_PERF_SCOPE("renderer.resolve_material");
            if (render_ctx && vstate && vstate->paint) {
                auto prog = vstate->paint.get<IProgram>();
                if (prog) {
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
    render_target_passes_.clear();

    auto resolve_texture = [](const IProgram::Ptr& material, uint64_t fallback) -> uint64_t {
        return material ? reinterpret_cast<uintptr_t>(material.get()) : fallback;
    };

    auto get_visuals = [](const ElementCache& cache, VisualPhase phase) -> const vector<VisualCommands>& {
        return phase == VisualPhase::AfterChildren ? cache.after_visuals : cache.before_visuals;
    };

    auto emit_visuals = [&](const vector<VisualListEntry>& entries, VisualPhase phase,
                            vector<Batch>& target_batches,
                            float offset_x = 0.f, float offset_y = 0.f) {
        uint64_t last_bkey = 0;
        size_t max_visuals = 0;
        for (auto& ve : entries) {
            if (ve.type != VisualEntry::Element) {
                continue;
            }
            auto it = element_cache_.find(ve.element.get());
            if (it != element_cache_.end()) {
                max_visuals = std::max(max_visuals, get_visuals(it->second, phase).size());
            }
        }

        for (size_t pass = 0; pass < max_visuals; ++pass) {
            for (auto& ve : entries) {
                if (ve.type != VisualEntry::Element) {
                    continue;
                }

                auto* elem = ve.element.get();
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

                float wx = elem_state->world_matrix(0, 3) - offset_x;
                float wy = elem_state->world_matrix(1, 3) - offset_y;

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
                        target_batches.push_back(std::move(batch));
                        last_bkey = bkey;
                    }

                    auto& batch = target_batches.back();

                    auto data_offset = batch.instance_data.size();
                    batch.instance_data.resize(data_offset + de.instance_size);
                    std::memcpy(batch.instance_data.data() + data_offset, de.instance_data,
                                de.instance_size);

                    float* inst = reinterpret_cast<float*>(batch.instance_data.data() + data_offset);
                    inst[0] += wx;
                    inst[1] += wy;

                    batch.instance_count++;
                }
            }
        }
    };

    // Pre-filter: partition visual list entries into main and render target lists.
    auto filter_render_targets = [&](const vector<VisualListEntry>& entries,
                                     vector<VisualListEntry>& main_out,
                                     VisualPhase phase) {
        int depth = 0;
        IElement* active_rt_elem = nullptr;
        RenderTargetPassData* active_pass = nullptr;

        for (auto& ve : entries) {
            if (ve.type == VisualEntry::PushRenderTarget) {
                if (depth == 0) {
                    active_rt_elem = ve.element.get();
                    active_pass = nullptr;
                    for (auto& rtp : render_target_passes_) {
                        if (rtp.element == active_rt_elem) {
                            active_pass = &rtp;
                            break;
                        }
                    }
                    if (!active_pass) {
                        render_target_passes_.push_back({active_rt_elem, {}, {}, {}});
                        active_pass = &render_target_passes_.back();
                    }
                }
                depth++;
                continue;
            }

            if (ve.type == VisualEntry::PopRenderTarget) {
                depth--;
                if (depth == 0) {
                    active_rt_elem = nullptr;
                    active_pass = nullptr;
                }
                continue;
            }

            if (depth > 0 && active_pass) {
                auto& list = (phase == VisualPhase::BeforeChildren)
                                 ? active_pass->before_entries
                                 : active_pass->after_entries;
                list.push_back(ve);
            }
            main_out.push_back(ve);
        }
    };

    vector<VisualListEntry> main_before;
    vector<VisualListEntry> main_after;
    filter_render_targets(state.visual_list, main_before, VisualPhase::BeforeChildren);
    filter_render_targets(state.after_visual_list, main_after, VisualPhase::AfterChildren);

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

        size_t mat_size = batch.material ? batch.material->gpu_data_size() : 0;
        if (mat_size > 0 && (mat_size % 16) != 0) {
            VELK_LOG(E,
                     "Renderer: material gpu_data_size (%zu) is not 16-byte aligned. "
                     "Use VELK_GPU_STRUCT for your material data.",
                     mat_size);
        }
        size_t total_size = sizeof(DrawDataHeader) + mat_size;

        auto reservation = frame_data.reserve(total_size);
        if (!reservation.ptr) {
            continue;
        }

        auto* dst = static_cast<uint8_t*>(reservation.ptr);
        uint64_t draw_data_addr = reservation.gpu_addr;

        std::memcpy(dst, &header, sizeof(header));
        if (mat_size > 0) {
            if (failed(batch.material->write_gpu_data(dst + sizeof(DrawDataHeader), mat_size))) {
                VELK_LOG(E, "Renderer: material write_gpu_data failed");
                continue;
            }
        }

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

} // namespace velk::ui
