#include "render_context.h"

#include "default_shaders.h"
#include "renderer.h"
#include "surface.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>

namespace velk_ui {

bool RenderContextImpl::init(const RenderConfig& config)
{
    static constexpr velk::Uid kGlPluginId{"e1e9e004-21cd-4cfa-b843-49b0eb358149"};
    static constexpr velk::Uid kGlBackendId{"2302c979-1531-4d0b-bab6-d1bac99f0a11"};

    velk::Uid plugin_id;
    velk::Uid class_id;

    switch (config.backend) {
    case RenderBackendType::GL:
        plugin_id = kGlPluginId;
        class_id = kGlBackendId;
        break;
    default:
        VELK_LOG(E, "RenderContext::init: unsupported backend type %d", static_cast<int>(config.backend));
        return false;
    }

    auto& reg = velk::instance().plugin_registry();
    if (!reg.get_or_load_plugin(plugin_id)) {
        VELK_LOG(E, "RenderContext::init: failed to load backend plugin");
        return false;
    }

    auto obj = velk::instance().create<velk::IObject>(class_id);
    backend_ = interface_pointer_cast<IRenderBackend>(obj);
    if (!backend_) {
        VELK_LOG(E, "RenderContext::init: failed to create backend");
        return false;
    }

    if (!backend_->init(config.backend_params)) {
        VELK_LOG(E, "RenderContext::init: backend init failed");
        backend_ = nullptr;
        return false;
    }

    PipelineDesc rect_desc{rect_vertex_src, rect_fragment_src, VertexFormat::Untextured};
    backend_->register_pipeline(PipelineKey::Rect, rect_desc);

    PipelineDesc text_desc{text_vertex_src, text_fragment_src, VertexFormat::Textured};
    backend_->register_pipeline(PipelineKey::Text, text_desc);

    initialized_ = true;
    VELK_LOG(I, "RenderContext initialized (backend=%d)", static_cast<int>(config.backend));
    return true;
}

ISurface::Ptr RenderContextImpl::create_surface(int width, int height)
{
    auto obj = velk::instance().create<velk::IObject>(Surface::static_class_id());
    auto surface = interface_pointer_cast<ISurface>(obj);
    if (!surface) {
        return nullptr;
    }

    velk::write_state<ISurface>(surface, [&](ISurface::State& s) {
        s.width = width;
        s.height = height;
    });

    if (backend_) {
        uint64_t sid = next_surface_id_++;
        SurfaceDesc desc{width, height};
        backend_->create_surface(sid, desc);
    }

    return surface;
}

IRenderer::Ptr RenderContextImpl::create_renderer()
{
    if (!initialized_ || !backend_) {
        VELK_LOG(E, "RenderContext::create_renderer: context not initialized");
        return nullptr;
    }

    auto obj = velk::instance().create<velk::IObject>(ClassId::Renderer);
    if (!obj) {
        VELK_LOG(E, "RenderContext::create_renderer: failed to create renderer");
        return nullptr;
    }

    auto* internal = interface_cast<IRendererInternal>(obj);
    if (internal) {
        internal->set_backend(backend_);
    }

    return interface_pointer_cast<IRenderer>(obj);
}

} // namespace velk_ui
