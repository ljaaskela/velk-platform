#include "vk_plugin.h"

#include "vk_backend.h"

namespace velk_ui {

velk::ReturnValue VkPlugin::initialize(velk::IVelk& velk, velk::PluginConfig&)
{
    return velk::register_type<VkBackend>(velk);
}

velk::ReturnValue VkPlugin::shutdown(velk::IVelk&)
{
    return velk::ReturnValue::Success;
}

} // namespace velk_ui
