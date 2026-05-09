#include "vk_plugin.h"

#include "vk_backend.h"
#include "vk_command_buffer.h"
#include "vk_gpu_buffer.h"
#include "vk_gpu_pipeline.h"

namespace velk::vk {

ReturnValue VkPlugin::initialize(IVelk& velk, PluginConfig&)
{
    ::velk::TypeOptions alloc;
    alloc.policy = ::velk::CreationPolicy::Alloc;
    ReturnValue rv = register_type<VkBackend>(velk, alloc);
    if (rv != ReturnValue::Success) return rv;
    rv = register_type<VkCommandBuffer>(velk);
    if (rv != ReturnValue::Success) return rv;
    rv = register_type<VkGpuBuffer>(velk);
    if (rv != ReturnValue::Success) return rv;
    return register_type<VkGpuPipeline>(velk);
}

ReturnValue VkPlugin::shutdown(IVelk&)
{
    return ReturnValue::Success;
}

} // namespace velk::vk
