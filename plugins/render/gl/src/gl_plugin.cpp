#include "gl_plugin.h"

#include "gl_renderer.h"

namespace velk_ui {

velk::ReturnValue GlPlugin::initialize(velk::IVelk& velk, velk::PluginConfig&)
{
    return velk::register_type<GlRenderer>(velk);
}

velk::ReturnValue GlPlugin::shutdown(velk::IVelk&)
{
    return velk::ReturnValue::Success;
}

} // namespace velk_ui
