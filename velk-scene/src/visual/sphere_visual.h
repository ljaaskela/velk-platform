#ifndef VELK_UI_SPHERE_VISUAL_H
#define VELK_UI_SPHERE_VISUAL_H

#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_primitive_shape.h>
#include <velk-render/interface/intf_raster_shader.h>
#include <velk-scene/ext/trait.h>
#include <velk-scene/plugin.h>

namespace velk::impl {

/**
 * @brief 3D UV sphere inscribed in the element's bounding box.
 *
 * Rasterises a procedural unit-bounds sphere mesh (radius 0.5 centered
 * at 0.5,0.5,0.5) scaled by the element's size; a non-uniform element
 * size yields a squashed ellipsoid. Tessellation is driven by
 * `subdivisions` (IPrimitiveShape); 0 uses the default of 16 segments.
 *
 * Also participates in RT via IAnalyticShape (shape_kind = 2),
 * independent of the raster path.
 */
class SphereVisual : public ext::Visual3D<SphereVisual,
                                          ::velk::IPrimitiveShape,
                                          ::velk::IAnalyticShape>
{
public:
    VELK_CLASS_UID(ClassId::Visual::Sphere, "SphereVisual");

    // IVisual
    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx,
                                       const ::velk::size& bounds) override;

    // IRasterShader
    ::velk::ShaderSource get_raster_source(::velk::IRasterShader::Target t) const override;
    uint64_t get_raster_pipeline_key() const override;

    // IAnalyticShape
    uint32_t get_shape_kind() const override { return 2; }
};

} // namespace velk::impl

#endif // VELK_UI_SPHERE_VISUAL_H
