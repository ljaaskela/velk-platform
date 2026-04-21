#ifndef VELK_UI_PRIMITIVE_SHADERS_H
#define VELK_UI_PRIMITIVE_SHADERS_H

#include <velk/hash.h>
#include <velk/string_view.h>

namespace velk::ui {

// Shared pipeline for procedural primitive visuals (cube, sphere, ...).
// Hash is stable and unique so the pipeline is compiled once and reused
// by every IPrimitiveShape visual.
inline constexpr uint64_t kPrimitive3DPipelineKey = make_hash64("VelkPrimitive3D");

// Vertex shader: reads interleaved vec3 position + vec3 normal + vec2
// uv from the mesh's VBO, offsets + scales by the element instance,
// applies the element's world matrix, and emits the canonical set of
// varyings the forward/deferred fragment drivers consume.
inline constexpr string_view primitive3d_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(ElementInstanceData, VelkVbo3D)
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_local_uv;
layout(location = 2) flat out vec2 v_size;
layout(location = 3) out vec3 v_world_pos;
layout(location = 4) out vec3 v_world_normal;

void main()
{
    VelkVertex3D v = velk_vertex3d(root);
    ElementInstance inst = root.instance_data.data[gl_InstanceIndex];

    vec4 local = vec4(inst.offset.xyz + v.position * inst.size.xyz, 1.0);
    vec4 world_pos_h = inst.world_matrix * local;
    gl_Position = root.global_data.view_projection * world_pos_h;

    v_color = inst.color;
    v_local_uv = v.uv;
    v_size = inst.size.xy;
    v_world_pos = world_pos_h.xyz;
    v_world_normal = normalize(mat3(inst.world_matrix) * v.normal);
}
)";

// Fragment shader: simple Lambertian lighting from a fixed overhead
// directional light + a small ambient term. Enough to visualise
// primitive shape and orientation without needing material routing.
// Paint-material routing is a follow-up.
inline constexpr string_view primitive3d_fragment_src = R"(
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local_uv;
layout(location = 2) flat in vec2 v_size;
layout(location = 3) in vec3 v_world_pos;
layout(location = 4) in vec3 v_world_normal;

layout(location = 0) out vec4 frag_color;

void main()
{
    vec3 N = normalize(v_world_normal);
    vec3 L = normalize(vec3(0.4, 0.8, 0.6));
    float diff = max(dot(N, L), 0.0);
    float ambient = 0.25;
    float shade = ambient + (1.0 - ambient) * diff;
    frag_color = vec4(v_color.rgb * shade, v_color.a);
}
)";

} // namespace velk::ui

#endif // VELK_UI_PRIMITIVE_SHADERS_H
