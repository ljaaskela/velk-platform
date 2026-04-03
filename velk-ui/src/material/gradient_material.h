#ifndef VELK_UI_GRADIENT_MATERIAL_H
#define VELK_UI_GRADIENT_MATERIAL_H

#include <velk/ext/object.h>

#include <velk-ui/interface/intf_gradient.h>
#include <velk-ui/interface/intf_material.h>
#include <velk-ui/interface/intf_render_context.h>
#include <velk-ui/plugin.h>

namespace velk_ui {

/**
 * @brief Built-in linear gradient material.
 *
 * Lazily compiles a gradient shader on first use via create_shader_material().
 * Provides gradient parameters (start_color, end_color, angle) as GPU data
 * that the shader reads via buffer_reference.
 */
class GradientMaterial : public velk::ext::Object<GradientMaterial, IMaterial, IGradient>
{
public:
    VELK_CLASS_UID(ClassId::Material::Gradient, "GradientMaterial");

    uint64_t get_pipeline_handle(IRenderContext& ctx) override;
    size_t get_gpu_data(void* out, size_t max_size) const override;

private:
    IMaterial::Ptr shader_mat_;
};

} // namespace velk_ui

#endif // VELK_UI_GRADIENT_MATERIAL_H
