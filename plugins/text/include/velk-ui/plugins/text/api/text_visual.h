#ifndef VELK_UI_TEXT_API_TEXT_VISUAL_H
#define VELK_UI_TEXT_API_TEXT_VISUAL_H

#include <velk/api/state.h>

#include <velk-ui/api/visual.h>
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/intf_text_visual.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around ITextVisual.
 *
 * Inherits color and paint accessors from Visual.
 *
 *   auto tv = visual::create_text();
 *   tv.set_font(font);
 *   tv.set_text("Hello!");
 *   tv.set_color(color::white());
 */
class TextVisual : public Visual
{
public:
    /** @brief Default-constructed TextVisual wraps no object. */
    TextVisual() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement ITextVisual. */
    explicit TextVisual(IObject::Ptr obj) : Visual(check_object<IVisual>(obj)) {}

    /** @brief Wraps an existing ITextVisual pointer. */
    explicit TextVisual(ITextVisual::Ptr t) : Visual(interface_pointer_cast<IObject>(t)) {}

    /** @brief Implicit conversion to ITextVisual::Ptr. */
    operator ITextVisual::Ptr() const { return as_ptr<ITextVisual>(); }

    /** @brief Sets the font used for shaping and rasterization. */
    void set_font(const Font& font)
    {
        with<ITextVisual>([&](auto& tv) { tv.set_font(font); });
    }

    /** @brief Returns the text content. */
    auto get_text() const { return read_state_value<ITextVisual>(&ITextVisual::State::text); }

    /** @brief Sets the text content. */
    void set_text(string_view text)
    {
        write_state_value<ITextVisual>(&ITextVisual::State::text, string(text));
    }
};

namespace visual {

/** @brief Creates a new TextVisual. */
inline TextVisual create_text()
{
    return TextVisual(instance().create<IObject>(ClassId::Visual::Text));
}

} // namespace visual

} // namespace velk::ui

#endif // VELK_UI_TEXT_API_TEXT_VISUAL_H
