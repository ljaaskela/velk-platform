#ifndef VELK_UI_VK_PLUGIN_IMPL_H
#define VELK_UI_VK_PLUGIN_IMPL_H

#include <velk/ext/plugin.h>

#include <velk-ui/plugins/vk/plugin.h>

namespace velk_ui {

class VkPlugin final : public velk::ext::Plugin<VkPlugin>
{
public:
    VELK_PLUGIN_UID(PluginId::VkPlugin);
    VELK_PLUGIN_NAME("velk-vk");
    VELK_PLUGIN_VERSION(0, 1, 0);

    velk::ReturnValue initialize(velk::IVelk& velk, velk::PluginConfig& config) override;
    velk::ReturnValue shutdown(velk::IVelk& velk) override;
};

} // namespace velk_ui

VELK_PLUGIN(velk_ui::VkPlugin)

#endif // VELK_UI_VK_PLUGIN_IMPL_H
