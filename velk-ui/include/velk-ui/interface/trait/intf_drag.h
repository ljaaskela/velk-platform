#ifndef VELK_UI_INTF_DRAG_H
#define VELK_UI_INTF_DRAG_H

#include <velk/interface/intf_metadata.h>

namespace velk::ui {

/**
 * @brief Input trait that tracks pointer drag gestures.
 *
 * Captures the pointer on press and tracks movement. Fires on_drag_start
 * on the first move after press, on_drag_move on subsequent moves, and
 * on_drag_end on release.
 */
class IDrag : public Interface<IDrag>
{
public:
    VELK_INTERFACE(
        (RPROP, bool, dragging, false), ///< True while a drag gesture is active.
        (EVT, on_drag_start),           ///< Fired on the first move after pointer down.
        (EVT, on_drag_move),            ///< Fired on each subsequent move during a drag.
        (EVT, on_drag_end)              ///< Fired when the pointer is released after dragging.
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_DRAG_H
