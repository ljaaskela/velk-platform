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

constexpr string_view env_vertex_src = R"(
#version 450
#include "velk.glsl"

layout(buffer_reference, std430) readonly buffer EnvInstanceData {
    vec4 dummy[];
};

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(EnvInstanceData)
    float intensity;
    float rotation_rad;
    float _pad0;
    float _pad1;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec2 v_uv;

void main()
{
    // Fullscreen quad from vertex index: covers clip space [-1,1].
    vec2 q = velk_unit_quad(gl_VertexIndex);
    gl_Position = vec4(q * 2.0 - 1.0, 1.0, 1.0);
    v_uv = q;
}
)";

constexpr string_view env_fragment_src = R"(
#version 450
#extension GL_EXT_nonuniform_qualifier : enable
#include "velk.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(Ptr64)
    float intensity;
    float rotation_rad;
    float _pad0;
    float _pad1;
};

layout(push_constant) uniform PC { DrawData root; };

layout(set = 0, binding = 0) uniform sampler2D velk_textures[];

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

const float PI = 3.14159265358979323846;

void main()
{
    // Reconstruct view-ray direction from screen UV using the inverse VP
    // from FrameGlobals (view-independent material, view-dependent data
    // lives in globals). Two points (near/far) subtracted for a correct
    // direction regardless of camera translation.
    vec2 ndc = vec2(v_uv.x * 2.0 - 1.0, (1.0 - v_uv.y) * 2.0 - 1.0);
    vec4 near_pt = root.global_data.inverse_view_projection * vec4(ndc, -1.0, 1.0);
    vec4 far_pt  = root.global_data.inverse_view_projection * vec4(ndc,  1.0, 1.0);
    vec3 dir = normalize(far_pt.xyz / far_pt.w - near_pt.xyz / near_pt.w);

    // Apply Y-axis rotation.
    float c = cos(root.rotation_rad);
    float s = sin(root.rotation_rad);
    dir = vec3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);

    // Equirectangular UV from direction.
    float u = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;
    float v = asin(clamp(dir.y, -1.0, 1.0)) / PI + 0.5;

    vec3 color = texture(velk_textures[nonuniformEXT(root.texture_id)], vec2(u, v)).rgb;
    frag_color = vec4(color * root.intensity, 1.0);
}
)";

} // namespace

void EnvMaterial::set_params(float intensity, float rotation_deg)
{
    intensity_ = intensity;
    rotation_ = rotation_deg * 0.01745329251994329577f; // deg to rad
}

uint64_t EnvMaterial::get_pipeline_handle(IRenderContext& ctx)
{
    return ensure_pipeline(ctx, env_fragment_src, env_vertex_src);
}

size_t EnvMaterial::gpu_data_size() const
{
    return sizeof(EnvGpuData);
}

ReturnValue EnvMaterial::write_gpu_data(void* out, size_t size) const
{
    return set_material<EnvGpuData>(out, size, [&](auto& p) {
        p.intensity = intensity_;
        p.rotation_rad = rotation_;
    });
}

namespace {
constexpr string_view env_fill_src = R"(
layout(buffer_reference, std430) readonly buffer EnvMaterialData {
    vec4 params; // x = intensity, y = rotation_rad
};

BrdfSample velk_fill_env(FillContext ctx)
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
    vec3 rgb = texture(velk_textures[nonuniformEXT(ctx.texture_id)], vec2(u, v)).rgb;
    BrdfSample bs;
    bs.emission = vec4(rgb * d.params.x, 1.0);
    bs.throughput = vec3(0.0);
    bs.next_dir = vec3(0.0);
    bs.terminate = true;
    return bs;
}
)";
} // namespace

string_view EnvMaterial::get_fill_src() const
{
    return env_fill_src;
}

string_view EnvMaterial::get_fill_fn_name() const
{
    return "velk_fill_env";
}

string_view EnvMaterial::get_fill_include_name() const
{
    return "velk_env.glsl";
}

} // namespace velk::ui::impl
