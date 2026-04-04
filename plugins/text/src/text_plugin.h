#ifndef VELK_UI_TEXT_PLUGIN_IMPL_H
#define VELK_UI_TEXT_PLUGIN_IMPL_H

#include <velk/ext/plugin.h>

#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

class TextPlugin final : public ::velk::ext::Plugin<TextPlugin>
{
public:
    VELK_PLUGIN_UID(PluginId::TextPlugin);
    VELK_PLUGIN_NAME("velk_text");
    VELK_PLUGIN_VERSION(0, 1, 0);

    ReturnValue initialize(IVelk& velk, PluginConfig& config) override;
    ReturnValue shutdown(IVelk& velk) override;
};

} // namespace velk::ui

VELK_PLUGIN(velk::ui::TextPlugin)

#endif // VELK_UI_TEXT_PLUGIN_IMPL_H
