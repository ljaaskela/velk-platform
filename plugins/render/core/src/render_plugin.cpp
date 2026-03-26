#include "render_plugin.h"
#include "render_context.h"
#include "renderer.h"
#include "surface.h"

namespace velk_ui {

velk::ReturnValue RenderPlugin::initialize(velk::IVelk& velk, velk::PluginConfig& config)
{
    auto rv = velk::register_type<RenderContextImpl>(velk);
    rv &= velk::register_type<Renderer>(velk);
    rv &= velk::register_type<Surface>(velk);
    return rv;
}

velk::ReturnValue RenderPlugin::shutdown(velk::IVelk&)
{
    return velk::ReturnValue::Success;
}

} // namespace velk_ui
