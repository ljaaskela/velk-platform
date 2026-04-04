#ifndef VELK_RENDER_DEFAULT_SHADERS_H
#define VELK_RENDER_DEFAULT_SHADERS_H

#include <velk/string_view.h>

namespace velk {

// Built-in shaders use:
//   #include "velk.glsl"    - framework types (Globals, Ptr64)
//   #include "velk-ui.glsl" - UI instance types (RectInstance, TextInstance, kQuad)

// ============================================================================
// Rect
// ============================================================================

[[maybe_unused]] constexpr string_view rect_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    Globals globals;
    RectInstances instances;
    uint texture_id;
    uint instance_count;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec4 v_color;

void main()
{
    vec2 q = kQuad[gl_VertexIndex];
    RectInstance inst = root.instances.data[gl_InstanceIndex];
    vec2 world_pos = inst.pos + q * inst.size;
    gl_Position = root.globals.projection * vec4(world_pos, 0.0, 1.0);
    v_color = inst.color;
}
)";

[[maybe_unused]] constexpr string_view rect_fragment_src = R"(
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = v_color;
}
)";

// ============================================================================
// Text
// ============================================================================

[[maybe_unused]] constexpr string_view text_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    Globals globals;
    TextInstances instances;
    uint texture_id;
    uint instance_count;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) flat out uint v_texture_id;

void main()
{
    vec2 q = kQuad[gl_VertexIndex];
    TextInstance inst = root.instances.data[gl_InstanceIndex];
    vec2 world_pos = inst.pos + q * inst.size;
    gl_Position = root.globals.projection * vec4(world_pos, 0.0, 1.0);
    v_color = inst.color;
    v_uv = mix(inst.uv_min, inst.uv_max, q);
    v_texture_id = root.texture_id;
}
)";

[[maybe_unused]] constexpr string_view text_fragment_src = R"(
#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(set = 0, binding = 0) uniform sampler2D velk_textures[];

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uint v_texture_id;
layout(location = 0) out vec4 frag_color;

void main()
{
    float alpha = texture(velk_textures[nonuniformEXT(v_texture_id)], v_uv).r;
    frag_color = vec4(v_color.rgb, v_color.a * alpha);
}
)";

// ============================================================================
// Rounded rect
// ============================================================================

[[maybe_unused]] constexpr string_view rounded_rect_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    Globals globals;
    RectInstances instances;
    uint texture_id;
    uint instance_count;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_local_uv;
layout(location = 2) flat out vec2 v_size;

void main()
{
    vec2 q = kQuad[gl_VertexIndex];
    RectInstance inst = root.instances.data[gl_InstanceIndex];
    vec2 world_pos = inst.pos + q * inst.size;
    gl_Position = root.globals.projection * vec4(world_pos, 0.0, 1.0);
    v_color = inst.color;
    v_local_uv = q;
    v_size = inst.size;
}
)";

[[maybe_unused]] constexpr string_view rounded_rect_fragment_src = R"(
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local_uv;
layout(location = 2) flat in vec2 v_size;
layout(location = 0) out vec4 frag_color;

float rounded_rect_sdf(vec2 p, vec2 half_size, float radius)
{
    vec2 d = abs(p) - half_size + radius;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
}

void main()
{
    float radius = min(min(v_size.x, v_size.y) * 0.5, 12.0);
    vec2 half_size = v_size * 0.5;
    vec2 p = (v_local_uv - 0.5) * v_size;

    float dist = rounded_rect_sdf(p, half_size, radius);
    float alpha = 1.0 - smoothstep(-0.5, 0.5, dist);

    if (alpha < 0.001) discard;
    frag_color = vec4(v_color.rgb, v_color.a * alpha);
}
)";

} // namespace velk

#endif // VELK_RENDER_DEFAULT_SHADERS_H
