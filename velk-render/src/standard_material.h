#ifndef VELK_RENDER_STANDARD_MATERIAL_H
#define VELK_RENDER_STANDARD_MATERIAL_H

#include <velk-render/ext/material.h>
#include <velk-render/interface/intf_raster_shader.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_shader_snippet.h>
#include <velk-render/interface/intf_standard.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief First-cut glTF metallic-roughness material.
 *
 * Raster path: a solid base-colour fill (PBR shading requires environment
 * sampling and reflection rays we only have in the RT path).
 *
 * RT path: full stochastic shading via the composed compute shader.
 *   diffuse  = base * (1 - metallic) * env(normal)
 *   specular = GGX-sampled reflection ray traced back into the scene
 *              (falls back to env on miss or depth exhaustion)
 *   final    = kD * diffuse + F * specular
 * Fresnel-Schlick blends between them.
 */
class StandardMaterial : public ::velk::ext::Material<StandardMaterial, IStandard, IShaderSnippet, IRasterShader>
{
public:
    VELK_CLASS_UID(::velk::ClassId::StandardMaterial, "StandardMaterial");

    uint64_t get_pipeline_handle(IRenderContext& ctx) override;
    size_t get_draw_data_size() const override;
    ReturnValue write_draw_data(void* out, size_t size) const override;

    string_view get_snippet_fn_name() const override;
    string_view get_snippet_source() const override;

    ShaderSource get_raster_source(IRasterShader::Target t) const override;
};

} // namespace velk::impl

#endif // VELK_RENDER_STANDARD_MATERIAL_H
