#ifndef VELK_UI_INTF_HOVER_H
#define VELK_UI_INTF_HOVER_H

#include <velk/interface/intf_metadata.h>

namespace velk::ui {

/**
 * @brief Input trait that tracks pointer hover state.
 *
 * Sets hovered to true when the pointer enters the element's bounds and
 * false when it leaves. Fires on_hover_changed on each transition.
 */
class IHover : public Interface<IHover>
{
public:
    VELK_INTERFACE(
        (RPROP, bool, hovered, false), ///< True while the pointer is over this element.
        (EVT, on_hover_changed)        ///< Fired when hovered transitions between true and false.
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_HOVER_H
