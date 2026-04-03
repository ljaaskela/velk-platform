#ifndef VELK_UI_RENDERER_IMPL_H
#define VELK_UI_RENDERER_IMPL_H

#include "intf_renderer_internal.h"

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <unordered_map>
#include <velk-ui/interface/intf_material.h>
#include <velk-ui/interface/intf_renderer.h>
#include <velk-ui/interface/intf_scene.h>
#include <velk-ui/interface/intf_texture_provider.h>
#include <velk-ui/gpu_data.h>
#include <velk-ui/plugins/render/intf_render_backend.h>
#include <velk-ui/plugins/render/plugin.h>
#include <velk-ui/types.h>

namespace velk_ui {

class Renderer : public velk::ext::Object<Renderer, IRendererInternal>
{
public:
    VELK_CLASS_UID(ClassId::Renderer, "Renderer");

    // IRendererInternal
    void set_backend(const IRenderBackend::Ptr& backend,
                     IRenderContext* ctx) override;

    // IRenderer
    void attach(const ISurface::Ptr& surface, const IScene::Ptr& scene) override;
    void detach(const ISurface::Ptr& surface) override;
    void render() override;
    void shutdown() override;

private:
    // Per-visual draw entry cache
    struct VisualCommands
    {
        velk::vector<DrawEntry> entries;
        uint64_t pipeline_override = 0;
        IMaterial* material = nullptr;
    };

    struct ElementCache
    {
        velk::vector<VisualCommands> visuals;
        ITextureProvider* texture_provider = nullptr;
    };

    struct SurfaceEntry
    {
        ISurface::Ptr surface;
        IScene::Ptr scene;
        uint64_t surface_id = 0;
        bool batches_dirty = true;
        int cached_width = 0;
        int cached_height = 0;
    };

    // Batch: a group of draw entries sharing the same pipeline
    struct Batch
    {
        uint64_t pipeline_key = 0;
        uint64_t texture_key = 0;
        velk::vector<uint8_t> instance_data;
        uint32_t instance_stride = 0;
        uint32_t instance_count = 0;
        IMaterial* material = nullptr;
        velk::rect rect{};
        bool has_rect = false;
    };

    void rebuild_commands(IElement* element);
    void rebuild_batches(const SceneState& state, const SurfaceEntry& entry);
    void build_draw_calls();

    // Frame buffer management (bump allocator in GPU memory)
    uint64_t write_to_frame_buffer(const void* data, size_t size, size_t alignment = 16);

    IRenderBackend::Ptr backend_;
    IRenderContext* render_ctx_ = nullptr;
    velk::vector<SurfaceEntry> surfaces_;
    std::unordered_map<IElement*, ElementCache> element_cache_;

    // Pipeline key -> PipelineId mapping (pointer to context's map, stays in sync)
    const std::unordered_map<uint64_t, PipelineId>* pipeline_map_ = nullptr;

    // Frame GPU buffers (double-buffered)
    static constexpr size_t kFrameBufferSize = 4 * 1024 * 1024; // 4 MB
    GpuBuffer frame_buffer_[2]{};
    void* frame_ptr_[2]{};
    uint64_t frame_gpu_base_[2]{};
    size_t write_offset_ = 0;
    int frame_index_ = 0;

    // Globals buffer (long-lived)
    GpuBuffer globals_buffer_ = 0;
    FrameGlobals* globals_ptr_ = nullptr;
    uint64_t globals_gpu_addr_ = 0;

    // Texture management
    std::unordered_map<uint64_t, TextureId> texture_map_;

    // Batching
    std::unordered_map<uint64_t, size_t> batch_index_;
    velk::vector<Batch> batches_;
    velk::vector<DrawCall> draw_calls_;
};

} // namespace velk_ui

#endif // VELK_UI_RENDERER_IMPL_H
