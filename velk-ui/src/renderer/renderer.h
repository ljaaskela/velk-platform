#ifndef VELK_UI_RENDERER_IMPL_H
#define VELK_UI_RENDERER_IMPL_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <unordered_map>
#include <velk-render/detail/intf_renderer_internal.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_material.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_texture_provider.h>
#include <velk-render/plugin.h>
#include <velk-render/render_types.h>
#include <velk-ui/interface/intf_renderer.h>
#include <velk-ui/interface/intf_scene.h>

namespace velk::ui {

class Renderer : public ::velk::ext::Object<Renderer, IRendererInternal, IRenderer>
{
public:
    VELK_CLASS_UID(::velk::ClassId::Renderer, "Renderer");

    // IRendererInternal
    void set_backend(const IRenderBackend::Ptr& backend, IRenderContext* ctx) override;

    // IRenderer
    void add_view(const IElement::Ptr& camera_element, const ISurface::Ptr& surface) override;
    void remove_view(const IElement::Ptr& camera_element, const ISurface::Ptr& surface) override;
    void render() override;
    void shutdown() override;

private:
    struct VisualCommands
    {
        vector<DrawEntry> entries;
        uint64_t pipeline_override = 0;
        IMaterial::Ptr material;
    };

    struct ElementCache
    {
        vector<VisualCommands> visuals;
        ITextureProvider::Ptr texture_provider;
    };

    struct ViewEntry
    {
        IElement::Ptr camera_element;
        ISurface::Ptr surface;
        uint64_t surface_id = 0;
        bool batches_dirty = true;
        int cached_width = 0;
        int cached_height = 0;
    };

    struct Batch
    {
        uint64_t pipeline_key = 0;
        uint64_t texture_key = 0;
        vector<uint8_t> instance_data;
        uint32_t instance_stride = 0;
        uint32_t instance_count = 0;
        IMaterial::Ptr material;
    };

    void rebuild_commands(IElement* element);
    void rebuild_batches(const SceneState& state, const ViewEntry& entry);
    void build_draw_calls();

    uint64_t write_to_frame_buffer(const void* data, size_t size, size_t alignment = 16);

    IRenderBackend::Ptr backend_;
    IRenderContext* render_ctx_ = nullptr;
    vector<ViewEntry> views_;
    std::unordered_map<IElement*, ElementCache> element_cache_;

    const std::unordered_map<uint64_t, PipelineId>* pipeline_map_ = nullptr;

    static constexpr size_t kInitialFrameBufferSize = 256 * 1024;
    GpuBuffer frame_buffer_[2]{};
    void* frame_ptr_[2]{};
    uint64_t frame_gpu_base_[2]{};
    size_t frame_buffer_size_ = 0;
    size_t write_offset_ = 0;
    size_t peak_usage_ = 0;
    int frame_index_ = 0;

    void ensure_frame_buffer_capacity();

    GpuBuffer globals_buffer_ = 0;
    FrameGlobals* globals_ptr_ = nullptr;
    uint64_t globals_gpu_addr_ = 0;

    std::unordered_map<uint64_t, TextureId> texture_map_;

    std::unordered_map<uint64_t, size_t> batch_index_;
    vector<Batch> batches_;
    vector<DrawCall> draw_calls_;
};

} // namespace velk::ui

#endif // VELK_UI_RENDERER_IMPL_H
