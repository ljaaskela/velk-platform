#ifndef VELK_UI_EXT_MATERIAL_SHADERS_H
#define VELK_UI_EXT_MATERIAL_SHADERS_H

#include <velk/string_view.h>

namespace velk::ui {

/**
 * @brief Shared vertex shader for eval-based rect materials.
 *
 * Any material whose visual emits `RectInstance` (gradient, texture,
 * image, standard, rounded_rect, ...) can return this from
 * `IMaterial::get_vertex_src()`. Emits the canonical superset of
 * varyings that the forward and deferred fragment drivers consume.
 * Text-like materials with custom per-instance data provide their own.
 */
inline constexpr string_view rect_material_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(RectInstanceData)
    OpaquePtr material;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_local_uv;
layout(location = 2) flat out vec2 v_size;
layout(location = 3) out vec3 v_world_pos;
layout(location = 4) out vec3 v_world_normal;
layout(location = 5) flat out uint v_shape_param;

void main()
{
    vec2 q = velk_unit_quad(gl_VertexIndex);
    RectInstance inst = root.instance_data.data[gl_InstanceIndex];
    vec4 local_pos = vec4(inst.pos + q * inst.size, 0.0, 1.0);
    vec4 world_pos = inst.world_matrix * local_pos;
    gl_Position = root.global_data.view_projection * world_pos;
    v_color = inst.color;
    v_local_uv = q;
    v_size = inst.size;
    v_world_pos = world_pos.xyz;
    // Rect convention: +Z axis of world matrix is the surface normal.
    v_world_normal = normalize(vec3(inst.world_matrix[2]));
    v_shape_param = 0u;
}
)";

} // namespace velk::ui

#endif // VELK_UI_EXT_MATERIAL_SHADERS_H
