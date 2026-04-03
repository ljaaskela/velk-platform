#ifndef VELK_UI_DEFAULT_SHADERS_H
#define VELK_UI_DEFAULT_SHADERS_H

namespace velk_ui {

// All shaders use buffer_reference to access data via GPU pointers.
// A single push constant carries the 8-byte GPU address of the DrawDataHeader.
// Shaders dereference this to reach globals, instances, and material params.
// Geometry is procedural: 6 vertices per quad (triangle list).
//
// IMPORTANT: Instance types are plain structs, NOT buffer_reference.
// Only types that represent actual GPU pointers use buffer_reference.

// ============================================================================
// Rect
// ============================================================================

inline const char* rect_vertex_src = R"(
#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require

layout(buffer_reference, std430) readonly buffer Globals {
    mat4 projection;
    vec4 viewport;
};

struct RectInstance {
    vec2 pos;
    vec2 size;
    vec4 color;
};

layout(buffer_reference, std430) readonly buffer RectInstances {
    RectInstance data[];
};

layout(buffer_reference, std430) readonly buffer DrawData {
    Globals globals;
    RectInstances instances;
    uint texture_id;
    uint instance_count;
};

layout(push_constant) uniform PC { DrawData root; };

const vec2 kQuad[6] = vec2[6](
    vec2(0, 0), vec2(1, 0), vec2(1, 1),
    vec2(0, 0), vec2(1, 1), vec2(0, 1)
);

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

inline const char* rect_fragment_src = R"(
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

inline const char* text_vertex_src = R"(
#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require

layout(buffer_reference, std430) readonly buffer Globals {
    mat4 projection;
    vec4 viewport;
};

struct TextInstance {
    vec2 pos;
    vec2 size;
    vec4 color;
    vec2 uv_min;
    vec2 uv_max;
};

layout(buffer_reference, std430) readonly buffer TextInstances {
    TextInstance data[];
};

layout(buffer_reference, std430) readonly buffer DrawData {
    Globals globals;
    TextInstances instances;
    uint texture_id;
    uint instance_count;
};

layout(push_constant) uniform PC { DrawData root; };

const vec2 kQuad[6] = vec2[6](
    vec2(0, 0), vec2(1, 0), vec2(1, 1),
    vec2(0, 0), vec2(1, 1), vec2(0, 1)
);

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

inline const char* text_fragment_src = R"(
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

inline const char* rounded_rect_vertex_src = R"(
#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require

layout(buffer_reference, std430) readonly buffer Globals {
    mat4 projection;
    vec4 viewport;
};

struct RectInstance {
    vec2 pos;
    vec2 size;
    vec4 color;
};

layout(buffer_reference, std430) readonly buffer RectInstances {
    RectInstance data[];
};

layout(buffer_reference, std430) readonly buffer DrawData {
    Globals globals;
    RectInstances instances;
    uint texture_id;
    uint instance_count;
};

layout(push_constant) uniform PC { DrawData root; };

const vec2 kQuad[6] = vec2[6](
    vec2(0, 0), vec2(1, 0), vec2(1, 1),
    vec2(0, 0), vec2(1, 1), vec2(0, 1)
);

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

inline const char* rounded_rect_fragment_src = R"(
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

} // namespace velk_ui

#endif // VELK_UI_DEFAULT_SHADERS_H
