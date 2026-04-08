#include "text_material.h"

#include "../embedded/velk_text_glsl.h"

#include <velk/api/state.h>
#include <velk-render/gpu_data.h>

#include <cstring>

namespace velk::ui {

namespace {

// Vertex shader for analytic-Bezier text. Reads TextInstance per glyph,
// expands a unit quad to glyph bbox size, passes through (uv with Y flip,
// glyph index, color) to the fragment stage. The three buffer references
// for curves/bands/glyphs sit in the DrawData material slot but the vertex
// shader doesn't dereference them, so it uses Ptr64 placeholders to keep
// the layout consistent without pulling in the (fragment-only) function
// definitions.
constexpr string_view text_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(TextInstanceData)
    Ptr64 curves;
    Ptr64 bands;
    Ptr64 glyphs;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) flat out uint v_glyph_index;

void main()
{
    vec2 q = velk_unit_quad(gl_VertexIndex);
    TextInstance inst = root.instance_data.data[gl_InstanceIndex];
    vec2 world_pos = inst.pos + q * inst.size;
    gl_Position = root.global_data.view_projection * vec4(world_pos, 0.0, 1.0);
    v_color = inst.color;
    // Quad uv has y=0 at top; glyph curves use FreeType's Y-up convention,
    // so we want uv.y=1 at the top of the glyph quad.
    v_uv = vec2(q.x, 1.0 - q.y);
    v_glyph_index = inst.glyph_index;
}
)";

// Fragment shader. Includes velk_text.glsl which defines the curve/band/
// glyph buffer reference types and the velk_text_coverage function. The
// DrawData layout uses the typed buffer references in the same slots that
// the vertex shader exposes as Ptr64 placeholders; both std430 layouts
// alias to the same memory.
constexpr string_view text_fragment_src = R"(
#version 450
#include "velk.glsl"
#include "velk_text.glsl"

// Intentionally NOT `readonly`: the curves/bands/glyphs fields are passed
// to velk_text_coverage as function arguments, and GLSL refuses to drop a
// readonly memory qualifier when crossing the call boundary into a
// non-readonly parameter. (We can't add readonly to the parameters either
// because spirv-val rejects NonWritable on PhysicalStorageBuffer pointer
// parameters.) The buffers are still effectively read-only because nothing
// in this shader writes to them.
layout(buffer_reference, std430) buffer DrawData {
    VELK_DRAW_DATA(Ptr64)
    VelkTextCurveBuffer curves;
    VelkTextBandBuffer bands;
    VelkTextGlyphBuffer glyphs;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uint v_glyph_index;
layout(location = 0) out vec4 frag_color;

void main()
{
    float coverage = velk_text_coverage(v_uv, v_glyph_index, root.curves, root.bands, root.glyphs);
    frag_color = vec4(v_color.rgb, v_color.a * coverage);
}
)";

// Material data layout: three 8-byte buffer addresses, written via
// memcpy from the IBuffer::get_gpu_address() values. Std430 places each
// uint64_t at an 8-byte boundary, so the three addresses pack to 24 bytes.
VELK_GPU_STRUCT TextMaterialData
{
    uint64_t curves_address;
    uint64_t bands_address;
    uint64_t glyphs_address;
};
// static_assert(sizeof(TextMaterialData) == 24, "TextMaterialData must be 24 bytes");

} // namespace

void TextMaterial::set_font_buffers(IBuffer::Ptr curves, IBuffer::Ptr bands, IBuffer::Ptr glyphs)
{
    curves_ = std::move(curves);
    bands_  = std::move(bands);
    glyphs_ = std::move(glyphs);
}

uint64_t TextMaterial::get_pipeline_handle(IRenderContext& ctx)
{
    if (!has_pipeline_handle()) {
        // Register the slug coverage GLSL include before compiling so the
        // fragment shader's #include "velk_text.glsl" resolves. Idempotent.
        ctx.register_shader_include("velk_text.glsl", string_view(embedded::velk_text_glsl));
    }
    return ensure_pipeline(ctx, text_fragment_src, text_vertex_src);
}

size_t TextMaterial::gpu_data_size() const
{
    return sizeof(TextMaterialData);
}

ReturnValue TextMaterial::write_gpu_data(void* out, size_t size) const
{
    if (size != sizeof(TextMaterialData)) {
        return ReturnValue::Fail;
    }
    TextMaterialData data{};
    data.curves_address = curves_ ? curves_->get_gpu_address() : 0;
    data.bands_address  = bands_  ? bands_->get_gpu_address()  : 0;
    data.glyphs_address = glyphs_ ? glyphs_->get_gpu_address() : 0;
    std::memcpy(out, &data, sizeof(data));
    return ReturnValue::Success;
}

} // namespace velk::ui
