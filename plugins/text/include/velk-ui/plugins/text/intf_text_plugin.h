#ifndef VELK_UI_TEXT_INTF_TEXT_PLUGIN_H
#define VELK_UI_TEXT_INTF_TEXT_PLUGIN_H

#include <velk/interface/intf_metadata.h>

#include <velk-ui/interface/intf_font.h>

namespace velk::ui {

/**
 * @brief Public interface for the text plugin.
 *
 * Provides access to the built-in fonts. Accessible via
 * plugin_registry().get_or_load_plugin<ITextPlugin>(PluginId::TextPlugin).
 *
 * The built-in fonts are also resources:
 * `font:memory://velk-text/Inter-Regular.ttf` and
 * `font:memory://velk-text/JetBrainsMono-Regular.ttf`.
 */
class ITextPlugin : public Interface<ITextPlugin>
{
public:
    /** @brief Returns the default font (Inter Regular). */
    virtual IFont::Ptr get_default_font() const = 0;

    /** @brief Returns the built-in monospace font (JetBrains Mono Regular). */
    virtual IFont::Ptr get_monospace_font() const = 0;
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_INTF_TEXT_PLUGIN_H
