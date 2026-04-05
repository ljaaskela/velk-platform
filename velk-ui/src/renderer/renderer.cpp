#include "renderer.h"

#include "default_ui_shaders.h"

#include <velk/api/any.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_object_storage.h>

#include <cstring>
#include <velk-render/interface/intf_material.h>
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

layout(buffer_reference, std430) readonly buffer RectInstances {
    RectInstance data[];
};

layout(buffer_reference, std430) readonly buffer TextInstances {
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

    // Compile built-in UI pipelines under well-known keys
    if (pipeline_map_->find(PipelineKey::Rect) == pipeline_map_->end()) {
        ctx->compile_pipeline(rect_fragment_src, rect_vertex_src, PipelineKey::Rect);
        ctx->compile_pipeline(text_fragment_src, text_vertex_src, PipelineKey::Text);
        ctx->compile_pipeline(rounded_rect_fragment_src, rounded_rect_vertex_src, PipelineKey::RoundedRect);
    }

    frame_buffer_size_ = kInitialFrameBufferSize;
    for (int i = 0; i < 2; ++i) {
        GpuBufferDesc desc;
        desc.size = frame_buffer_size_;
        desc.cpu_writable = true;
        frame_buffer_[i] = backend_->create_buffer(desc);
        frame_ptr_[i] = backend_->map(frame_buffer_[i]);
        frame_gpu_base_[i] = backend_->gpu_address(frame_buffer_[i]);
    }

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

    auto state = read_state<ISurface>(surface);
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
        if (visual) {
            VisualCommands vc;
            vc.entries = visual->get_draw_entries(local_rect);

            auto vstate = read_state<IVisual>(visual);
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

            cache.visuals.push_back(std::move(vc));
        }
        auto tp = interface_pointer_cast<ITextureProvider>(att);
        if (tp) {
            cache.texture_provider = std::move(tp);
        }
    }
}

void Renderer::rebuild_batches(const SceneState& state, const SurfaceEntry& entry)
{
    batches_.clear();
    batch_index_.clear();

    auto resolve_texture = [](const IMaterial::Ptr& material, uint64_t fallback) -> uint64_t {
        return material ? reinterpret_cast<uintptr_t>(material.get()) : fallback;
    };

    for (auto* element : state.visual_list) {
        auto it = element_cache_.find(element);
        if (it == element_cache_.end()) {
            continue;
        }

        auto& cache = it->second;
        auto elem_state = read_state<IElement>(element);
        if (!elem_state) {
            continue;
        }

        float wx = elem_state->world_matrix(0, 3);
        float wy = elem_state->world_matrix(1, 3);

        for (auto& vc : cache.visuals) {
            for (auto& de : vc.entries) {
                uint64_t pipeline = (vc.pipeline_override != 0) ? vc.pipeline_override : de.pipeline_key;
                uint64_t texture = de.texture_key;

                // Batch by material identity: same instance = same params = safe to batch.
                uint64_t bkey = make_batch_key(pipeline, resolve_texture(vc.material, texture));

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

    auto* dst = static_cast<uint8_t*>(frame_ptr_[frame_index_]) + write_offset_;
    std::memcpy(dst, data, size);

    uint64_t gpu_addr = frame_gpu_base_[frame_index_] + write_offset_;
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

    for (int i = 0; i < 2; ++i) {
        backend_->destroy_buffer(frame_buffer_[i]);
        GpuBufferDesc desc;
        desc.size = new_size;
        desc.cpu_writable = true;
        frame_buffer_[i] = backend_->create_buffer(desc);
        frame_ptr_[i] = backend_->map(frame_buffer_[i]);
        frame_gpu_base_[i] = backend_->gpu_address(frame_buffer_[i]);
    }

    frame_buffer_size_ = new_size;
    peak_usage_ = 0;
}

void Renderer::build_draw_calls()
{
    draw_calls_.clear();

    for (auto& batch : batches_) {
        uint64_t instances_addr =
            write_to_frame_buffer(batch.instance_data.data(), batch.instance_data.size());
        if (!instances_addr) {
            continue;
        }

        uint32_t texture_id = 0;
        if (batch.texture_key != 0) {
            auto tit = texture_map_.find(batch.texture_key);
            if (tit != texture_map_.end()) {
                texture_id = tit->second;
            }
        }

        DrawDataHeader header{};
        header.globals_address = globals_gpu_addr_;
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

        write_offset_ = (write_offset_ + 15) & ~size_t(15);
        if (write_offset_ + total_size > frame_buffer_size_) {
            continue;
        }

        auto* dst = static_cast<uint8_t*>(frame_ptr_[frame_index_]) + write_offset_;
        uint64_t draw_data_addr = frame_gpu_base_[frame_index_] + write_offset_;

        std::memcpy(dst, &header, sizeof(header));
        if (mat_size > 0) {
            batch.material->write_gpu_data(dst + sizeof(DrawDataHeader), mat_size);
        }

        write_offset_ += total_size;
        if (write_offset_ > peak_usage_) {
            peak_usage_ = write_offset_;
        }

        if (!pipeline_map_) {
            continue;
        }
        auto pit = pipeline_map_->find(batch.pipeline_key);
        if (pit == pipeline_map_->end()) {
            continue;
        }

        DrawCall call{};
        call.pipeline = pit->second;
        call.vertex_count = 4;
        call.instance_count = batch.instance_count;
        call.root_constants_size = sizeof(uint64_t);
        std::memcpy(call.root_constants, &draw_data_addr, sizeof(uint64_t));

        draw_calls_.push_back(call);
    }
}

void Renderer::render()
{
    if (!backend_) {
        return;
    }

    for (auto& entry : surfaces_) {
        auto* scene = interface_cast<IScene>(entry.scene);
        if (!scene) {
            continue;
        }

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

        for (auto& removed : state.removed_list) {
            auto* elem = interface_cast<IElement>(removed);
            if (elem) {
                element_cache_.erase(elem);
            }
        }

        for (auto* element : state.redraw_list) {
            rebuild_commands(element);
        }

        bool textures_uploaded = false;
        if (has_changes) {
            for (auto& [elem, cache] : element_cache_) {
                if (cache.texture_provider && cache.texture_provider->is_texture_dirty()) {
                    auto& tp = cache.texture_provider;
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

        if (has_changes || textures_uploaded) {
            entry.batches_dirty = true;
        }

        if (entry.batches_dirty) {
            rebuild_batches(state, entry);
            entry.batches_dirty = false;
        }

        ensure_frame_buffer_capacity();
        write_offset_ = 0;

        if (globals_ptr_ && sstate) {
            build_ortho_projection(globals_ptr_->projection,
                                   static_cast<float>(sstate->width),
                                   static_cast<float>(sstate->height));
            globals_ptr_->viewport[0] = static_cast<float>(sstate->width);
            globals_ptr_->viewport[1] = static_cast<float>(sstate->height);
            globals_ptr_->viewport[2] = 1.0f / static_cast<float>(sstate->width);
            globals_ptr_->viewport[3] = 1.0f / static_cast<float>(sstate->height);
        }

        build_draw_calls();

        RENDER_LOG("render: submitting %zu draw calls", draw_calls_.size());

        backend_->begin_frame(entry.surface_id);
        backend_->submit({draw_calls_.data(), draw_calls_.size()});
        backend_->end_frame();

        frame_index_ = 1 - frame_index_;
    }
}

void Renderer::shutdown()
{
    if (backend_) {
        for (auto& entry : surfaces_) {
            backend_->destroy_surface(entry.surface_id);
        }

        for (auto& [key, tid] : texture_map_) {
            backend_->destroy_texture(tid);
        }
        texture_map_.clear();

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

} // namespace velk::ui
