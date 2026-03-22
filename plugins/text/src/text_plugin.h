#ifndef VELK_UI_TEXT_PLUGIN_IMPL_H
#define VELK_UI_TEXT_PLUGIN_IMPL_H

#include <velk/ext/plugin.h>

namespace velk_ui {

class TextPlugin final : public velk::ext::Plugin<TextPlugin>
{
public:
    VELK_PLUGIN_UID("a8b9c0d1-e2f3-4a5b-6c7d-8e9f0a1b2c3d");
    VELK_PLUGIN_NAME("velk_text");
    VELK_PLUGIN_VERSION(0, 1, 0);

    velk::ReturnValue initialize(velk::IVelk& velk, velk::PluginConfig& config) override;
    velk::ReturnValue shutdown(velk::IVelk& velk) override;
};

} // namespace velk_ui

VELK_PLUGIN(velk_ui::TextPlugin)

#endif // VELK_UI_TEXT_PLUGIN_IMPL_H
