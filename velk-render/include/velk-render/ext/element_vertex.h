#ifndef VELK_RENDER_EXT_ELEMENT_VERTEX_H
#define VELK_RENDER_EXT_ELEMENT_VERTEX_H

#include <velk/string.h>
#include <velk/string_view.h>

namespace velk::ext {

#define VELK_ELEMENT_VERTEX_HEADER "#version 450\n#include \"velk.glsl\"\n#include \"velk-ui.glsl\"\n"

#define VELK_ELEMENT_VERTEX_BODY R"(
#ifndef VELK_VERTEX_FN
#define VELK_VERTEX_FN velk_default_varyings
#endif

VELK_DRAW_DATA(root)

VELK_VARYINGS_OUT

void main()
{
    GlobalData globals = velk_global_data(root);
    VelkVaryings s = VELK_VERTEX_FN(velk_vertex3d(root), velk_instance(root), globals);
    gl_Position = globals.view_projection * vec4(s.rel_pos, 1.0);

    v_color        = s.color;
    v_local_uv     = s.local_uv;
    v_size         = s.size;
    v_rel_pos      = s.rel_pos;
    v_world_normal = s.world_normal;
    v_shape_param  = s.shape_param;
    v_uv1          = velk_uv1(root);
    v_world_tangent = s.world_tangent;
    v_lod_error     = velk_lod_error(root);
    v_instance      = uint(gl_InstanceIndex);
}
)"

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
 * (via `ext::Material`'s default `get_source(kVertex)`). Materials
 * that need a different vertex path in forward only (e.g. env material's
 * clip-space fullscreen pass) override `get_source(kVertex)` to return their
 * own. Materials moving their vertices use velk_vertex_shader_with.
 *
 * The unit quad (4-vertex TriangleStrip, z=0, normal +Z) + 3D meshes
 * (cube, sphere, glTF) all use this one shader. 2D visuals carry
 * `size.z = 0`; mesh visuals carry full xyz extents.
 */
inline constexpr string_view element_vertex_src = VELK_ELEMENT_VERTEX_HEADER VELK_ELEMENT_VERTEX_BODY;

/**
 * @brief The element vertex shader, getting each vertex's varyings from a material's
 *        `shader_role::kVertexFunction` snippet @p function named @p name.
 */
inline string velk_vertex_shader_with(string_view function, string_view name)
{
    string out(VELK_ELEMENT_VERTEX_HEADER);
    out.append(function);
    out.append(string_view("\n#define VELK_VERTEX_FN "));
    out.append(name);
    out.append(string_view(VELK_ELEMENT_VERTEX_BODY));
    return out;
}

} // namespace velk::ext

#endif // VELK_RENDER_EXT_ELEMENT_VERTEX_H
