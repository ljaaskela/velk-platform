#ifndef VELK_UI_API_RENDER_CONTEXT_H
#define VELK_UI_API_RENDER_CONTEXT_H

#include <velk/api/object.h>
#include <velk/api/velk.h>
#include <velk-ui/interface/intf_render_context.h>
#include <velk-ui/plugins/render/plugin.h>

namespace velk_ui {

/**
 * @brief Convenience wrapper around IRenderContext.
 *
 *   auto ctx = create_render_context({RenderBackendType::GL, glfwGetProcAddress});
 *   auto renderer = ctx.create_renderer();
 *   auto surface = ctx.create_surface(800, 600);
 */
class RenderContext : public ::velk::Object
{
public:
    RenderContext() = default;
    explicit RenderContext(velk::IObject::Ptr obj) : ::velk::Object(check_object<IRenderContext>(obj)) {}
    explicit RenderContext(IRenderContext::Ptr obj) : ::velk::Object(obj) {}

    operator IRenderContext::Ptr() const { return as_ptr<IRenderContext>(); }

    ISurface::Ptr create_surface(int width, int height)
    {
        return with<IRenderContext>([&](auto& ctx) { return ctx.create_surface(width, height); });
    }

    IRenderer::Ptr create_renderer()
    {
        return with<IRenderContext>([&](auto& ctx) { return ctx.create_renderer(); });
    }
};

/**
 * @brief Creates and initializes a render context with the specified backend.
 *
 * Loads the render plugin, creates the context, initializes the backend,
 * and returns it ready to use.
 */
inline RenderContext create_render_context(const RenderConfig& config)
{
    auto& v = velk::instance();
    v.plugin_registry().get_or_load_plugin(PluginId::RenderPlugin);
    auto ctx = v.create<IRenderContext>(ClassId::RenderContext);
    return RenderContext(ctx && ctx->init(config) ? ctx : nullptr);
}

} // namespace velk_ui

#endif // VELK_UI_API_RENDER_CONTEXT_H
