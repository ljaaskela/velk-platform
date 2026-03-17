#include "plugin.h"

namespace velk_ui {

velk::ReturnValue VelkUiPlugin::initialize(velk::IVelk& velk, velk::PluginConfig&)
{
    return velk::register_type<Element>(velk);
}

velk::ReturnValue VelkUiPlugin::shutdown(velk::IVelk&)
{
    return velk::ReturnValue::Success;
}

} // namespace velk_ui
