#include "vk_plugin.h"

#include "vk_backend.h"
#include "vk_command_buffer.h"

namespace velk::vk {

ReturnValue VkPlugin::initialize(IVelk& velk, PluginConfig&)
{
    ::velk::TypeOptions alloc;
    alloc.policy = ::velk::CreationPolicy::Alloc;
    ReturnValue rv = register_type<VkBackend>(velk, alloc);
    if (rv != ReturnValue::Success) return rv;
    return register_type<VkCommandBuffer>(velk);
}

ReturnValue VkPlugin::shutdown(IVelk&)
{
    return ReturnValue::Success;
}

} // namespace velk::vk
