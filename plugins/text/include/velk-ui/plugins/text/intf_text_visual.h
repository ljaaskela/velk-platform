#ifndef VELK_UI_INTF_TEXT_VISUAL_H
#define VELK_UI_INTF_TEXT_VISUAL_H

#include <velk/interface/intf_metadata.h>
#include <velk/string.h>

#include <velk-ui/interface/intf_font.h>
#include <velk-scene/types.h>

namespace velk::ui {

/**
 * @brief Interface for configuring a TextVisual.
 *
 * The visual owns the font. Set the font, then set the text (or vice versa).
 * Reshaping happens automatically when both are present.
 */
class ITextVisual : public Interface<ITextVisual>
{
public:
    VELK_INTERFACE(
        (PROP, string, text, {}),                                  ///< Text content to render.
        (PROP, float, font_size, 16.f),                            ///< Font size in pixels.
        (PROP, ::velk::HAlign, h_align, ::velk::HAlign::Left),             ///< Horizontal text alignment.
        (PROP, ::velk::VAlign, v_align, ::velk::VAlign::Top),              ///< Vertical text alignment.
        (PROP, ::velk::TextLayout, layout, ::velk::TextLayout::MultiLine)  ///< Text layout mode.
    )

    /** @brief Sets the font used for shaping and rasterization. Takes ownership (shared). */
    virtual void set_font(const IFont::Ptr& font) = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_TEXT_VISUAL_H
