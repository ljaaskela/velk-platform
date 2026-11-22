#ifndef VELK_UI_INTF_TEXT_FIELD_H
#define VELK_UI_INTF_TEXT_FIELD_H

#include <velk/interface/intf_metadata.h>
#include <velk/string.h>

namespace velk::ui {

/**
 * @brief A single-line text field: an ITextVisual that takes keyboard focus and edits its text.
 *
 * Clicking the field focuses it and starts editing. While editing, typed text goes to an
 * edit buffer and the `text` property keeps its value. Enter or losing focus commits the
 * buffer to `text`; Escape restores the buffer from `text`. Text wider than the field
 * scrolls to keep the caret in view. A text field is also an IClip, which cuts the text at
 * the element's edges.
 */
class ITextField : public Interface<ITextField>
{
public:
    VELK_INTERFACE(
        (RPROP, bool, editing, false),     ///< True while the field takes typed text.
        (EVT, on_commit, (string, text))   ///< An edit changed `text`; carries the new value.
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_TEXT_FIELD_H
