#ifndef VELK_UI_RENDER_CONTEXT_IMPL_H
#define VELK_UI_RENDER_CONTEXT_IMPL_H

#include <velk/ext/object.h>

#include <velk-ui/interface/intf_render_context.h>
#include <velk-ui/plugins/render/intf_render_backend.h>
#include <velk-ui/plugins/render/plugin.h>

namespace velk_ui {

class RenderContextImpl : public velk::ext::ObjectCore<RenderContextImpl, IRenderContext>
{
public:
    VELK_CLASS_UID(ClassId::RenderContext, "RenderContext");

    bool init(const RenderConfig& config) override;
    ISurface::Ptr create_surface(int width, int height) override;
    IRenderer::Ptr create_renderer() override;

private:
    IRenderBackend::Ptr backend_;
    uint64_t next_surface_id_ = 1;
    bool initialized_ = false;
};

} // namespace velk_ui

#endif // VELK_UI_RENDER_CONTEXT_IMPL_H
