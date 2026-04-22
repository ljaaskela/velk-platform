#ifndef VELK_UI_SPHERE_VISUAL_H
#define VELK_UI_SPHERE_VISUAL_H

#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_primitive_shape.h>
#include <velk-render/interface/intf_raster_shader.h>
#include <velk-ui/ext/trait.h>
#include <velk-ui/interface/intf_mesh_visual.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

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
class SphereVisual : public ext::Visual<SphereVisual,
                                        IMeshVisual,
                                        ::velk::IPrimitiveShape,
                                        ::velk::IAnalyticShape>
{
public:
    VELK_CLASS_UID(ClassId::Visual::Sphere, "SphereVisual");

    // IVisual
    vector<DrawEntry> get_draw_entries(const ::velk::size& bounds) override;

    // IMeshVisual
    ::velk::IMesh::Ptr get_mesh(::velk::IRenderContext& ctx) const override;

    // IRasterShader
    ::velk::ShaderSource get_raster_source(::velk::IRasterShader::Target t) const override;
    uint64_t get_raster_pipeline_key() const override;

    // IAnalyticShape
    uint32_t get_shape_kind() const override { return 2; }

private:
    mutable IMesh::Ptr mesh_;
};

} // namespace velk::ui

#endif // VELK_UI_SPHERE_VISUAL_H
