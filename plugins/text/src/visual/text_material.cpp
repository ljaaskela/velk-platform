#include "text_material.h"

#include "../embedded/velk_text_glsl.h"

#include <velk/api/state.h>
#include <velk-render/gpu_data.h>

#include <cstring>

namespace velk::ui {

namespace {

// Material data layout: element bases into the three shared text arenas
// (set = 1 slots 7-9). Plain uints, so the record is modellable and the
// compute path reads it by index like every other material.
VELK_GPU_STRUCT TextMaterialData
{
    uint32_t curve_base;
    uint32_t band_base;
    uint32_t glyph_base;
    uint32_t _pad;
};
static_assert(sizeof(TextMaterialData) == 16, "TextMaterialData must be 16 bytes");

// Eval body: glyph coverage from the slug buffers. Compute shaders
// have no fragment-quad derivatives; override fwidth there with a
// fixed per-pixel estimate so velk_text.glsl compiles cleanly. Raster
// keeps the native fwidth for proper AA at varied scales.
constexpr string_view text_eval_src = R"(
#ifdef VELK_COMPUTE
#define fwidth(x) vec2(1.0 / 32.0)
#endif
#include "velk_text.glsl"

struct TextMaterialData {
    uint curve_base;
    uint band_base;
    uint glyph_base;
    uint _pad;
};
VELK_MATERIAL(TextMaterialData)

MaterialEval velk_eval_text(EvalContext ctx)
{
    TextMaterialData d = VELK_LOAD_MATERIAL(TextMaterialData, ctx);
    // Glyph curves use FreeType's Y-up convention (y=0 at descender,
    // y=1 at ascender). ctx.uv arrives Y-down from raster varyings /
    // RT intersect_rect; flip here so both paths hit the same glyph
    // space.
    vec2 glyph_uv = vec2(ctx.uv.x, 1.0 - ctx.uv.y);
    float coverage = velk_text_coverage(glyph_uv, ctx.shape_param,
                                        d.curve_base, d.band_base, d.glyph_base);
    MaterialEval e = velk_default_material_eval();
    e.color = vec4(ctx.base.rgb, ctx.base.a * coverage);
    e.normal = ctx.normal;
    return e;
}
)";

} // namespace

void TextMaterial::set_font(IFont* font)
{
    font_ = font;
}

size_t TextMaterial::get_draw_data_size() const
{
    return sizeof(TextMaterialData);
}

ReturnValue TextMaterial::write_draw_data(void* out, size_t size, ITextureResolver*) const
{
    if (size == sizeof(TextMaterialData)) {
        auto& p = *static_cast<TextMaterialData*>(out);
        p.curve_base = font_ ? font_->curve_base() : 0u;
        p.band_base  = font_ ? font_->band_base() : 0u;
        p.glyph_base = font_ ? font_->glyph_base() : 0u;
        p._pad = 0u;
        return ReturnValue::Success;
    }
    return ReturnValue::Fail;
}

string_view TextMaterial::get_source(string_view role) const
{
    if (role == ::velk::shader_role::kEval) return text_eval_src;
    return Base::get_source(role);
}

string_view TextMaterial::get_fn_name(string_view role) const
{
    if (role == ::velk::shader_role::kEval) return "velk_eval_text";
    return {};
}

void TextMaterial::register_includes(IRenderContext& ctx) const
{
    ctx.shaders().register_include("velk_text.glsl", embedded::velk_text_glsl);
}

} // namespace velk::ui
