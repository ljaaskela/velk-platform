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
 *   auto ctx = create_render_context({&vk_params});
 *   auto renderer = ctx.create_renderer();
 *   auto surface = ctx.create_surface(800, 600);
 */
class RenderContext : public ::velk::Object
{
public:
    RenderContext() = default;
    explicit RenderContext(velk::IObject::Ptr obj) : ::velk::Object(check_object<IRenderContext>(obj)) {}
    explicit RenderContext(IRenderContext::Ptr obj) : ::velk::Object(velk::as_object(obj)) {}

    operator IRenderContext::Ptr() const { return as_ptr<IRenderContext>(); }

    ISurface::Ptr create_surface(int width, int height)
    {
        return with<IRenderContext>([&](auto& ctx) { return ctx.create_surface(width, height); });
    }

    IRenderer::Ptr create_renderer()
    {
        return with<IRenderContext>([&](auto& ctx) { return ctx.create_renderer(); });
    }

    /**
     * @brief Creates a shader material from GLSL source.
     * @param fragment_source GLSL fragment shader source.
     * @param vertex_source Optional vertex shader (uses default instanced quad if nullptr).
     * @return An IObject implementing IMaterial with the pipeline handle set,
     *         or nullptr on compilation failure.
     */
    velk::IObject::Ptr create_shader_material(const char* fragment_source,
                                              const char* vertex_source = nullptr)
    {
        return with<IRenderContext>([&](auto& ctx) {
            return ctx.create_shader_material(fragment_source, vertex_source);
        });
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
