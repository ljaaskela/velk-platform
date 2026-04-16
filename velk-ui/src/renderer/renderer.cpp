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
#include <velk-ui/interface/intf_environment.h>
#include <velk-ui/interface/intf_visual.h>

#ifdef VELK_RENDER_DEBUG
#define RENDER_LOG(...) VELK_LOG(I, __VA_ARGS__)
#else
#define RENDER_LOG(...) ((void)0)
#endif

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
                backend_->destroy_surface(get_render_target_id(it->surface));
            }
            views_.erase(it);
            return;
        }
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

        // Evict removed elements
        for (auto& removed : state.removed_list) {
            batch_builder_.evict(removed.get());
            auto rit = render_target_entries_.find(removed.get());
            if (rit != render_target_entries_.end()) {
                if (rit->second.texture_id != 0 && backend_) {
                    resources_.defer_texture_destroy(rit->second.texture_id,
                                                     present_counter_ + kGpuLatencyFrames);
                }
                render_target_entries_.erase(rit);
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

        if (has_changes || resources_uploaded) {
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

            ICamera* camera = nullptr;
            if (auto* storage = interface_cast<IObjectStorage>(entry.camera_element)) {
                camera = interface_cast<ICamera>(storage->find_attachment<ICamera>());
            }

            if (entry.batches_dirty) {
                batch_builder_.rebuild_batches(sit->second, entry.batches);
                if (camera) {
                    prepend_environment_batch(*camera, entry);
                }
                entry.batches_dirty = false;
            }

            auto sstate = read_state<IWindowSurface>(entry.surface);
            float sw = static_cast<float>(sstate ? sstate->size.x : 0);
            float sh = static_cast<float>(sstate ? sstate->size.y : 0);
            bool has_viewport = entry.viewport.width > 0 && entry.viewport.height > 0;
            float vp_w = has_viewport ? entry.viewport.width * sw : sw;
            float vp_h = has_viewport ? entry.viewport.height * sh : sh;

            if (vp_w > 0 && vp_h > 0) {
                FrameGlobals globals{};
                mat4 vp_mat;
                if (camera) {
                    vp_mat = camera->get_view_projection(*entry.camera_element, vp_w, vp_h);
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
                globals_gpu_addr_ = frame_buffer_.write(&globals, sizeof(globals));
            }

            draw_calls_.clear();
            batch_builder_.build_draw_calls(entry.batches,
                                            draw_calls_,
                                            frame_buffer_,
                                            resources_,
                                            globals_gpu_addr_,
                                            pipeline_map_,
                                            render_ctx_,
                                            this);

            // Main surface pass
            RenderPass pass;
            pass.target.target = interface_pointer_cast<IRenderTarget>(entry.surface);
            float vp_x = has_viewport ? entry.viewport.x * sw : 0;
            float vp_y = has_viewport ? entry.viewport.y * sh : 0;
            pass.viewport = {vp_x, vp_y, vp_w, vp_h};
            pass.draw_calls = draw_calls_;
            slot.passes.push_back(std::move(pass));
        }

        // Build render target passes once (not per view)
        for (auto& rtp : batch_builder_.render_target_passes()) {
            auto& rte = render_target_entries_[rtp.element];
            if (!rte.target) {
                if (auto* storage = interface_cast<IObjectStorage>(rtp.element)) {
                    if (auto rtt = storage->find_attachment<IRenderToTexture>()) {
                        auto rtt_state = read_state<IRenderToTexture>(rtt);
                        if (rtt_state) {
                            rte.target = rtt_state->render_target.get<IRenderTarget>();
                        }
                    }
                }
            }
            if (!rte.target) {
                continue;
            }
            int w{1}, h{1};
            if (auto es = read_state<IElement>(rtp.element)) {
                w = std::max(static_cast<int>(es->size.width), 1);
                h = std::max(static_cast<int>(es->size.height), 1);
            }
            if (rte.texture_id != 0 && (rte.width != w || rte.height != h)) {
                if (backend_) {
                    resources_.defer_texture_destroy(rte.texture_id, present_counter_ + kGpuLatencyFrames);
                }
                rte.texture_id = 0;
            }
            if (rte.texture_id == 0 && backend_) {
                TextureDesc tdesc{};
                tdesc.width = w;
                tdesc.height = h;
                tdesc.format = PixelFormat::RGBA8;
                tdesc.usage = TextureUsage::RenderTarget;
                rte.texture_id = backend_->create_texture(tdesc);
                rte.width = w;
                rte.height = h;
                rte.target->set_render_target_id(static_cast<uint64_t>(rte.texture_id));
            }

            if (rte.texture_id == 0) {
                continue;
            }

            FrameGlobals rt_globals{};
            build_ortho_projection(
                rt_globals.view_projection, static_cast<float>(rte.width), static_cast<float>(rte.height));
            rt_globals.viewport[0] = static_cast<float>(rte.width);
            rt_globals.viewport[1] = static_cast<float>(rte.height);
            rt_globals.viewport[2] = 1.0f / static_cast<float>(rte.width);
            rt_globals.viewport[3] = 1.0f / static_cast<float>(rte.height);
            uint64_t saved_globals = globals_gpu_addr_;
            globals_gpu_addr_ = frame_buffer_.write(&rt_globals, sizeof(rt_globals));

            vector<DrawCall> saved_calls;
            std::swap(draw_calls_, saved_calls);
            batch_builder_.build_draw_calls(rtp.batches,
                                            draw_calls_,
                                            frame_buffer_,
                                            resources_,
                                            globals_gpu_addr_,
                                            pipeline_map_,
                                            render_ctx_,
                                            this);

            // Insert render target pass before surface passes
            RenderPass rt_pass{};
            rt_pass.target.target = rte.target;
            rt_pass.viewport = {0, 0, static_cast<float>(rte.width), static_cast<float>(rte.height)};
            rt_pass.draw_calls = draw_calls_;
            slot.passes.insert(slot.passes.begin(), std::move(rt_pass));

            std::swap(draw_calls_, saved_calls);
            globals_gpu_addr_ = saved_globals;
            rte.dirty = false;
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

        for (auto& entry : views_) {
            backend_->destroy_surface(get_render_target_id(entry.surface));
        }

        for (auto& [key, rte] : render_target_entries_) {
            if (rte.texture_id != 0) {
                backend_->destroy_texture(rte.texture_id);
            }
        }
        render_target_entries_.clear();

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
    draw_calls_.clear();
    pipeline_map_ = nullptr;
}

void Renderer::prepend_environment_batch(ICamera& camera, ViewEntry& entry)
{
    if (!backend_) {
        return;
    }

    auto cam_state = read_state<ICamera>(&camera);
    if (!(cam_state && cam_state->environment)) {
        return;
    }
    auto env_ptr = cam_state->environment.get<IEnvironment>();
    if (!env_ptr) {
        return;
    }
    auto* surf = interface_cast<ISurface>(env_ptr);
    auto* buf = interface_cast<IBuffer>(env_ptr);
    if (!surf || !buf) {
        return;
    }

    // Upload the environment texture if dirty.
    if (buf->is_dirty()) {
        const uint8_t* pixels = buf->get_data();
        auto sz = surf->get_dimensions();
        int tw = static_cast<int>(sz.x);
        int th = static_cast<int>(sz.y);
        if (pixels && tw > 0 && th > 0) {
            TextureId tid = resources_.find_texture(surf);
            if (tid == 0) {
                TextureDesc desc{};
                desc.width = tw;
                desc.height = th;
                desc.format = surf->format();
                tid = backend_->create_texture(desc);
                resources_.register_texture(surf, tid);
                surf->add_gpu_resource_observer(this);
                auto buf_ptr = interface_pointer_cast<IBuffer>(env_ptr);
                if (buf_ptr) {
                    resources_.add_env_observer(buf_ptr);
                }
            }
            if (tid != 0) {
                backend_->upload_texture(tid, pixels, tw, th);
            }
            buf->clear_dirty();
        }
    }

    // Get the material from the environment (owned by the environment,
    // like Font owns TextMaterial).
    auto material = env_ptr->get_material();
    if (!material) {
        return;
    }

    // Insert a synthetic batch at the front of batches_ so the environment
    // renders before all scene geometry. The batch flows through the
    // normal build_draw_calls path. The env vertex shader generates a
    // fullscreen quad from vertex index; no instance data is needed, but
    // the batch must have instance_count = 1 to produce a draw call.
    BatchBuilder::Batch env_batch;
    env_batch.pipeline_key = 0; // material override supplies the pipeline
    env_batch.texture_key = reinterpret_cast<uint64_t>(surf);
    env_batch.instance_stride = 4;
    env_batch.instance_count = 1;
    env_batch.instance_data.resize(4, 0); // dummy, shader ignores it
    env_batch.material = std::move(material);

    entry.batches.insert(entry.batches.begin(), std::move(env_batch));
}

} // namespace velk::ui
