#ifndef VELK_UI_CUBE_VISUAL_H
#define VELK_UI_CUBE_VISUAL_H

#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_primitive_shape.h>
#include <velk-render/interface/intf_raster_shader.h>
#include <velk-scene/ext/trait.h>
#include <velk-scene/plugin.h>

namespace velk::impl {

/**
 * @brief 3D axis-aligned box visual.
 *
 * Rasterises a procedural unit-bounds cube mesh, scaled by the
 * element's size and positioned by its world matrix. The mesh itself
 * is owned by the render context's IMeshBuilder, keyed by
 * `subdivisions` (IPrimitiveShape) so repeated cubes share one
 * allocation.
 *
 * Also participates in RT via IAnalyticShape (shape_kind = 1), using
 * the existing `intersect_cube` dispatch — independent of the raster
 * path.
 */
class CubeVisual : public ext::Visual3D<CubeVisual,
                                        ::velk::IPrimitiveShape,
                                        ::velk::IAnalyticShape>
{
public:
    VELK_CLASS_UID(ClassId::Visual::Cube, "CubeVisual");

    // IVisual
    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx,
                                       const ::velk::size& bounds) override;

    // IRasterShader
    ::velk::ShaderSource get_raster_source(::velk::IRasterShader::Target t) const override;
    uint64_t get_raster_pipeline_key() const override;

    // IAnalyticShape
    uint32_t get_shape_kind() const override { return 1; }
};

} // namespace velk::impl

#endif // VELK_UI_CUBE_VISUAL_H
