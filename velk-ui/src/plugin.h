#ifndef VELK_UI_ELEMENT_PLUGIN_H
#define VELK_UI_ELEMENT_PLUGIN_H

#include <velk/ext/plugin.h>

#include <velk-ui/plugin.h>

namespace velk::ui {

class VelkUiPlugin final : public ::velk::ext::Plugin<VelkUiPlugin>
{
public:
    VELK_PLUGIN_UID(PluginId::VelkUiPlugin);
    VELK_PLUGIN_NAME("velk-ui");
    VELK_PLUGIN_VERSION(0, 1, 0);

    ReturnValue initialize(IVelk& velk, PluginConfig& config) override;
    ReturnValue shutdown(IVelk& velk) override;

    void post_update(const IPlugin::PostUpdateInfo& info) override;
};

} // namespace velk::ui

VELK_PLUGIN(velk::ui::VelkUiPlugin)

#endif // VELK_UI_ELEMENT_PLUGIN_H
