#ifndef VELK_UI_ENV_MATERIAL_H
#define VELK_UI_ENV_MATERIAL_H

#include <velk-render/ext/material.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-ui/plugins/image/plugin.h>

namespace velk::ui::impl {

/**
 * @brief Internal interface for binding EnvMaterial parameters.
 */
class IEnvMaterialInternal : public Interface<IEnvMaterialInternal>
{
public:
    virtual void set_params(float intensity, float rotation_deg) = 0;
};

/**
 * @brief Equirectangular skybox material.
 *
 * Migrated to the eval-driver architecture. The vertex shader emits a
 * fullscreen quad and a far-plane world position; the forward driver
 * derives ray direction from that + globals.cam_pos and hands it to
 * `velk_eval_env`, which samples the equirect texture. RT path calls
 * the same eval via the composer-generated dispatch.
 */
class EnvMaterial : public ::velk::ext::Material<EnvMaterial, IEnvMaterialInternal>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::Material::Environment, "EnvMaterial");

    // IEnvMaterialInternal
    void set_params(float intensity, float rotation_deg) override;

    // IMaterial
    size_t get_draw_data_size() const override;
    ReturnValue write_draw_data(void* out, size_t size, ITextureResolver* resolver = nullptr) const override;

    string_view get_eval_src() const override;
    string_view get_eval_fn_name() const override;
    string_view get_vertex_src() const override;

private:
    float intensity_ = 1.f;
    float rotation_ = 0.f;
};

} // namespace velk::ui::impl

#endif // VELK_UI_ENV_MATERIAL_H
