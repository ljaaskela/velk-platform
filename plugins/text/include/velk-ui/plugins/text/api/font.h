#ifndef VELK_UI_TEXT_API_FONT_H
#define VELK_UI_TEXT_API_FONT_H

#include <velk/api/object.h>
#include <velk/api/state.h>

#include <velk-ui/interface/intf_font.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk_ui {

/**
 * @brief Convenience wrapper around IFont.
 *
 * Provides null-safe access to font operations.
 *
 *   auto font = create_font();
 *   font.init_default();
 *   font.set_size(32.f);
 */
class Font : public velk::Object
{
public:
    /** @brief Default-constructed Font wraps no object. */
    Font() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement IFont. */
    explicit Font(velk::IObject::Ptr obj)
        : Object(obj && interface_cast<IFont>(obj) ? std::move(obj) : velk::IObject::Ptr{})
    {}

    /** @brief Wraps an existing IFont pointer. */
    explicit Font(IFont::Ptr f) : Object(interface_pointer_cast<velk::IObject>(f)) {}

    /** @brief Implicit conversion to IFont::Ptr. */
    operator IFont::Ptr() const { return as_ptr<IFont>(); }

    /** @brief Initializes with the built-in default font (Inter Regular). */
    bool init_default() { return with<IFont>([](auto& f) { return f.init_default(); }); }

    /** @brief Sets the font size in pixels. */
    bool set_size(float size_px) { return with<IFont>([&](auto& f) { return f.set_size(size_px); }); }

    /** @brief Returns the font ascender in pixels. */
    auto get_ascender() const { return read_state_value<IFont>(&IFont::State::ascender); }

    /** @brief Returns the font descender in pixels. */
    auto get_descender() const { return read_state_value<IFont>(&IFont::State::descender); }

    /** @brief Returns the line height in pixels. */
    auto get_line_height() const { return read_state_value<IFont>(&IFont::State::line_height); }

    /** @brief Returns the current font size in pixels. */
    auto get_size_px() const { return read_state_value<IFont>(&IFont::State::size_px); }
};

/** @brief Creates a new Font. */
inline Font create_font()
{
    return Font(velk::instance().create<velk::IObject>(ClassId::Font));
}

} // namespace velk_ui

#endif // VELK_UI_TEXT_API_FONT_H
