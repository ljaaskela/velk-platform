#ifndef VELK_RENDER_API_RENDER_CONTEXT_H
#define VELK_RENDER_API_RENDER_CONTEXT_H

#include <velk/api/object.h>
#include <velk/api/velk.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/plugin.h>

namespace velk {

/**
 * @brief Convenience wrapper around IRenderContext.
 *
 *   auto ctx = velk::create_render_context(config);
 *   auto surface = ctx.create_surface({.width = 800, .height = 600});
 */
class RenderContext : public Object
{
public:
    RenderContext() = default;
    explicit RenderContext(IObject::Ptr obj) : Object(check_object<IRenderContext>(obj)) {}
    explicit RenderContext(IRenderContext::Ptr obj) : Object(as_object(obj)) {}

    operator IRenderContext::Ptr() const { return as_ptr<IRenderContext>(); }

    IWindowSurface::Ptr create_surface(const SurfaceConfig& config)
    {
        return with<IRenderContext>([&](auto& ctx) { return ctx.create_surface(config); });
    }

    IMaterial::Ptr create_shader_material(string_view fragment_source, string_view vertex_source = {})
    {
        return with<IRenderContext>([&](auto& ctx) {
            return ctx.create_shader_material(fragment_source, vertex_source);
        });
    }
};

/**
 * @brief Creates and initializes a render context with the specified backend.
 */
inline RenderContext create_render_context(const RenderConfig& config)
{
    auto& v = instance();
    v.plugin_registry().get_or_load_plugin(PluginId::RenderPlugin);
    auto ctx = v.create<IRenderContext>(ClassId::RenderContext);
    return RenderContext(ctx && ctx->init(config) ? ctx : nullptr);
}

} // namespace velk

#endif // VELK_RENDER_API_RENDER_CONTEXT_H
