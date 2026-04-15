#include "vk_plugin.h"

#include "vk_backend.h"

namespace velk::vk {

ReturnValue VkPlugin::initialize(IVelk& velk, PluginConfig&)
{
    ::velk::TypeOptions alloc;
    alloc.policy = ::velk::CreationPolicy::Alloc;
    return register_type<VkBackend>(velk, alloc);
}

ReturnValue VkPlugin::shutdown(IVelk&)
{
    return ReturnValue::Success;
}

} // namespace velk::vk
