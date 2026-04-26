#ifndef VELK_UI_API_RENDERER_H
#define VELK_UI_API_RENDERER_H

#include <velk/api/velk.h>
#include <velk-render/detail/intf_renderer_internal.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/plugin.h>
#include <velk-scene/interface/intf_renderer.h>

namespace velk::ui {

/**
 * @brief Creates a scene renderer connected to the given render context.
 *
 * The renderer draws views added via add_view(camera_element, surface).
 * Each view uses a camera trait to determine the projection.
 */
inline IRenderer::Ptr create_renderer(IRenderContext& ctx)
{
    auto obj = instance().create<IObject>(::velk::ClassId::Renderer);
    if (!obj) return nullptr;

    auto* internal = interface_cast<IRendererInternal>(obj);
    if (internal) {
        internal->set_backend(ctx.backend(), &ctx);
    }

    return interface_pointer_cast<IRenderer>(obj);
}

} // namespace velk::ui

#endif // VELK_UI_API_RENDERER_H
