#include "env_material.h"

#include <velk-render/gpu_data.h>

#include <cstring>

namespace velk::ui::impl {

namespace {

VELK_GPU_STRUCT EnvGpuData
{
    float intensity;
    float rotation_rad;
    float _pad[2];
};

// Env vertex: fullscreen quad with gl_Position already in clip space.
// v_world_pos is a far-plane world point reconstructed via
// inverse_view_projection so the forward driver's ray_dir computation
// (normalize(v_world_pos - cam_pos)) yields the correct skybox
// direction. Other canonical varyings are set to defaults.
constexpr string_view env_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(OpaquePtr, VelkVbo3D)
    OpaquePtr material;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_local_uv;
layout(location = 2) flat out vec2 v_size;
layout(location = 3) out vec3 v_world_pos;
layout(location = 4) out vec3 v_world_normal;

void main()
{
    // Env draws as a fullscreen quad in clip space — read xy from
    // the 3D unit-quad VBO (z = 0).
    vec2 q = velk_vertex3d(root).position.xy;
    // Fullscreen clip-space quad at z=1 (far plane).
    gl_Position = vec4(q * 2.0 - 1.0, 1.0, 1.0);

    // Reconstruct the corresponding world-space point on the far plane.
    // Y is flipped to match the rest of the pipeline's viewport.
    vec2 ndc = vec2(q.x * 2.0 - 1.0, (1.0 - q.y) * 2.0 - 1.0);
    vec4 far_pt = root.global_data.inverse_view_projection * vec4(ndc, 1.0, 1.0);
    v_world_pos = far_pt.xyz / far_pt.w;

    v_color = vec4(0.0);
    v_local_uv = q;
    v_size = vec2(0.0);
    v_world_normal = vec3(0.0);
    v_shape_param = 0u;
}
)";

// Env eval: rotate incoming ray direction around Y, convert to
// equirectangular UV, sample the env texture.
constexpr string_view env_eval_src = R"(
layout(buffer_reference, std430) readonly buffer EnvMaterialData {
    vec4 params; // x = intensity, y = rotation_rad, zw unused
};

MaterialEval velk_eval_env(EvalContext ctx)
{
    const float PI = 3.14159265358979323846;
    EnvMaterialData d = EnvMaterialData(ctx.data_addr);

    float c = cos(d.params.y);
    float s = sin(d.params.y);
    vec3 dir = vec3(c * ctx.ray_dir.x + s * ctx.ray_dir.z,
                    ctx.ray_dir.y,
                    -s * ctx.ray_dir.x + c * ctx.ray_dir.z);

    float u = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;
    float v = asin(clamp(dir.y, -1.0, 1.0)) / PI + 0.5;
    vec3 rgb = velk_texture(ctx.texture_id, vec2(u, v)).rgb;

    MaterialEval e = velk_default_material_eval();
    e.color = vec4(rgb * d.params.x, 1.0);
    e.normal = ctx.normal;
    return e;
}
)";

} // namespace

void EnvMaterial::set_params(float intensity, float rotation_deg)
{
    intensity_ = intensity;
    rotation_ = rotation_deg * 0.01745329251994329577f; // deg to rad
}

size_t EnvMaterial::get_draw_data_size() const
{
    return sizeof(EnvGpuData);
}

ReturnValue EnvMaterial::write_draw_data(void* out, size_t size, ITextureResolver*) const
{
    return set_material<EnvGpuData>(out, size, [&](auto& p) {
        p.intensity = intensity_;
        p.rotation_rad = rotation_;
    });
}

string_view EnvMaterial::get_eval_src() const
{
    return env_eval_src;
}

string_view EnvMaterial::get_eval_fn_name() const
{
    return "velk_eval_env";
}

string_view EnvMaterial::get_vertex_src() const
{
    return env_vertex_src;
}

} // namespace velk::ui::impl
