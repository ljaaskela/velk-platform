#ifndef VELK_UI_CUBE_VISUAL_H
#define VELK_UI_CUBE_VISUAL_H

#include <velk-ui/ext/trait.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief 3D axis-aligned box in the element's local frame.
 *
 * Renders nothing in the rasterizer path (for now — mesh rendering is a
 * future addition). In the RT path, the renderer emits an RtShape with
 * shape_kind = 1 (cube), extent pulled from element.size (width, height,
 * depth) and orientation from the element's world matrix.
 */
class CubeVisual : public ext::Visual<CubeVisual>
{
public:
    VELK_CLASS_UID(ClassId::Visual::Cube, "CubeVisual");

    vector<DrawEntry> get_draw_entries(const rect&) override { return {}; }
    uint32_t get_shape_kind() const override { return 1; }
};

} // namespace velk::ui

#endif // VELK_UI_CUBE_VISUAL_H
