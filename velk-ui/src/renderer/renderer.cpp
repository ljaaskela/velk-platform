#include "renderer.h"

#include "default_ui_shaders.h"

#include <velk/api/any.h>
#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_object_storage.h>

#include <cstring>
#include <velk-render/interface/intf_material.h>
#include <velk-ui/interface/intf_camera.h>
#include <velk-ui/interface/intf_visual.h>

#ifdef VELK_RENDER_DEBUG
#define RENDER_LOG(...) VELK_LOG(I, __VA_ARGS__)
#else
#define RENDER_LOG(...) ((void)0)
#endif

namespace velk::ui {

namespace {

uint64_t make_batch_key(uint64_t pipeline, uint64_t texture)
{
    return pipeline * 31 + texture;
}

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

constexpr string_view velk_ui_glsl = R"(
struct RectInstance {
    vec2 pos;
    vec2 size;
    vec4 color;
};

struct TextInstance {
    vec2 pos;
    vec2 size;
    vec4 color;
    vec2 uv_min;
    vec2 uv_max;
};

layout(buffer_reference, std430) readonly buffer RectInstanceData {
    RectInstance data[];
};

layout(buffer_reference, std430) readonly buffer TextInstanceData {
    TextInstance data[];
};
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

    // Register default material shaders
    ctx->set_default_vertex_shader(ctx->compile_shader(material_vertex_src, ShaderStage::Vertex));
    ctx->set_default_fragment_shader(ctx->compile_shader(material_fragment_src, ShaderStage::Fragment));

    // Compile built-in UI pipelines under well-known keys
    if (pipeline_map_->find(PipelineKey::Rect) == pipeline_map_->end()) {
        ctx->compile_pipeline(rect_fragment_src, rect_vertex_src, PipelineKey::Rect);
        ctx->compile_pipeline(text_fragment_src, text_vertex_src, PipelineKey::Text);
        ctx->compile_pipeline(rounded_rect_fragment_src, rounded_rect_vertex_src, PipelineKey::RoundedRect);
    }

    frame_buffer_size_ = kInitialFrameBufferSize;
    for (auto& slot : frame_slots_) {
        init_slot_buffers(slot);
    }

    GpuBufferDesc globals_desc;
    globals_desc.size = sizeof(FrameGlobals);
    globals_desc.cpu_writable = true;
    globals_buffer_ = backend_->create_buffer(globals_desc);
    globals_ptr_ = static_cast<FrameGlobals*>(backend_->map(globals_buffer_));
    globals_gpu_addr_ = backend_->gpu_address(globals_buffer_);
}

void Renderer::add_view(const IElement::Ptr& camera_element, const ISurface::Ptr& surface)
{
    if (!(camera_element && surface)) {
        VELK_LOG(E, "Renderer::add_view: camera_element and surface must be valid");
        return;
    }

    auto state = read_state<ISurface>(surface);
    uint64_t sid = 0;
    if (backend_ && state) {
        SurfaceDesc desc{};
        desc.width = state->width;
        desc.height = state->height;
        sid = backend_->create_surface(desc);
    }
    views_.push_back({camera_element, surface, sid});
}

void Renderer::remove_view(const IElement::Ptr& camera_element, const ISurface::Ptr& surface)
{
    for (auto it = views_.begin(); it != views_.end(); ++it) {
        if (it->camera_element == camera_element && it->surface == surface) {
            if (backend_) {
                backend_->destroy_surface(it->surface_id);
            }
            views_.erase(it);
            return;
        }
    }
}

void Renderer::rebuild_commands(IElement* element)
{
    VELK_PERF_SCOPE("renderer.rebuild_commands");
    auto& cache = element_cache_[element];
    cache.before_visuals.clear();
    cache.after_visuals.clear();
    cache.texture_providers.clear();

    auto* storage = interface_cast<IObjectStorage>(element);
    if (!storage) {
        return;
    }

    auto state = read_state<IElement>(element);
    if (!state) {
        return;
    }

    rect local_rect = {0, 0, state->size.width, state->size.height};

    // Walk the element's attachments looking for visuals
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

        // If the visual has a paint (material), resolve its pipeline
        {
            VELK_PERF_SCOPE("renderer.resolve_material");
            if (render_ctx_ && vstate && vstate->paint) {
                auto mat = vstate->paint.get<IMaterial>();
                if (mat) {
                    uint64_t handle = mat->get_pipeline_handle(*render_ctx_);
                    if (handle) {
                        vc.pipeline_override = handle;
                        vc.material = std::move(mat);
                    }
                }
            }
        }

        // If the visual provides a texture (e.g. text visual with a font atlas)
        auto tp = visual->get_texture_provider();
        if (tp) {
            cache.texture_providers.push_back(std::move(tp));
        }

        // Sort into before/after based on visual phase
        VisualPhase phase = vstate ? vstate->visual_phase : VisualPhase::BeforeChildren;
        if (phase == VisualPhase::AfterChildren) {
            cache.after_visuals.push_back(std::move(vc));
        } else {
            cache.before_visuals.push_back(std::move(vc));
        }
    }
}

void Renderer::rebuild_batches(const SceneState& state, const ViewEntry& entry)
{
    VELK_PERF_SCOPE("renderer.rebuild_batches");
    batches_.clear();

    auto resolve_texture = [](const IMaterial::Ptr& material, uint64_t fallback) -> uint64_t {
        return material ? reinterpret_cast<uintptr_t>(material.get()) : fallback;
    };

    // Multi-pass batching over a visual list. Emits visual[0] from all elements,
    // then visual[1], etc. Consecutive entries with the same batch key are merged.
    //
    // Called twice: once for before-children visuals (pre-order list),
    // once for after-children visuals (post-order list).

    uint64_t last_bkey = 0;

    auto get_visuals = [](const ElementCache& cache, VisualPhase phase) -> const vector<VisualCommands>& {
        return phase == VisualPhase::AfterChildren ? cache.after_visuals : cache.before_visuals;
    };

    auto emit_visuals = [&](const vector<IElement::Ptr>& elements, VisualPhase phase) {
        size_t max_visuals = 0;
        for (auto& element : elements) {
            auto it = element_cache_.find(element.get());
            if (it != element_cache_.end()) {
                max_visuals = std::max(max_visuals, get_visuals(it->second, phase).size());
            }
        }

        for (size_t pass = 0; pass < max_visuals; ++pass) {
            for (auto& element : elements) {
                auto* elem = element.get();
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

                float wx = elem_state->world_matrix(0, 3);
                float wy = elem_state->world_matrix(1, 3);

                auto& vc = visuals[pass];
                for (auto& de : vc.entries) {
                    uint64_t pipeline = (vc.pipeline_override != 0) ? vc.pipeline_override : de.pipeline_key;
                    uint64_t texture = de.texture_key;

                    uint64_t bkey = make_batch_key(pipeline, resolve_texture(vc.material, texture));

                    if (batches_.empty() || bkey != last_bkey) {
                        Batch batch;
                        batch.pipeline_key = pipeline;
                        batch.texture_key = texture;
                        batch.instance_stride = de.instance_size;
                        batch.material = vc.material;
                        batches_.push_back(std::move(batch));
                        last_bkey = bkey;
                    }

                    auto& batch = batches_.back();

                    auto data_offset = batch.instance_data.size();
                    batch.instance_data.resize(data_offset + de.instance_size);
                    std::memcpy(batch.instance_data.data() + data_offset, de.instance_data, de.instance_size);

                    float* inst = reinterpret_cast<float*>(batch.instance_data.data() + data_offset);
                    inst[0] += wx;
                    inst[1] += wy;

                    batch.instance_count++;
                }
            }
        }
    };

    // Before-children visuals in pre-order (depth-first)
    emit_visuals(state.visual_list, VisualPhase::BeforeChildren);

    // After-children visuals in post-order
    emit_visuals(state.after_visual_list, VisualPhase::AfterChildren);
}

uint64_t Renderer::write_to_frame_buffer(const void* data, size_t size, size_t alignment)
{
    write_offset_ = (write_offset_ + alignment - 1) & ~(alignment - 1);

    if (write_offset_ + size > frame_buffer_size_) {
        VELK_LOG(E,
                 "Renderer: frame buffer overflow (%zu + %zu > %zu), will grow next frame",
                 write_offset_,
                 size,
                 frame_buffer_size_);
        return 0;
    }

    auto* dst = static_cast<uint8_t*>(active_slot_->frame_ptr) + write_offset_;
    std::memcpy(dst, data, size);

    uint64_t gpu_addr = active_slot_->frame_gpu_base + write_offset_;
    write_offset_ += size;

    if (write_offset_ > peak_usage_) {
        peak_usage_ = write_offset_;
    }

    return gpu_addr;
}

void Renderer::ensure_frame_buffer_capacity()
{
    if (peak_usage_ <= frame_buffer_size_ * 3 / 4) {
        return;
    }

    size_t new_size = frame_buffer_size_;
    while (new_size < peak_usage_ * 2) {
        new_size *= 2;
    }

    VELK_LOG(I,
             "Renderer: growing frame buffers %zu -> %zu KB (peak usage: %zu KB)",
             frame_buffer_size_ / 1024,
             new_size / 1024,
             peak_usage_ / 1024);

    for (auto& slot : frame_slots_) {
        if (slot.frame_buffer) {
            backend_->destroy_buffer(slot.frame_buffer);
        }
        GpuBufferDesc desc;
        desc.size = new_size;
        desc.cpu_writable = true;
        slot.frame_buffer = backend_->create_buffer(desc);
        slot.frame_ptr = backend_->map(slot.frame_buffer);
        slot.frame_gpu_base = backend_->gpu_address(slot.frame_buffer);
    }

    frame_buffer_size_ = new_size;
    peak_usage_ = 0;
}

void Renderer::init_slot_buffers(FrameSlot& slot)
{
    if (!backend_ || frame_buffer_size_ == 0) {
        return;
    }
    GpuBufferDesc desc;
    desc.size = frame_buffer_size_;
    desc.cpu_writable = true;
    slot.frame_buffer = backend_->create_buffer(desc);
    slot.frame_ptr = backend_->map(slot.frame_buffer);
    slot.frame_gpu_base = backend_->gpu_address(slot.frame_buffer);
}

void Renderer::build_draw_calls()
{
    VELK_PERF_SCOPE("renderer.build_draw_calls");
    draw_calls_.clear();

    for (auto& batch : batches_) {

        // Write instance data into the staging buffer
        uint64_t instances_addr =
            write_to_frame_buffer(batch.instance_data.data(), batch.instance_data.size());
        if (!instances_addr) {
            continue;
        }

        // Resolve bindless texture index from the texture key
        uint32_t texture_id = 0;
        if (batch.texture_key != 0) {
            auto tit = texture_map_.find(batch.texture_key);
            if (tit != texture_map_.end()) {
                texture_id = tit->second;
            }
        }

        // Build the DrawDataHeader (root struct the shader receives via push constant)
        DrawDataHeader header{};
        header.globals_address = globals_gpu_addr_;
        header.instances_address = instances_addr;
        header.texture_id = texture_id;
        header.instance_count = batch.instance_count;

        // Determine material data size (follows the header in the staging buffer)
        size_t mat_size = batch.material ? batch.material->gpu_data_size() : 0;
        if (mat_size > 0 && (mat_size % 16) != 0) {
            VELK_LOG(E,
                     "Renderer: material gpu_data_size (%zu) is not 16-byte aligned. "
                     "Use VELK_GPU_STRUCT for your material data.",
                     mat_size);
        }
        size_t total_size = sizeof(DrawDataHeader) + mat_size;

        // Align write offset and check capacity
        write_offset_ = (write_offset_ + 15) & ~size_t(15);
        if (write_offset_ + total_size > frame_buffer_size_) {
            continue;
        }

        auto* dst = static_cast<uint8_t*>(active_slot_->frame_ptr) + write_offset_;
        uint64_t draw_data_addr = active_slot_->frame_gpu_base + write_offset_;

        // Write header, then material params (if any) immediately after
        std::memcpy(dst, &header, sizeof(header));
        if (mat_size > 0) {
            if (failed(batch.material->write_gpu_data(dst + sizeof(DrawDataHeader), mat_size))) {
                VELK_LOG(E, "Renderer: material write_gpu_data failed");
                continue;
            }
        }

        write_offset_ += total_size;
        if (write_offset_ > peak_usage_) {
            peak_usage_ = write_offset_;
        }

        // Look up the backend pipeline handle from the pipeline key
        if (!pipeline_map_) {
            continue;
        }
        auto pit = pipeline_map_->find(batch.pipeline_key);
        if (pit == pipeline_map_->end()) {
            continue;
        }

        // Emit a DrawCall with the GPU address of the DrawDataHeader as root constant
        DrawCall call{};
        call.pipeline = pit->second;
        call.vertex_count = 4;
        call.instance_count = batch.instance_count;
        call.root_constants_size = sizeof(uint64_t);
        std::memcpy(call.root_constants, &draw_data_addr, sizeof(uint64_t));

        draw_calls_.push_back(call);
    }
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

Frame Renderer::prepare(const FrameDesc& desc)
{
    if (!backend_) {
        return {};
    }
    VELK_PERF_SCOPE("renderer.prepare");

    // Claim a frame slot
    FrameSlot* slot = nullptr;
    {
        std::unique_lock<std::mutex> lock(slot_mutex_);
        slot_cv_.wait(lock, [&] {
            for (auto& s : frame_slots_) {
                if (!s.ready) {
                    return true;
                }
            }
            return false;
        });
        for (auto& s : frame_slots_) {
            if (!s.ready) {
                slot = &s;
                break;
            }
        }
    }

    slot->id = next_frame_id_++;
    slot->surface_submits.clear();
    active_slot_ = slot;

    for (auto& entry : views_) {
        if (!view_matches(entry, desc)) {
            continue;
        }

        // Get the scene from the camera element
        auto scene_ptr = entry.camera_element->get_scene();
        auto* scene = interface_cast<IScene>(scene_ptr);
        if (!scene) {
            continue;
        }

        // Find the camera trait on the element
        ICamera* camera = nullptr;
        if (auto* storage = interface_cast<IObjectStorage>(entry.camera_element)) {
            for (size_t i = 0; i < storage->attachment_count(); ++i) {
                camera = interface_cast<ICamera>(storage->get_attachment(i));
                if (camera) break;
            }
        }

        // Handle surface resize
        auto sstate = read_state<ISurface>(entry.surface);
        if (sstate) {
            if (sstate->width != entry.cached_width || sstate->height != entry.cached_height) {
                entry.cached_width = sstate->width;
                entry.cached_height = sstate->height;
                backend_->resize_surface(entry.surface_id, sstate->width, sstate->height);
                entry.batches_dirty = true;
                RENDER_LOG("render: surface resized to %dx%d", sstate->width, sstate->height);
            }
        }

        // Consume scene changes since last frame
        auto state = scene->consume_state();

        bool has_changes = !state.redraw_list.empty() || !state.removed_list.empty();

        // Evict removed elements from the cache
        for (auto& removed : state.removed_list) {
            element_cache_.erase(removed.get());
        }

        // Rebuild draw commands for elements that changed
        for (auto* element : state.redraw_list) {
            rebuild_commands(element);
        }

        // Upload dirty textures (e.g. glyph atlas updates)
        bool textures_uploaded = false;
        if (has_changes) {
            for (auto& [elem, cache] : element_cache_) {
                for (auto& tp : cache.texture_providers) {
                    if (!tp || !tp->is_texture_dirty()) {
                        continue;
                    }
                    uint32_t tw = tp->get_texture_width();
                    uint32_t th = tp->get_texture_height();
                    const uint8_t* pixels = tp->get_pixels();
                    if (pixels && tw > 0 && th > 0) {
                        uint64_t tex_key = reinterpret_cast<uint64_t>(tp.get());

                        auto tit = texture_map_.find(tex_key);
                        if (tit == texture_map_.end()) {
                            TextureDesc desc{};
                            desc.width = static_cast<int>(tw);
                            desc.height = static_cast<int>(th);
                            desc.format = PixelFormat::R8;
                            TextureId tid = backend_->create_texture(desc);
                            texture_map_[tex_key] = tid;
                            tit = texture_map_.find(tex_key);
                        }

                        backend_->upload_texture(
                            tit->second, pixels, static_cast<int>(tw), static_cast<int>(th));
                        tp->clear_texture_dirty();
                        textures_uploaded = true;
                    }
                }
            }
        }

        // Rebuild batches if anything changed
        if (has_changes || textures_uploaded) {
            entry.batches_dirty = true;
        }

        if (entry.batches_dirty) {
            rebuild_batches(state, entry);
            entry.batches_dirty = false;
        }

        // Prepare the staging buffer for this frame
        ensure_frame_buffer_capacity();
        write_offset_ = 0;

        // Update frame globals from camera
        if (globals_ptr_ && sstate) {
            float w = static_cast<float>(sstate->width);
            float h = static_cast<float>(sstate->height);

            if (camera) {
                auto vp = camera->get_view_projection(*entry.camera_element, w, h);
                std::memcpy(globals_ptr_->view_projection, vp.m, sizeof(vp.m));
            } else {
                build_ortho_projection(globals_ptr_->view_projection, w, h);
            }

            globals_ptr_->viewport[0] = w;
            globals_ptr_->viewport[1] = h;
            globals_ptr_->viewport[2] = 1.0f / w;
            globals_ptr_->viewport[3] = 1.0f / h;
        }

        // Write all batches to the staging buffer and produce DrawCall structs
        build_draw_calls();

        // Capture draw calls for this surface into the frame slot
        SurfaceSubmit submit;
        submit.surface_id = entry.surface_id;
        submit.draw_calls = draw_calls_;
        slot->surface_submits.push_back(std::move(submit));
    }

    active_slot_ = nullptr;
    slot->ready = true;
    return Frame{slot->id};
}

void Renderer::present(Frame frame)
{
    if (!backend_ || frame.id == 0) {
        return;
    }
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
        // Remove surface submits that overlap with the frame being presented
        for (auto it = s.surface_submits.begin(); it != s.surface_submits.end();) {
            bool overlaps = false;
            for (auto& ts : target->surface_submits) {
                if (it->surface_id == ts.surface_id) {
                    overlaps = true;
                    break;
                }
            }
            if (overlaps) {
                it = s.surface_submits.erase(it);
            } else {
                ++it;
            }
        }
        // If all submits were removed, recycle the slot
        if (s.surface_submits.empty()) {
            s.ready = false;
        }
    }

    // Present the frame
    for (auto& submit : target->surface_submits) {
        RENDER_LOG("present: submitting %zu draw calls to surface %llu",
                   submit.draw_calls.size(), submit.surface_id);
        {
            VELK_PERF_SCOPE("renderer.begin_frame");
            backend_->begin_frame(submit.surface_id);
        }
        {
            VELK_PERF_SCOPE("renderer.submit");
            backend_->submit({submit.draw_calls.data(), submit.draw_calls.size()});
        }
        {
            VELK_PERF_SCOPE("renderer.end_frame");
            backend_->end_frame();
        }
    }
    target->ready = false;
    target->surface_submits.clear();

    slot_cv_.notify_one();
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
        init_slot_buffers(frame_slots_[i]);
    }
    slot_cv_.notify_all();
}

void Renderer::shutdown()
{
    if (backend_) {
        for (auto& entry : views_) {
            backend_->destroy_surface(entry.surface_id);
        }

        for (auto& [key, tid] : texture_map_) {
            backend_->destroy_texture(tid);
        }
        texture_map_.clear();

        for (auto& slot : frame_slots_) {
            if (slot.frame_buffer) {
                backend_->destroy_buffer(slot.frame_buffer);
                slot.frame_buffer = 0;
            }
        }
        if (globals_buffer_) {
            backend_->destroy_buffer(globals_buffer_);
            globals_buffer_ = 0;
        }

        backend_ = nullptr;
    }

    views_.clear();
    element_cache_.clear();
    batches_.clear();
    draw_calls_.clear();
    pipeline_map_ = nullptr;
}

} // namespace velk::ui
