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
// shader doesn't dereference them, so it uses OpaquePtr placeholders to keep
// the layout consistent without pulling in the (fragment-only) function
// definitions.
constexpr string_view text_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer TextParams {
    OpaquePtr curves;
    OpaquePtr bands;
    OpaquePtr glyphs;
};

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(TextInstanceData)
    TextParams material;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) flat out uint v_glyph_index;

void main()
{
    vec2 q = velk_unit_quad(gl_VertexIndex);
    TextInstance inst = root.instance_data.data[gl_InstanceIndex];
    vec4 local_pos = vec4(inst.pos + q * inst.size, 0.0, 1.0);
    gl_Position = root.global_data.view_projection * inst.world_matrix * local_pos;
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
layout(buffer_reference, std430) buffer TextParams {
    VelkTextCurveBuffer curves;
    VelkTextBandBuffer bands;
    VelkTextGlyphBuffer glyphs;
};

layout(buffer_reference, std430) buffer DrawData {
    VELK_DRAW_DATA(OpaquePtr)
    TextParams material;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uint v_glyph_index;
layout(location = 0) out vec4 frag_color;

void main()
{
    float coverage = velk_text_coverage(v_uv, v_glyph_index, root.material.curves, root.material.bands, root.material.glyphs);
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
static_assert(sizeof(TextMaterialData) == 32, "TextMaterialData must be 32 bytes");

// Deferred vertex: same transform as the forward path but also emits
// world_pos + world_normal so the gbuffer has usable geometry data.
// Material slot is OpaquePtr since the vertex doesn't dereference it.
constexpr string_view text_deferred_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(TextInstanceData)
    OpaquePtr material;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) flat out uint v_glyph_index;
layout(location = 3) out vec3 v_world_pos;
layout(location = 4) out vec3 v_world_normal;

void main()
{
    vec2 q = velk_unit_quad(gl_VertexIndex);
    TextInstance inst = root.instance_data.data[gl_InstanceIndex];
    vec4 local_pos = vec4(inst.pos + q * inst.size, 0.0, 1.0);
    vec4 world_pos_h = inst.world_matrix * local_pos;
    gl_Position = root.global_data.view_projection * world_pos_h;
    v_color = inst.color;
    v_uv = vec2(q.x, 1.0 - q.y);
    v_glyph_index = inst.glyph_index;
    v_world_pos = world_pos_h.xyz;
    v_world_normal = normalize(vec3(inst.world_matrix[2]));
}
)";

// Deferred fragment: TextVisual's visual_discard (spliced in by the
// batch_builder composer) drops fragments below a coverage threshold
// before the body runs. Surviving fragments write the glyph's color
// into the G-buffer tagged as Unlit - text doesn't participate in PBR
// lighting, deferred compute passes albedo through unchanged.
constexpr string_view text_deferred_fragment_src = R"(
#version 450
#include "velk.glsl"
#include "velk_text.glsl"

layout(buffer_reference, std430) buffer TextParams {
    VelkTextCurveBuffer curves;
    VelkTextBandBuffer bands;
    VelkTextGlyphBuffer glyphs;
};

layout(buffer_reference, std430) buffer DrawData {
    VELK_DRAW_DATA(OpaquePtr)
    TextParams material;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uint v_glyph_index;
layout(location = 3) in vec3 v_world_pos;
layout(location = 4) in vec3 v_world_normal;

layout(location = 0) out vec4 g_albedo;
layout(location = 1) out vec4 g_normal;
layout(location = 2) out vec4 g_world_pos;
layout(location = 3) out vec4 g_material;

void velk_visual_discard();

void main()
{
    velk_visual_discard();
    g_albedo    = v_color;
    g_normal    = vec4(normalize(v_world_normal), 0.0);
    g_world_pos = vec4(v_world_pos, 0.0);
    g_material  = vec4(0.0, 0.0, 0.0 /*Unlit*/, 0.0);
}
)";

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

        // TODO: A proper place for this would be some kind of plugin init but we'd need IRenderContext for
        // that.
        ctx.register_shader_include("velk_text.glsl", embedded::velk_text_glsl);
    }
    return ensure_pipeline(ctx, text_fragment_src, text_vertex_src);
}

size_t TextMaterial::get_draw_data_size() const
{
    return sizeof(TextMaterialData);
}

ReturnValue TextMaterial::write_draw_data(void* out, size_t size) const
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

namespace {
// Fill snippet for the compute ray tracer. Wraps slug's velk_text_coverage
// to produce alpha-modulated coverage; the hit's base color (the glyph's
// TextInstance color) passes through via `base`.
//
// The compute shader's hit_uv has y=0 at the top of the glyph rect, while
// velk_text_coverage expects FreeType's Y-up convention (y=0 at descender).
// The flip mirrors the y = 1 - q.y that the raster vertex shader applies.
constexpr string_view text_fill_src = R"(
// Compute shaders don't have fragment-quad derivatives, so fwidth() isn't
// available. Override it with a fixed per-pixel uv estimate before
// pulling in velk_text.glsl. This fixes AA quality to a single glyph
// scale (good around 32px tall glyphs); a per-shape derivative would
// come from a CPU-side pixel-footprint estimate in a later pass.
#define fwidth(x) vec2(1.0 / 32.0)
#include "velk_text.glsl"

// Non-readonly to match velk_text_coverage's parameters (GLSL refuses to
// drop a readonly memory qualifier across a function boundary).
layout(buffer_reference, std430) buffer TextMaterialData {
    VelkTextCurveBuffer curves;
    VelkTextBandBuffer bands;
    VelkTextGlyphBuffer glyphs;
};

BrdfSample velk_fill_text(FillContext ctx)
{
    TextMaterialData d = TextMaterialData(ctx.data_addr);
    vec2 glyph_uv = vec2(ctx.uv.x, 1.0 - ctx.uv.y);
    float coverage = velk_text_coverage(glyph_uv, ctx.shape_param, d.curves, d.bands, d.glyphs);
    BrdfSample bs;
    bs.emission = vec4(ctx.base.rgb, ctx.base.a * coverage);
    bs.throughput = vec3(0.0);
    bs.next_dir = vec3(0.0);
    bs.terminate = true;
    return bs;
}
)";
} // namespace

string_view TextMaterial::get_snippet_fn_name() const
{
    return "velk_fill_text";
}

string_view TextMaterial::get_snippet_source() const
{
    return text_fill_src;
}

void TextMaterial::register_snippet_includes(IRenderContext& ctx) const
{
    ctx.register_shader_include("velk_text.glsl", embedded::velk_text_glsl);
}

ShaderSource TextMaterial::get_raster_source(IRasterShader::Target t) const
{
    if (t == IRasterShader::Target::Deferred) {
        return {text_deferred_vertex_src, text_deferred_fragment_src};
    }
    return {};
}

} // namespace velk::ui
