#ifndef VELK_UI_DEFAULT_SHADERS_H
#define VELK_UI_DEFAULT_SHADERS_H

namespace velk_ui {

// All shaders #include "velk_common.glsl" which is resolved at compile time
// by the shader compiler's virtual include system. This provides:
//   u_projection (mat4), u_rect (vec4), VERTEX_INDEX, SAMPLE_ATLAS(uv)
// with backend-appropriate declarations (uniforms for GL, push constants for Vulkan).

inline const char* rect_vertex_src = R"(
#version 450
#include "velk_common.glsl"

layout(location = 0) in vec4 inst_rect;
layout(location = 1) in vec4 inst_color;

layout(location = 0) out vec4 v_color;

void main()
{
    vec2 pos = vec2(float(VERTEX_INDEX & 1), float(VERTEX_INDEX >> 1));
    vec2 world_pos = inst_rect.xy + pos * inst_rect.zw;
    gl_Position = u_projection * vec4(world_pos, 0.0, 1.0);
    v_color = inst_color;
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

inline const char* text_vertex_src = R"(
#version 450
#include "velk_common.glsl"

layout(location = 0) in vec4 inst_rect;
layout(location = 1) in vec4 inst_color;
layout(location = 2) in vec4 inst_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main()
{
    vec2 pos = vec2(float(VERTEX_INDEX & 1), float(VERTEX_INDEX >> 1));
    vec2 world_pos = inst_rect.xy + pos * inst_rect.zw;
    gl_Position = u_projection * vec4(world_pos, 0.0, 1.0);
    v_color = inst_color;
    v_uv.x = mix(inst_uv.x, inst_uv.z, pos.x);
    v_uv.y = mix(inst_uv.y, inst_uv.w, pos.y);
}
)";

inline const char* text_fragment_src = R"(
#version 450
#include "velk_common.glsl"

#ifdef VELK_VULKAN
#extension GL_EXT_nonuniform_qualifier : enable
layout(set = 0, binding = 0) uniform sampler2D velk_textures[];
#define SAMPLE_ATLAS(uv) texture(velk_textures[u_texture_index], uv)
#else
layout(binding = 0) uniform sampler2D u_atlas;
#define SAMPLE_ATLAS(uv) texture(u_atlas, uv)
#endif

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

void main()
{
    float alpha = SAMPLE_ATLAS(v_uv).r;
    frag_color = vec4(v_color.rgb, v_color.a * alpha);
}
)";

inline const char* rounded_rect_vertex_src = R"(
#version 450
#include "velk_common.glsl"

layout(location = 0) in vec4 inst_rect;
layout(location = 1) in vec4 inst_color;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_local_uv;

void main()
{
    vec2 pos = vec2(float(VERTEX_INDEX & 1), float(VERTEX_INDEX >> 1));
    vec2 world_pos = inst_rect.xy + pos * inst_rect.zw;
    gl_Position = u_projection * vec4(world_pos, 0.0, 1.0);
    v_color = inst_color;
    v_local_uv = pos;
}
)";

inline const char* rounded_rect_fragment_src = R"(
#version 450
#include "velk_common.glsl"

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local_uv;
layout(location = 0) out vec4 frag_color;

float rounded_rect_sdf(vec2 p, vec2 half_size, float radius)
{
    vec2 d = abs(p) - half_size + radius;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
}

void main()
{
    vec2 size = u_rect.zw;
    float radius = min(min(size.x, size.y) * 0.5, 12.0);
    vec2 half_size = size * 0.5;
    vec2 p = (v_local_uv - 0.5) * size;

    float dist = rounded_rect_sdf(p, half_size, radius);
    float alpha = 1.0 - smoothstep(-0.5, 0.5, dist);

    if (alpha < 0.001) discard;
    frag_color = vec4(v_color.rgb, v_color.a * alpha);
}
)";

} // namespace velk_ui

#endif // VELK_UI_DEFAULT_SHADERS_H
