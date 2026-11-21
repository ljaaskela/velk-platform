#ifndef VELK_UI_TEXT_PLUGIN_IMPL_H
#define VELK_UI_TEXT_PLUGIN_IMPL_H

#include <velk/ext/plugin.h>
#include <velk/interface/resource/intf_resource_decoder.h>

#include <velk-ui/plugins/text/intf_text_plugin.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

class TextPlugin final : public ::velk::ext::Plugin<TextPlugin, ITextPlugin>
{
public:
    VELK_PLUGIN_UID(PluginId::TextPlugin);
    VELK_PLUGIN_NAME("velk-text");
    VELK_PLUGIN_VERSION(0, 1, 0);

    ReturnValue initialize(IVelk& velk, PluginConfig& config) override;
    ReturnValue shutdown(IVelk& velk) override;

    // ITextPlugin
    IFont::Ptr get_default_font() const override;
    IFont::Ptr get_monospace_font() const override;

private:
    IResourceDecoder::Ptr font_decoder_;
    IFont::Ptr default_font_;
    IFont::Ptr monospace_font_;
};

} // namespace velk::ui

VELK_PLUGIN(velk::ui::TextPlugin)

#endif // VELK_UI_TEXT_PLUGIN_IMPL_H
