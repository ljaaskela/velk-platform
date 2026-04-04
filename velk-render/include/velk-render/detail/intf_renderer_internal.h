#ifndef VELK_RENDER_INTF_RENDERER_INTERNAL_H
#define VELK_RENDER_INTF_RENDERER_INTERNAL_H

#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>

namespace velk {

/**
 * @brief Internal interface for injecting the backend into a renderer.
 *
 * Not exposed to the app. The RenderContext uses this to connect the
 * backend and context to the renderer implementation after creation.
 */
class IRendererInternal : public Interface<IRendererInternal>
{
public:
    virtual void set_backend(const IRenderBackend::Ptr& backend,
                             IRenderContext* ctx) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDERER_INTERNAL_H
