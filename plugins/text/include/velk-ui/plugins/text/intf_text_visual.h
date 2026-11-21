#ifndef VELK_UI_INTF_TEXT_VISUAL_H
#define VELK_UI_INTF_TEXT_VISUAL_H

#include <velk/api/object_ref.h>
#include <velk/interface/intf_metadata.h>
#include <velk/string.h>

#include <velk-ui/interface/intf_font.h>
#include <velk-scene/types.h>

namespace velk::ui {

/**
 * @brief Interface for configuring a TextVisual.
 *
 * Reshaping happens automatically when the text, font or font size changes.
 */
class ITextVisual : public Interface<ITextVisual>
{
public:
    VELK_INTERFACE(
        (PROP, string, text, {}),                                  ///< Text content to render.
        (PROP, ObjectRef, font, {}),                               ///< IFont to use. Empty, or a font that failed to load, means the default font.
        (PROP, float, font_size, 16.f),                            ///< Font size in pixels.
        (PROP, ::velk::HAlign, h_align, ::velk::HAlign::Left),             ///< Horizontal text alignment.
        (PROP, ::velk::VAlign, v_align, ::velk::VAlign::Top),              ///< Vertical text alignment.
        (PROP, ::velk::TextLayout, layout, ::velk::TextLayout::MultiLine)  ///< Text layout mode.
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_TEXT_VISUAL_H
