#include "text_plugin.h"
#include "font.h"

namespace velk_ui {

velk::ReturnValue TextPlugin::initialize(velk::IVelk& velk, velk::PluginConfig&)
{
    return velk::register_type<Font>(velk);
}

velk::ReturnValue TextPlugin::shutdown(velk::IVelk&)
{
    return velk::ReturnValue::Success;
}

} // namespace velk_ui
