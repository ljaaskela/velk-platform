#ifndef VELK_RENDER_INTF_MATERIAL_H
#define VELK_RENDER_INTF_MATERIAL_H

#include <velk/api/math_types.h>
#include <velk/string_view.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_program.h>

namespace velk {

class IRenderContext;
class ISurface;

/**
 * @brief Renderable material.
 *
 * Materials supply their shader sources via `IShaderSource`
 * (multi-implemented alongside `IMaterial`):
 *   - role `shader_role::kEval`     — `MaterialEval velk_eval_<name>(EvalContext)`
 *                                      snippet; path drivers compose forward /
 *                                      deferred / RT fragments around it.
 *   - role `shader_role::kVertex`   — full vertex shader.
 *   - role `shader_role::kFragment` — full fragment shader, when the material
 *                                      bypasses eval-driver composition (e.g.
 *                                      `ShaderMaterial`).
 *
 * `IMaterial` itself only carries the non-shader concerns (alpha
 * discard thresholds, bindable textures).
 *
 * Chain: IInterface -> IGpuResource -> IProgram -> IMaterial
 */
class IMaterial : public Interface<IMaterial, IProgram>
{
public:
    /// Fragments with `color.a` below this are discarded. 0 for none: a blended
    /// material's alpha is its opacity, and an opaque material's alpha is ignored.
    virtual float get_discard_threshold() const = 0;

    /// Returns the bindable textures this material samples, in slot order.
    /// The renderer resolves each to a bindless TextureId and writes them
    /// into DrawDataHeader::texture_ids[] in the same order, so shader code
    /// can index them by fixed position. Default: no textures.
    virtual vector<ISurface*> get_textures() const = 0;

    /// Roughly what colour its surface shows, for when it covers less than a pixel or light
    /// bounces off it: for a lit material its base colour, for an unlit one the colour it shows.
    virtual color get_average_albedo() const = 0;

    /// Roughly how much of its surface is metal, 0 to 1. Metal reflects its base colour only as
    /// a mirror does, so light bouncing off it diffusely takes 1 less this of it.
    virtual float get_average_metallic() const = 0;

    /// Roughly what light its surface gives off, in linear colour, for the same uses.
    virtual color get_average_emission() const = 0;

    /// Whether light falls on its surface. An unlit surface shows its colour as it is, so
    /// light bouncing off it takes that colour as given off, not reflected.
    virtual bool is_lit() const = 0;

    /// Whether its surface's colour is multiplied by its draw's instance colour and vertex
    /// colours (EvalContext::base), so that light bouncing off it takes them too.
    virtual bool is_tinted() const = 0;

    /// Where in its record (the material words a draw names) its opacity is, for a ray that hits
    /// it while it is blended: @p factor_word the word of a float factor, @p texture_word the
    /// word of a bindless texture id whose alpha, at the hit's first uv, the factor multiplies
    /// (0 there for none). False when it has no opacity rays read: it is solid to them.
    virtual bool get_opacity_words(uint32_t& factor_word, uint32_t& texture_word) const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_MATERIAL_H
