#ifndef VELK_UI_ROUNDED_RECT_VISUAL_H
#define VELK_UI_ROUNDED_RECT_VISUAL_H

#include <velk-ui/ext/trait.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Rounded rectangle visual.
 *
 * Produces a single FillRoundedRect draw command. The renderer uses
 * an SDF fragment shader to clip corners with antialiasing.
 */
class RoundedRectVisual : public ext::Visual<RoundedRectVisual>
{
public:
    VELK_CLASS_UID(ClassId::Visual::RoundedRect, "RoundedRectVisual");

    vector<DrawEntry> get_draw_entries(const rect& bounds) override;
    uint64_t get_pipeline_key() const override;
    string_view get_fragment_src() const override;
    string_view get_intersect_src() const override;
};

} // namespace velk::ui

#endif // VELK_UI_ROUNDED_RECT_VISUAL_H
