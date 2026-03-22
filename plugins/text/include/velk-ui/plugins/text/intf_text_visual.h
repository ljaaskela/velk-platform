#ifndef VELK_UI_INTF_TEXT_VISUAL_H
#define VELK_UI_INTF_TEXT_VISUAL_H

#include <velk/interface/intf_interface.h>
#include <velk/string_view.h>

#include <velk-ui/interface/intf_font.h>

namespace velk_ui {

/**
 * @brief Interface for setting text content on a TextVisual.
 *
 * Implemented by TextVisual. Allows external code to shape text
 * without depending on the concrete TextVisual class.
 */
class ITextVisual : public velk::Interface<ITextVisual>
{
public:
    /** @brief Shapes text with the given font and rebuilds glyph commands. */
    virtual void set_text(velk::string_view text, IFont& font) = 0;
};

} // namespace velk_ui

#endif // VELK_UI_INTF_TEXT_VISUAL_H
