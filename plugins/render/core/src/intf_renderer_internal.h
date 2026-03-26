#ifndef VELK_UI_INTF_RENDERER_INTERNAL_H
#define VELK_UI_INTF_RENDERER_INTERNAL_H

#include <velk/interface/intf_metadata.h>

#include <velk-ui/interface/intf_renderer.h>
#include <velk-ui/plugins/render/intf_render_backend.h>

namespace velk_ui {

/**
 * @brief Internal renderer interface used by RenderContext.
 *
 * Not exposed to the app. The RenderContext uses this to inject
 * the backend into the renderer after creation.
 */
class IRendererInternal : public velk::Interface<IRendererInternal, IRenderer>
{
public:
    virtual void set_backend(const IRenderBackend::Ptr& backend) = 0;
};

} // namespace velk_ui

#endif // VELK_UI_INTF_RENDERER_INTERNAL_H
