#ifndef VELK_RENDER_EXT_ELEMENT_VERTEX_H
#define VELK_RENDER_EXT_ELEMENT_VERTEX_H

#include <velk/string_view.h>

namespace velk::ext {

/**
 * @brief Shared vertex shader used by every visual's default path.
 *
 * Reads a `VelkVertex3D` from the bound VBO, applies
 * `inst.offset + v.position * inst.size` in element-local space, and
 * transforms through the element's `world_matrix` and the camera's
 * view-projection. Emits the canonical set of varyings the forward
 * and deferred fragment drivers consume.
 *
 * Every visual with a paint-material routes through this by default
 * (via `ext::Material::get_vertex_src()`). Materials that need a
 * different vertex path (e.g. env material's clip-space fullscreen
 * pass) override `get_vertex_src()` with their own source.
 *
 * The unit quad (4-vertex TriangleStrip, z=0, normal +Z) + 3D meshes
 * (cube, sphere, glTF) all use this one shader. 2D visuals carry
 * `size.z = 0`; mesh visuals carry full xyz extents.
 */
inline constexpr string_view element_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(ElementInstanceData, VelkVbo3D)
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
    VelkVertex3D v = velk_vertex3d(root);
    ElementInstance inst = root.instance_data.data[gl_InstanceIndex];

    vec4 local = vec4(inst.offset.xyz + v.position * inst.size.xyz, 1.0);
    vec4 world_h = inst.world_matrix * local;
    gl_Position = root.global_data.view_projection * world_h;

    v_color        = inst.color;
    v_local_uv     = v.uv;
    v_size         = inst.size.xy;
    v_world_pos    = world_h.xyz;
    v_world_normal = normalize(mat3(inst.world_matrix) * v.normal);
    v_shape_param  = inst.params.x;
}
)";

} // namespace velk::ext

#endif // VELK_RENDER_EXT_ELEMENT_VERTEX_H
