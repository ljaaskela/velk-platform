#ifndef VELK_UI_SPHERE_VISUAL_H
#define VELK_UI_SPHERE_VISUAL_H

#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-ui/ext/trait.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief 3D sphere inscribed in the element's bounding box.
 *
 * Radius is min(size.width, size.height, size.depth) / 2. Renders nothing
 * in the rasterizer path (for now — mesh or impostor rendering later). In
 * the RT path, the renderer emits an RtShape with shape_kind = 2 (sphere),
 * center at the element's AABB centroid, radius from the minimum extent.
 */
class SphereVisual : public ext::Visual<SphereVisual, ::velk::IAnalyticShape>
{
public:
    VELK_CLASS_UID(ClassId::Visual::Sphere, "SphereVisual");

    vector<DrawEntry> get_draw_entries(const rect&) override { return {}; }

    // IAnalyticShape
    uint32_t get_shape_kind() const override { return 2; }
};

} // namespace velk::ui

#endif // VELK_UI_SPHERE_VISUAL_H
