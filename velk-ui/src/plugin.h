#ifndef VELK_UI_ELEMENT_PLUGIN_H
#define VELK_UI_ELEMENT_PLUGIN_H

#include <velk/ext/plugin.h>

namespace velk_ui {

class VelkUiPlugin final : public velk::ext::Plugin<VelkUiPlugin>
{
public:
    VELK_PLUGIN_UID("45c450a1-5f11-4869-8f72-3bafaeae0079");
    VELK_PLUGIN_NAME("velk-ui");
    VELK_PLUGIN_VERSION(0, 1, 0);

    velk::ReturnValue initialize(velk::IVelk& velk, velk::PluginConfig& config) override;
    velk::ReturnValue shutdown(velk::IVelk& velk) override;

    void post_update(const IPlugin::PostUpdateInfo& info) override;
};

} // namespace velk_ui

VELK_PLUGIN(velk_ui::VelkUiPlugin)

#endif // VELK_UI_ELEMENT_PLUGIN_H
