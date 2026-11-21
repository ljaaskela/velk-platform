#ifndef VELK_UI_TEXT_API_FONT_H
#define VELK_UI_TEXT_API_FONT_H

#include <velk/api/object.h>
#include <velk/api/state.h>

#include <velk-ui/interface/intf_font.h>
#include <velk-ui/plugins/text/intf_text_plugin.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around IFont.
 *
 * Provides null-safe access to font operations.
 *
 *   Font font(instance().resource_store().get_resource<IFont>("font:app://fonts/x.ttf"));
 */
class Font : public Object
{
public:
    /** @brief Default-constructed Font wraps no object. */
    Font() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement IFont. */
    explicit Font(IObject::Ptr obj)
        : Object(obj && interface_cast<IFont>(obj) ? std::move(obj) : IObject::Ptr{})
    {}

    /** @brief Wraps an existing IFont pointer. */
    explicit Font(IFont::Ptr f) : Object(interface_pointer_cast<IObject>(f)) {}

    /** @brief Implicit conversion to IFont::Ptr. */
    operator IFont::Ptr() const { return as_ptr<IFont>(); }

    /** @brief Returns the font ascender in pixels. */
    auto get_ascender() const { return read_state_value<IFont>(&IFont::State::ascender); }

    /** @brief Returns the font descender in pixels. */
    auto get_descender() const { return read_state_value<IFont>(&IFont::State::descender); }

    /** @brief Returns the line height in pixels. */
    auto get_line_height() const { return read_state_value<IFont>(&IFont::State::line_height); }
};

/** @brief Returns the default font (Inter Regular). */
inline Font get_default_font()
{
    auto plugin = get_or_load_plugin<ITextPlugin>(PluginId::TextPlugin);
    return Font(plugin ? plugin->get_default_font() : nullptr);
}

/** @brief Returns the built-in monospace font (JetBrains Mono Regular). */
inline Font get_monospace_font()
{
    auto plugin = get_or_load_plugin<ITextPlugin>(PluginId::TextPlugin);
    return Font(plugin ? plugin->get_monospace_font() : nullptr);
}

} // namespace velk::ui

#endif // VELK_UI_TEXT_API_FONT_H
