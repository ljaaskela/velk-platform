#ifndef VELK_UI_ROUNDED_RECT_VISUAL_H
#define VELK_UI_ROUNDED_RECT_VISUAL_H

#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_shader_snippet.h>
#include <velk-scene/ext/trait.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Rounded rectangle visual.
 *
 * Produces a single FillRoundedRect draw command. The renderer uses
 * an SDF fragment shader to clip corners with antialiasing.
 */
class RoundedRectVisual : public ::velk::ext::Visual2D<RoundedRectVisual,
                                                ::velk::IAnalyticShape,
                                                ::velk::IShaderSnippet>
{
public:
    VELK_CLASS_UID(ClassId::Visual::RoundedRect, "RoundedRectVisual");

    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx,
                                       const ::velk::size& bounds) override;

    // IRasterShader overrides: custom fragment for SDF corners;
    // vertex stays default.
    ::velk::ShaderSource get_raster_source(::velk::IRasterShader::Target t) const override;
    uint64_t get_raster_pipeline_key() const override;

    // IAnalyticShape: reports shape_kind 0 (a rect) with a non-empty
    // intersect source, so scene_collector knows to enable the corner
    // radius on emitted shadow/RT shapes.
    uint32_t get_shape_kind() const override { return 0; }
    string_view get_shape_intersect_source() const override;
    string_view get_shape_intersect_fn_name() const override;

    // IShaderSnippet: provides the deferred `velk_visual_discard`
    // implementation that clips the rect's rounded corners.
    string_view get_snippet_fn_name() const override;
    string_view get_snippet_source() const override;
};

} // namespace velk::ui

#endif // VELK_UI_ROUNDED_RECT_VISUAL_H
