#include "text_material.h"

#include "../embedded/velk_text_glsl.h"

#include <velk/api/state.h>
#include <velk-render/gpu_data.h>

#include <cstring>

namespace velk::ui {

namespace {

// Material data layout: three 8-byte buffer addresses, written via
// memcpy from the IBuffer::get_gpu_address() values. Std430 places each
// uint64_t at an 8-byte boundary, so the three addresses pack to 24 bytes.
VELK_GPU_STRUCT TextMaterialData
{
    uint64_t curves_address;
    uint64_t bands_address;
    uint64_t glyphs_address;
};
static_assert(sizeof(TextMaterialData) == 32, "TextMaterialData must be 32 bytes");

// Eval body: glyph coverage from the slug buffers. Compute shaders
// have no fragment-quad derivatives; override fwidth there with a
// fixed per-pixel estimate so velk_text.glsl compiles cleanly. Raster
// keeps the native fwidth for proper AA at varied scales.
constexpr string_view text_eval_src = R"(
#ifdef VELK_COMPUTE
#define fwidth(x) vec2(1.0 / 32.0)
#endif
#include "velk_text.glsl"

layout(buffer_reference, std430) buffer TextMaterialData {
    VelkTextCurveBuffer curves;
    VelkTextBandBuffer bands;
    VelkTextGlyphBuffer glyphs;
};

MaterialEval velk_eval_text(EvalContext ctx)
{
    TextMaterialData d = TextMaterialData(ctx.data_addr);
    // Glyph curves use FreeType's Y-up convention (y=0 at descender,
    // y=1 at ascender). ctx.uv arrives Y-down from raster varyings /
    // RT intersect_rect; flip here so both paths hit the same glyph
    // space.
    vec2 glyph_uv = vec2(ctx.uv.x, 1.0 - ctx.uv.y);
    float coverage = velk_text_coverage(glyph_uv, ctx.shape_param,
                                         d.curves, d.bands, d.glyphs);
    MaterialEval e = velk_default_material_eval();
    e.color = vec4(ctx.base.rgb, ctx.base.a * coverage);
    e.normal = ctx.normal;
    return e;
}
)";

} // namespace

void TextMaterial::set_font_buffers(IBuffer::Ptr curves, IBuffer::Ptr bands, IBuffer::Ptr glyphs)
{
    curves_ = std::move(curves);
    bands_  = std::move(bands);
    glyphs_ = std::move(glyphs);
}

size_t TextMaterial::get_draw_data_size() const
{
    return sizeof(TextMaterialData);
}

ReturnValue TextMaterial::write_draw_data(void* out, size_t size, ITextureResolver*) const
{
    if (size == sizeof(TextMaterialData)) {
        auto& p = *static_cast<TextMaterialData*>(out);
        p.curves_address = get_gpu_address(curves_);
        p.bands_address = get_gpu_address(bands_);
        p.glyphs_address = get_gpu_address(glyphs_);
        return ReturnValue::Success;
    }
    return ReturnValue::Fail;
}

string_view TextMaterial::get_eval_src() const
{
    return text_eval_src;
}

string_view TextMaterial::get_eval_fn_name() const
{
    return "velk_eval_text";
}


void TextMaterial::register_eval_includes(IRenderContext& ctx) const
{
    ctx.register_shader_include("velk_text.glsl", embedded::velk_text_glsl);
}

} // namespace velk::ui
