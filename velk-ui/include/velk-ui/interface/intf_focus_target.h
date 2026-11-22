#ifndef VELK_UI_INTF_FOCUS_TARGET_H
#define VELK_UI_INTF_FOCUS_TARGET_H

#include <velk/interface/intf_interface.h>

namespace velk::ui {

/**
 * @brief An input trait that can take keyboard focus.
 *
 * Implemented next to IInputTrait. On pointer down the dispatcher gives focus to the
 * innermost element under the pointer whose input trait implements this, and clears focus
 * when there is none. Key events go to the focused element first.
 */
class IFocusTarget : public Interface<IFocusTarget>
{
public:
    /** @brief The element gained (@p focused true) or lost keyboard focus. */
    virtual void on_focus_changed(bool focused) = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_FOCUS_TARGET_H
