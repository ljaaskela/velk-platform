#include "renderer.h"
#include "render_context.h"

#include <velk/api/any.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_object_storage.h>
#include <velk-ui/interface/intf_visual.h>

#include <cstring>

#ifdef VELK_RENDER_DEBUG
#define RENDER_LOG(...) VELK_LOG(I, __VA_ARGS__)
#else
#define RENDER_LOG(...) ((void)0)
#endif

namespace velk_ui {

namespace {

uint64_t make_batch_key(uint64_t pipeline, uint64_t texture)
{
    return pipeline * 31 + texture;
}

void build_ortho_projection(float* out, float width, float height)
{
    std::memset(out, 0, 16 * sizeof(float));
    out[0]  =  2.0f / width;
    out[5]  =  2.0f / height;   // Vulkan Y is top-down, same as UI
    out[10] = -1.0f;
    out[12] = -1.0f;
    out[13] = -1.0f;
    out[15] =  1.0f;
}

} // namespace

void Renderer::set_backend(const IRenderBackend::Ptr& backend, IRenderContext* ctx)
{
    backend_ = backend;
    render_ctx_ = ctx;

    if (!backend_) return;

    // Reference the context's pipeline map (stays in sync as materials are added)
    auto* ctx_impl = static_cast<RenderContextImpl*>(ctx);
    if (ctx_impl) {
        pipeline_map_ = &ctx_impl->pipeline_map();
    }

    // Create double-buffered frame GPU buffers
    for (int i = 0; i < 2; ++i) {
        GpuBufferDesc desc;
        desc.size = kFrameBufferSize;
        desc.cpu_writable = true;
        frame_buffer_[i] = backend_->create_buffer(desc);
        frame_ptr_[i] = backend_->map(frame_buffer_[i]);
        frame_gpu_base_[i] = backend_->gpu_address(frame_buffer_[i]);
    }

    // Create globals buffer
    GpuBufferDesc globals_desc;
    globals_desc.size = sizeof(FrameGlobals);
    globals_desc.cpu_writable = true;
    globals_buffer_ = backend_->create_buffer(globals_desc);
    globals_ptr_ = static_cast<FrameGlobals*>(backend_->map(globals_buffer_));
    globals_gpu_addr_ = backend_->gpu_address(globals_buffer_);
}

void Renderer::attach(const ISurface::Ptr& surface, const IScene::Ptr& scene)
{
    if (!(surface && scene)) {
        VELK_LOG(E, "Renderer::attach: surface and scene must be valid objects");
        return;
    }

    for (auto& entry : surfaces_) {
        if (entry.surface == surface) {
            entry.scene = scene;
            return;
        }
    }

    auto state = velk::read_state<ISurface>(surface);
    uint64_t sid = 0;
    if (backend_ && state) {
        SurfaceDesc desc{};
        desc.width = state->width;
        desc.height = state->height;
        sid = backend_->create_surface(desc);
    }
    surfaces_.push_back({surface, scene, sid});
}

void Renderer::detach(const ISurface::Ptr& surface)
{
    for (auto it = surfaces_.begin(); it != surfaces_.end(); ++it) {
        if (it->surface == surface) {
            if (backend_) {
                backend_->destroy_surface(it->surface_id);
            }
            surfaces_.erase(it);
            return;
        }
    }
}

void Renderer::rebuild_commands(IElement* element)
{
    auto& cache = element_cache_[element];
    cache.visuals.clear();
    cache.texture_provider = nullptr;

    auto* storage = interface_cast<velk::IObjectStorage>(element);
    if (!storage) return;

    auto state = velk::read_state<IElement>(element);
    if (!state) return;

    velk::rect local_rect = {0, 0, state->size.width, state->size.height};

    for (size_t i = 0; i < storage->attachment_count(); ++i) {
        auto att = storage->get_attachment(i);

        auto* visual = interface_cast<IVisual>(att);
        if (visual) {
            VisualCommands vc;
            vc.entries = visual->get_draw_entries(local_rect);

            auto vstate = velk::read_state<IVisual>(visual);
            auto mat_obj = (vstate && vstate->paint) ? vstate->paint.get() : velk::IObject::Ptr{};
            auto* mat = mat_obj ? interface_cast<IMaterial>(mat_obj) : nullptr;
            if (mat && render_ctx_) {
                uint64_t handle = mat->get_pipeline_handle(*render_ctx_);
                if (handle != 0) {
                    vc.pipeline_override = handle;
                    vc.material = mat;
                }
            }

            cache.visuals.push_back(std::move(vc));
        }

        auto* tp = interface_cast<ITextureProvider>(att);
        if (tp) {
            cache.texture_provider = tp;
        }
    }
}

void Renderer::rebuild_batches(const SceneState& state, const SurfaceEntry& entry)
{
    batches_.clear();
    batch_index_.clear();

    for (auto* element : state.visual_list) {
        auto it = element_cache_.find(element);
        if (it == element_cache_.end()) continue;

        auto& cache = it->second;
        auto elem_state = velk::read_state<IElement>(element);
        if (!elem_state) continue;

        float wx = elem_state->world_matrix(0, 3);
        float wy = elem_state->world_matrix(1, 3);

        for (auto& vc : cache.visuals) {
            bool has_material = vc.material != nullptr;

            for (auto& de : vc.entries) {
                uint64_t pipeline = (vc.pipeline_override != 0) ? vc.pipeline_override : de.pipeline_key;
                uint64_t texture = de.texture_key;

                bool per_element = has_material || (pipeline >= PipelineKey::CustomBase)
                    || (pipeline == PipelineKey::RoundedRect);

                uint64_t bkey = per_element
                    ? make_batch_key(pipeline, reinterpret_cast<uintptr_t>(element))
                    : make_batch_key(pipeline, texture);

                auto bit = batch_index_.find(bkey);
                size_t batch_idx;
                if (bit != batch_index_.end()) {
                    batch_idx = bit->second;
                } else {
                    batch_idx = batches_.size();
                    batch_index_[bkey] = batch_idx;
                    Batch batch;
                    batch.pipeline_key = pipeline;
                    batch.texture_key = texture;
                    batch.instance_stride = de.instance_size;
                    batch.material = vc.material;
                    batches_.push_back(std::move(batch));
                }

                auto& batch = batches_[batch_idx];

                // Copy instance data and apply world offset to first two floats (x, y)
                auto data_offset = batch.instance_data.size();
                batch.instance_data.resize(data_offset + de.instance_size);
                std::memcpy(batch.instance_data.data() + data_offset, de.instance_data, de.instance_size);

                float* inst = reinterpret_cast<float*>(batch.instance_data.data() + data_offset);
                inst[0] += wx;
                inst[1] += wy;

                if (per_element && !batch.has_rect) {
                    float x = wx + de.bounds.x;
                    float y = wy + de.bounds.y;
                    batch.rect = {x, y, de.bounds.width, de.bounds.height};
                    batch.has_rect = true;
                }

                batch.instance_count++;
            }
        }
    }
}

uint64_t Renderer::write_to_frame_buffer(const void* data, size_t size, size_t alignment)
{
    // Align offset
    write_offset_ = (write_offset_ + alignment - 1) & ~(alignment - 1);

    if (write_offset_ + size > kFrameBufferSize) {
        VELK_LOG(E, "Renderer: frame buffer overflow (%zu + %zu > %zu)",
                 write_offset_, size, kFrameBufferSize);
        return 0;
    }

    auto* dst = static_cast<uint8_t*>(frame_ptr_[frame_index_]) + write_offset_;
    std::memcpy(dst, data, size);

    uint64_t gpu_addr = frame_gpu_base_[frame_index_] + write_offset_;
    write_offset_ += size;
    return gpu_addr;
}

void Renderer::build_draw_calls()
{
    draw_calls_.clear();

    for (auto& batch : batches_) {
        // Write instance data to frame buffer
        uint64_t instances_addr = write_to_frame_buffer(
            batch.instance_data.data(), batch.instance_data.size());
        if (!instances_addr) continue;

        // Resolve texture
        uint32_t texture_id = 0;
        if (batch.texture_key != 0) {
            auto tit = texture_map_.find(batch.texture_key);
            if (tit != texture_map_.end()) {
                texture_id = tit->second;
            }
        }

        // Build draw data header
        DrawDataHeader header{};
        header.globals_address = globals_gpu_addr_;
        header.instances_address = instances_addr;
        header.texture_id = texture_id;
        header.instance_count = batch.instance_count;

        // If material, append material GPU data after header
        size_t total_size = sizeof(DrawDataHeader);
        uint8_t draw_data[256]{};
        std::memcpy(draw_data, &header, sizeof(header));

        if (batch.material) {
            size_t mat_size = batch.material->get_gpu_data(
                draw_data + sizeof(DrawDataHeader),
                sizeof(draw_data) - sizeof(DrawDataHeader));
            total_size += mat_size;
        }

        uint64_t draw_data_addr = write_to_frame_buffer(draw_data, total_size);
        if (!draw_data_addr) continue;

        // Resolve pipeline
        if (!pipeline_map_) continue;
        auto pit = pipeline_map_->find(batch.pipeline_key);
        if (pit == pipeline_map_->end()) continue;

        DrawCall call{};
        call.pipeline = pit->second;
        call.vertex_count = 6;  // procedural quad (triangle list)
        call.instance_count = batch.instance_count;
        call.root_constants_size = sizeof(uint64_t);
        std::memcpy(call.root_constants, &draw_data_addr, sizeof(uint64_t));

        draw_calls_.push_back(call);
    }
}

void Renderer::render()
{
    if (!backend_) return;

    for (auto& entry : surfaces_) {
        auto* scene = interface_cast<IScene>(entry.scene);
        if (!scene) continue;

        auto sstate = velk::read_state<ISurface>(entry.surface);
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

        for (auto& removed : state.removed_list) {
            auto* elem = interface_cast<IElement>(removed);
            if (elem) element_cache_.erase(elem);
        }

        for (auto* element : state.redraw_list) {
            rebuild_commands(element);
        }

        // Upload dirty textures
        bool textures_uploaded = false;
        if (has_changes) {
            for (auto& [elem, cache] : element_cache_) {
                if (cache.texture_provider && cache.texture_provider->is_texture_dirty()) {
                    auto* tp = cache.texture_provider;
                    uint32_t tw = tp->get_texture_width();
                    uint32_t th = tp->get_texture_height();
                    const uint8_t* pixels = tp->get_pixels();
                    if (pixels && tw > 0 && th > 0) {
                        uint64_t tex_key = reinterpret_cast<uint64_t>(tp);

                        // Create texture if not yet registered
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

                        backend_->upload_texture(tit->second, pixels,
                                                 static_cast<int>(tw), static_cast<int>(th));
                        tp->clear_texture_dirty();
                        textures_uploaded = true;
                    }
                }
            }
        }

        if (has_changes || textures_uploaded) {
            entry.batches_dirty = true;
        }

        if (entry.batches_dirty) {
            rebuild_batches(state, entry);
            entry.batches_dirty = false;
        }

        // Reset frame buffer bump allocator
        write_offset_ = 0;

        // Update globals
        if (globals_ptr_ && sstate) {
            build_ortho_projection(globals_ptr_->projection,
                                   static_cast<float>(sstate->width),
                                   static_cast<float>(sstate->height));
            globals_ptr_->viewport[0] = static_cast<float>(sstate->width);
            globals_ptr_->viewport[1] = static_cast<float>(sstate->height);
            globals_ptr_->viewport[2] = 1.0f / static_cast<float>(sstate->width);
            globals_ptr_->viewport[3] = 1.0f / static_cast<float>(sstate->height);
        }

        // Build draw calls from batches
        build_draw_calls();

        RENDER_LOG("render: submitting %zu draw calls", draw_calls_.size());

        backend_->begin_frame(entry.surface_id);
        backend_->submit({draw_calls_.data(), draw_calls_.size()});
        backend_->end_frame();

        // Alternate frame buffer for next frame
        frame_index_ = 1 - frame_index_;
    }
}

void Renderer::shutdown()
{
    if (backend_) {
        for (auto& entry : surfaces_) {
            backend_->destroy_surface(entry.surface_id);
        }

        // Destroy textures
        for (auto& [key, tid] : texture_map_) {
            backend_->destroy_texture(tid);
        }
        texture_map_.clear();

        // Destroy GPU buffers
        for (int i = 0; i < 2; ++i) {
            if (frame_buffer_[i]) {
                backend_->destroy_buffer(frame_buffer_[i]);
                frame_buffer_[i] = 0;
            }
        }
        if (globals_buffer_) {
            backend_->destroy_buffer(globals_buffer_);
            globals_buffer_ = 0;
        }

        backend_ = nullptr;
    }

    surfaces_.clear();
    element_cache_.clear();
    batch_index_.clear();
    batches_.clear();
    draw_calls_.clear();
    pipeline_map_ = nullptr;
}

} // namespace velk_ui
