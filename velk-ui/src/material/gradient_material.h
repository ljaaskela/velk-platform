#ifndef VELK_UI_GRADIENT_MATERIAL_H
#define VELK_UI_GRADIENT_MATERIAL_H

#include <velk-render/ext/material.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-ui/interface/intf_gradient.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Built-in linear gradient material.
 *
 * Migrated to the eval-driver architecture: provides just a
 * `velk_eval_gradient` body; the framework generates forward /
 * deferred / RT-fill variants from the shared driver templates.
 */
class GradientMaterial : public ::velk::ext::Material<GradientMaterial, IGradient>
{
public:
    VELK_CLASS_UID(ClassId::Material::Gradient, "GradientMaterial");

    size_t get_draw_data_size() const override;
    ReturnValue write_draw_data(void* out, size_t size, ITextureResolver* resolver = nullptr) const override;

    // IMaterial — eval-driver overrides.
    string_view get_eval_src() const override;
    string_view get_eval_fn_name() const override;
};

} // namespace velk::ui

#endif // VELK_UI_GRADIENT_MATERIAL_H
