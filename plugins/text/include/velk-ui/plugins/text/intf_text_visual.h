#ifndef VELK_UI_INTF_TEXT_VISUAL_H
#define VELK_UI_INTF_TEXT_VISUAL_H

#include <velk/interface/intf_metadata.h>
#include <velk/string.h>

#include <velk-ui/interface/intf_font.h>
#include <velk-ui/types.h>

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
        (PROP, string, text, {}),          ///< Text content to render.
        (PROP, ui::HAlign, h_align, ui::HAlign::Left),  ///< Horizontal text alignment.
        (PROP, ui::VAlign, v_align, ui::VAlign::Top)    ///< Vertical text alignment.
    )

    /** @brief Sets the font used for shaping and rasterization. Takes ownership (shared). */
    virtual void set_font(const IFont::Ptr& font) = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_TEXT_VISUAL_H
