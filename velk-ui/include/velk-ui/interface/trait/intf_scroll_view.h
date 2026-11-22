#ifndef VELK_UI_INTF_SCROLL_VIEW_H
#define VELK_UI_INTF_SCROLL_VIEW_H

#include <velk/interface/intf_metadata.h>

namespace velk::ui {

/**
 * @brief A view onto content larger than the element.
 *
 * Each child is placed at the start of the element, at least as large as the view and as
 * large as it wants to be, and moved by the scroll offset. The view clips to its box (it is
 * an IClip), scrolls with the mouse wheel (Shift for sideways) and shows a scroll bar on an
 * axis whose content does not fit; the bar takes `bar_width` from the content's space, and
 * its thumb can be dragged. The thumb has the visual's `color`.
 */
class IScrollView : public Interface<IScrollView>
{
public:
    VELK_INTERFACE(
        (PROP, float, scroll_x, 0.f),  ///< Offset of the content, in pixels; kept within the content.
        (PROP, float, scroll_y, 0.f),
        (PROP, float, bar_width, 8.f), ///< Thickness of a scroll bar, in pixels.
        (PROP, float, wheel_step, 40.f) ///< Pixels one wheel line scrolls.
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_SCROLL_VIEW_H
