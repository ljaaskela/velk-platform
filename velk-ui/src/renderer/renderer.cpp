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
    uint glyph_index;
    uint _pad0;
    uint _pad1;
    uint _pad2;
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

    // Compile built-in UI pipelines under well-known keys. Text is no
    // longer registered here: the text plugin owns its rendering and
    // creates the analytic-Bezier text pipeline lazily via TextMaterial.
    if (pipeline_map_->find(PipelineKey::Rect) == pipeline_map_->end()) {
        ctx->compile_pipeline(rect_fragment_src, rect_vertex_src, PipelineKey::Rect);
        ctx->compile_pipeline(rounded_rect_fragment_src, rounded_rect_vertex_src, PipelineKey::RoundedRect);
    }

    frame_buffer_size_ = kInitialFrameBufferSize;
    for (auto& slot : frame_slots_) {
        init_slot_buffers(slot);
    }
}

void Renderer::add_view(const IElement::Ptr& camera_element, const ISurface::Ptr& surface,
                        const rect& viewport)
{
    if (!(camera_element && surface)) {
        VELK_LOG(E, "Renderer::add_view: camera_element and surface must be valid");
        return;
    }

    // Reuse existing surface_id if another view already uses this surface
    uint64_t sid = 0;
    for (auto& v : views_) {
        if (v.surface == surface) {
            sid = v.surface_id;
            break;
        }
    }
    if (sid == 0 && backend_) {
        auto state = read_state<ISurface>(surface);
        if (state) {
            SurfaceDesc desc{};
            desc.width = state->width;
            desc.height = state->height;
            sid = backend_->create_surface(desc);
        }
    }
    views_.push_back({camera_element, surface, viewport, sid});
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

        // Collect GPU resources used by the visual (textures, plain buffers,
        // etc). We observe each so we can defer-destroy its GPU handle when
        // its last strong reference drops (on any thread).
        for (auto& res : visual->get_gpu_resources()) {
            if (!res) {
                continue;
            }
            res->add_gpu_resource_observer(this);
            cache.gpu_resources.push_back(IBuffer::WeakPtr(res));
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

        // Resolve bindless texture index from the texture key (= ITexture pointer)
        uint32_t texture_id = 0;
        if (batch.texture_key != 0) {
            auto* tex = reinterpret_cast<ITexture*>(batch.texture_key);
            auto tit = texture_map_.find(tex);
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

    // Drain any GPU resources whose safe window has elapsed.
    drain_deferred_destroy();

    // Claim a frame slot that is not ready (awaiting prepare) and whose GPU buffer
    // is safe to reuse (enough frames have elapsed since it was last presented).
    FrameSlot* slot = nullptr;
    {
        auto is_slot_free = [&](const FrameSlot& s) {
            return !s.ready && (s.presented_at == 0 ||
                                present_counter_ - s.presented_at >= kGpuLatencyFrames);
        };

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
    slot->surface_submits.clear();
    active_slot_ = slot;

    // Prepare the staging buffer once for all views
    ensure_frame_buffer_capacity();
    write_offset_ = 0;

    // Consume scene state once per unique scene and process changes
    std::unordered_map<IScene*, SceneState> consumed_scenes;
    for (auto& entry : views_) {
        if (!view_matches(entry, desc)) {
            continue;
        }

        auto scene_ptr = entry.camera_element->get_scene();
        auto* scene = interface_cast<IScene>(scene_ptr);
        if (!scene || consumed_scenes.count(scene)) {
            continue;
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

        // Upload dirty GPU resources (e.g. glyph atlas updates, decoded
        // images, font curve buffers). Each resource is an IBuffer; if it
        // is also an ITexture we take the image upload path, otherwise we
        // take the plain-buffer path.
        bool resources_uploaded = false;
        if (has_changes) {
            for (auto& [elem, cache] : element_cache_) {
                for (auto& weak : cache.gpu_resources) {
                    // Promote to a temporary strong ref so the resource
                    // cannot die between is_dirty() and upload.
                    auto buf_ptr = weak.lock();
                    auto* buf = buf_ptr.get();
                    if (!buf || !buf->is_dirty()) {
                        continue;
                    }

                    if (auto* tex = interface_cast<ITexture>(buf)) {
                        // Texture path: bindless image upload via the
                        // backend's upload_texture.
                        int tw = tex->width();
                        int th = tex->height();
                        const uint8_t* pixels = tex->get_data();
                        if (pixels && tw > 0 && th > 0) {
                            auto tit = texture_map_.find(tex);
                            if (tit == texture_map_.end()) {
                                TextureDesc desc{};
                                desc.width = tw;
                                desc.height = th;
                                desc.format = tex->format();
                                TextureId tid = backend_->create_texture(desc);
                                texture_map_[tex] = tid;
                                tit = texture_map_.find(tex);
                            }

                            backend_->upload_texture(tit->second, pixels, tw, th);
                            tex->clear_dirty();
                            resources_uploaded = true;
                        }
                    } else {
                        // Plain buffer path: cpu-writable GpuBuffer + memcpy.
                        // The backend hands out a persistently-mapped pointer
                        // and a stable GPU virtual address that the shader
                        // reaches via buffer_reference.
                        size_t bsize = buf->get_size();
                        const uint8_t* bytes = buf->get_data();
                        if (!bytes || bsize == 0) {
                            continue;
                        }

                        auto bit = buffer_map_.find(buf);
                        bool need_alloc = (bit == buffer_map_.end());
                        if (!need_alloc && bit->second.size != bsize) {
                            // Size changed: defer-destroy the old buffer and
                            // reallocate. The new GPU address will be picked
                            // up by the next frame's draw call assembly.
                            {
                                std::lock_guard<std::mutex> lock(deferred_destroy_mutex_);
                                deferred_buffer_destroy_.push_back(
                                    {bit->second.handle, present_counter_ + kGpuLatencyFrames});
                            }
                            buffer_map_.erase(bit);
                            need_alloc = true;
                        }

                        if (need_alloc) {
                            GpuBufferDesc desc{};
                            desc.size = bsize;
                            desc.cpu_writable = true;
                            BufferEntry entry{};
                            entry.handle = backend_->create_buffer(desc);
                            entry.size = bsize;
                            buffer_map_[buf] = entry;
                            bit = buffer_map_.find(buf);
                            // Publish the GPU address back to the buffer so
                            // materials reading it via get_gpu_address() see
                            // the new value (this also handles regrow, since
                            // the previous entry was just erased).
                            buf->set_gpu_address(backend_->gpu_address(entry.handle));
                        }

                        if (auto* dst = backend_->map(bit->second.handle)) {
                            std::memcpy(dst, bytes, bsize);
                        }
                        buf->clear_dirty();
                        resources_uploaded = true;
                    }
                }
            }
        }

        if (has_changes || resources_uploaded) {
            // Mark all views sharing this scene as batches dirty
            for (auto& v : views_) {
                auto sp = v.camera_element->get_scene();
                if (interface_cast<IScene>(sp) == scene) {
                    v.batches_dirty = true;
                }
            }
        }

        consumed_scenes[scene] = std::move(state);
    }

    // Build draw calls per view
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

        // Find the camera trait on the element
        ICamera* camera = nullptr;
        if (auto* storage = interface_cast<IObjectStorage>(entry.camera_element)) {
            for (size_t i = 0; i < storage->attachment_count(); ++i) {
                camera = interface_cast<ICamera>(storage->get_attachment(i));
                if (camera) break;
            }
        }

        if (entry.batches_dirty) {
            rebuild_batches(sit->second, entry);
            entry.batches_dirty = false;
        }

        // Resolve effective viewport dimensions
        auto sstate = read_state<ISurface>(entry.surface);
        // Resolve normalized viewport (0..1) to pixel coordinates
        float sw = static_cast<float>(sstate ? sstate->width : 0);
        float sh = static_cast<float>(sstate ? sstate->height : 0);
        bool has_viewport = entry.viewport.width > 0 && entry.viewport.height > 0;
        float vp_w = has_viewport ? entry.viewport.width * sw : sw;
        float vp_h = has_viewport ? entry.viewport.height * sh : sh;

        // Write per-view globals into the staging buffer
        if (vp_w > 0 && vp_h > 0) {
            FrameGlobals globals{};
            if (camera) {
                auto vp = camera->get_view_projection(*entry.camera_element, vp_w, vp_h);
                std::memcpy(globals.view_projection, vp.m, sizeof(vp.m));
            } else {
                build_ortho_projection(globals.view_projection, vp_w, vp_h);
            }
            globals.viewport[0] = vp_w;
            globals.viewport[1] = vp_h;
            globals.viewport[2] = 1.0f / vp_w;
            globals.viewport[3] = 1.0f / vp_h;
            globals_gpu_addr_ = write_to_frame_buffer(&globals, sizeof(globals));
        }

        // Write all batches to the staging buffer and produce DrawCall structs
        build_draw_calls();

        // Capture draw calls for this surface into the frame slot
        SurfaceSubmit submit;
        submit.surface_id = entry.surface_id;
        float vp_x = has_viewport ? entry.viewport.x * sw : 0;
        float vp_y = has_viewport ? entry.viewport.y * sh : 0;
        submit.viewport = {vp_x, vp_y, vp_w, vp_h};
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
        // If all submits were removed, mark slot as not ready but preserve presented_at
        // so the buffer isn't reused before the GPU is done with it
        if (s.surface_submits.empty()) {
            s.ready = false;
            s.presented_at = present_counter_ + 1;
        }
    }

    // Present the frame, grouping submits by surface
    uint64_t current_surface = 0;
    for (size_t i = 0; i < target->surface_submits.size(); ++i) {
        auto& submit = target->surface_submits[i];

        if (submit.surface_id != current_surface) {
            if (current_surface != 0) {
                VELK_PERF_SCOPE("renderer.end_frame");
                backend_->end_frame();
            }
            current_surface = submit.surface_id;
            VELK_PERF_SCOPE("renderer.begin_frame");
            backend_->begin_frame(submit.surface_id);
        }

        RENDER_LOG("present: submitting %zu draw calls to surface %llu",
                   submit.draw_calls.size(), submit.surface_id);
        {
            VELK_PERF_SCOPE("renderer.submit");
            backend_->submit({submit.draw_calls.data(), submit.draw_calls.size()}, submit.viewport);
        }
    }
    if (current_surface != 0) {
        VELK_PERF_SCOPE("renderer.end_frame");
        backend_->end_frame();
    }
    present_counter_++;
    target->ready = false;
    target->presented_at = present_counter_;
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

void Renderer::on_gpu_resource_destroyed(IGpuResource* resource)
{
    // The notification gives us a base IGpuResource* and the underlying
    // object may be either an ITexture or a plain IBuffer. We don't know
    // which without a cross-cast, so just probe both maps; the cast back
    // to the concrete pointer types is safe because we keyed the maps by
    // those pointer values when the entries were created.
    std::lock_guard<std::mutex> lock(deferred_destroy_mutex_);

    if (auto* tex = interface_cast<ITexture>(resource)) {
        auto it = texture_map_.find(tex);
        if (it != texture_map_.end()) {
            deferred_destroy_.push_back({it->second, present_counter_ + kGpuLatencyFrames});
            texture_map_.erase(it);
            return;
        }
    }

    if (auto* buf = interface_cast<IBuffer>(resource)) {
        auto it = buffer_map_.find(buf);
        if (it != buffer_map_.end()) {
            deferred_buffer_destroy_.push_back(
                {it->second.handle, present_counter_ + kGpuLatencyFrames});
            buffer_map_.erase(it);
            return;
        }
    }
}

void Renderer::drain_deferred_destroy()
{
    if (!backend_) {
        return;
    }
    std::lock_guard<std::mutex> lock(deferred_destroy_mutex_);

    size_t kept = 0;
    for (size_t i = 0; i < deferred_destroy_.size(); ++i) {
        auto& d = deferred_destroy_[i];
        if (present_counter_ >= d.safe_after_frame) {
            backend_->destroy_texture(d.tid);
        } else {
            deferred_destroy_[kept++] = d;
        }
    }
    deferred_destroy_.resize(kept);

    kept = 0;
    for (size_t i = 0; i < deferred_buffer_destroy_.size(); ++i) {
        auto& d = deferred_buffer_destroy_[i];
        if (present_counter_ >= d.safe_after_frame) {
            backend_->destroy_buffer(d.handle);
        } else {
            deferred_buffer_destroy_[kept++] = d;
        }
    }
    deferred_buffer_destroy_.resize(kept);
}

void Renderer::shutdown()
{
    if (backend_) {
        // Detach from any GPU resources we are still observing so their
        // dtors (which may run later, on any thread) cannot reach a dead
        // renderer.
        for (auto& [elem, cache] : element_cache_) {
            for (auto& weak : cache.gpu_resources) {
                if (auto res = weak.lock()) {
                    res->remove_gpu_resource_observer(this);
                }
            }
        }

        // Drop any deferred destroys outright; the backend will be torn
        // down so all GPU resources die anyway.
        {
            std::lock_guard<std::mutex> lock(deferred_destroy_mutex_);
            deferred_destroy_.clear();
            deferred_buffer_destroy_.clear();
        }

        for (auto& entry : views_) {
            backend_->destroy_surface(entry.surface_id);
        }

        for (auto& [key, tid] : texture_map_) {
            backend_->destroy_texture(tid);
        }
        texture_map_.clear();

        for (auto& [key, entry] : buffer_map_) {
            backend_->destroy_buffer(entry.handle);
        }
        buffer_map_.clear();

        for (auto& slot : frame_slots_) {
            if (slot.frame_buffer) {
                backend_->destroy_buffer(slot.frame_buffer);
                slot.frame_buffer = 0;
            }
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
