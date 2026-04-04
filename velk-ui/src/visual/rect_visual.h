#ifndef VELK_UI_RECT_VISUAL_H
#define VELK_UI_RECT_VISUAL_H

#include <velk-ui/ext/visual.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

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
    vector<DrawEntry> get_draw_entries(const rect& bounds) override;
};

} // namespace velk::ui

#endif // VELK_UI_RECT_VISUAL_H
