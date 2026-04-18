#ifndef VELK_UI_ENV_MATERIAL_H
#define VELK_UI_ENV_MATERIAL_H

#include <velk-render/ext/material.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_shader_snippet.h>
#include <velk-ui/plugins/image/plugin.h>

namespace velk::ui::impl {

/**
 * @brief Internal interface for binding EnvMaterial parameters.
 *
 * The Environment creates one EnvMaterial via the type registry and
 * casts to this interface to set intensity/rotation. The renderer
 * never calls this directly.
 */
class IEnvMaterialInternal : public Interface<IEnvMaterialInternal>
{
public:
    virtual void set_params(float intensity, float rotation_deg) = 0;
};

/**
 * @brief Material for rendering an equirectangular environment map as a
 *        fullscreen skybox.
 *
 * Owns the skybox vertex + fragment shader. Per-draw GPU data contains
 * intensity and rotation. The inverse view-projection matrix (needed for
 * reconstructing world-space ray directions) is read from FrameGlobals
 * by the shader, keeping this material view-independent.
 *
 * Owned by the Environment, shared across all cameras using the same
 * environment (same batching model as Font-owned TextMaterial).
 */
class EnvMaterial : public ::velk::ext::Material<EnvMaterial, IEnvMaterialInternal, ::velk::IShaderSnippet>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::Material::Environment, "EnvMaterial");

    // IEnvMaterialInternal
    void set_params(float intensity, float rotation_deg) override;

    // IMaterial
    uint64_t get_pipeline_handle(IRenderContext& ctx) override;
    size_t get_draw_data_size() const override;
    ReturnValue write_draw_data(void* out, size_t size) const override;

    // IShaderSnippet
    string_view get_snippet_fn_name() const override;
    string_view get_snippet_source() const override;

private:
    float intensity_ = 1.f;
    float rotation_ = 0.f;
};

} // namespace velk::ui::impl

#endif // VELK_UI_ENV_MATERIAL_H
