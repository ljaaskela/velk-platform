#include "vk_plugin.h"

#include "vk_backend.h"

namespace velk::vk {

ReturnValue VkPlugin::initialize(IVelk& velk, PluginConfig&)
{
    return register_type<VkBackend>(velk);
}

ReturnValue VkPlugin::shutdown(IVelk&)
{
    return ReturnValue::Success;
}

} // namespace velk::vk
