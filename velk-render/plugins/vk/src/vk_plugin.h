#ifndef VELK_VK_PLUGIN_IMPL_H
#define VELK_VK_PLUGIN_IMPL_H

#include <velk/ext/plugin.h>

#include <velk-render/plugins/vk/plugin.h>

namespace velk::vk {

class VkPlugin final : public ext::Plugin<VkPlugin>
{
public:
    VELK_PLUGIN_UID(PluginId::VkPlugin);
    VELK_PLUGIN_NAME("velk-vk");
    VELK_PLUGIN_VERSION(0, 1, 0);

    ReturnValue initialize(IVelk& velk, PluginConfig& config) override;
    ReturnValue shutdown(IVelk& velk) override;
};

} // namespace velk::vk

VELK_PLUGIN(velk::vk::VkPlugin)

#endif // VELK_VK_PLUGIN_IMPL_H
