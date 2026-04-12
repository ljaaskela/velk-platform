#ifndef VELK_UI_TEXT_API_TEXT_VISUAL_H
#define VELK_UI_TEXT_API_TEXT_VISUAL_H

#include <velk/api/state.h>

#include <velk-ui/api/visual/visual.h>
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/intf_text_visual.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around ITextVisual.
 *
 * Inherits color and paint accessors from Visual.
 *
 *   auto tv = trait::visual::create_text();
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

    /** @brief Sets the font size. */
    void set_font_size(float font_size)
    {
        write_state_value<ITextVisual>(&ITextVisual::State::font_size, font_size);
    }

    /** @brief Returns the font size. */
    auto get_font_size() const { return read_state_value<ITextVisual>(&ITextVisual::State::font_size); }

    /** @brief Sets the horizontal text alignment. */
    void set_h_align(HAlign align)
    {
        write_state_value<ITextVisual>(&ITextVisual::State::h_align, align);
    }

    /** @brief Returns the horizontal text alignment. */
    auto get_h_align() const { return read_state_value<ITextVisual>(&ITextVisual::State::h_align); }

    /** @brief Sets the vertical text alignment. */
    void set_v_align(VAlign align)
    {
        write_state_value<ITextVisual>(&ITextVisual::State::v_align, align);
    }

    /** @brief Returns the vertical text alignment. */
    auto get_v_align() const { return read_state_value<ITextVisual>(&ITextVisual::State::v_align); }

    /** @brief Sets the text layout mode (SingleLine, MultiLine, WordWrap). */
    void set_layout(TextLayout layout)
    {
        write_state_value<ITextVisual>(&ITextVisual::State::layout, layout);
    }

    /** @brief Returns the text layout mode. */
    auto get_layout() const { return read_state_value<ITextVisual>(&ITextVisual::State::layout); }
};

namespace trait::visual {

/** @brief Creates a new TextVisual. */
inline TextVisual create_text()
{
    return TextVisual(instance().create<IObject>(ClassId::Visual::Text));
}

} // namespace trait::visual

} // namespace velk::ui

#endif // VELK_UI_TEXT_API_TEXT_VISUAL_H
