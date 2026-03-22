#ifndef VELK_UI_RECT_VISUAL_H
#define VELK_UI_RECT_VISUAL_H

#include <velk-ui/ext/visual.h>
#include <velk-ui/plugin.h>

namespace velk_ui {

/**
 * @brief Solid color rectangle visual.
 *
 * Produces a single FillRect draw command that fills the element's bounds
 * with the visual's color.
 */
class RectVisual : public ext::Visual<RectVisual>
{
public:
    VELK_CLASS_UID(ClassId::Visual::Rect, "RectVisual");

    // IVisual
    velk::vector<DrawCommand> get_draw_commands(const velk::rect& bounds) override;
};

} // namespace velk_ui

#endif // VELK_UI_RECT_VISUAL_H
