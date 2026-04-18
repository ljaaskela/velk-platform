#ifndef VELK_RENDER_DEFAULT_SHADERS_H
#define VELK_RENDER_DEFAULT_SHADERS_H

#include <velk/string_view.h>

namespace velk {

// The default vertex/fragment shader pair used when a visual or material
// does not supply its own. The vertex shader expands the unit quad to a
// world-space rect and emits the varyings every known consumer reads:
//   location 0: v_color    (vec4) - instance color
//   location 1: v_local_uv (vec2) - unit quad coords, 0..1 across the rect
//   location 2: v_size     (vec2, flat) - the rect's world-space size
// Fragments that do not read v_local_uv / v_size simply ignore them.
//
// Built-in shaders use:
//   #include "velk.glsl"    - framework types (GlobalData, OpaquePtr, velk_unit_quad, VELK_DRAW_DATA)
//   #include "velk-ui.glsl" - UI instance types (RectInstance, TextInstance)

[[maybe_unused]] constexpr string_view default_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(RectInstanceData)
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_local_uv;
layout(location = 2) flat out vec2 v_size;

void main()
{
    vec2 q = velk_unit_quad(gl_VertexIndex);
    RectInstance inst = root.instance_data.data[gl_InstanceIndex];
    vec4 local_pos = vec4(inst.pos + q * inst.size, 0.0, 1.0);
    gl_Position = root.global_data.view_projection * inst.world_matrix * local_pos;
    v_color = inst.color;
    v_local_uv = q;
    v_size = inst.size;
}
)";

[[maybe_unused]] constexpr string_view default_fragment_src = R"(
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = v_color;
}
)";

// Default vertex shader for the deferred G-buffer fill. Mirrors
// default_vertex_src but additionally emits world_pos and world_normal
// varyings that G-buffer fragment shaders need to populate attachments
// 1 (normal) and 2 (world pos). Normal is the +Z axis of the instance's
// world matrix (rect convention: normal is the third column); for cube
// / sphere shapes this only matches for flat faces, and those primitives
// are expected to provide their own G-buffer vertex shader.
[[maybe_unused]] constexpr string_view default_gbuffer_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(RectInstanceData)
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_local_uv;
layout(location = 2) flat out vec2 v_size;
layout(location = 3) out vec3 v_world_pos;
layout(location = 4) out vec3 v_world_normal;

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
}
)";

// Default fragment shader for the deferred G-buffer fill. Writes the
// instance color straight through to albedo, marks the fragment as
// "unlit" so the compute lighting pass passes albedo through unchanged.
// Materials that want lighting (StandardMaterial) override this.
[[maybe_unused]] constexpr string_view default_gbuffer_fragment_src = R"(
#version 450

// Canonical deferred varyings; the default gbuffer vertex emits the
// full set and every fragment reads a subset.
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local_uv;
layout(location = 2) flat in vec2 v_size;
layout(location = 3) in vec3 v_world_pos;
layout(location = 4) in vec3 v_world_normal;

// Canonical G-buffer attachments (see velk-render/gbuffer.h).
layout(location = 0) out vec4 g_albedo;         // rgba: surface albedo
layout(location = 1) out vec4 g_normal;         // xyz: world normal
layout(location = 2) out vec4 g_world_pos;      // xyz: world position
layout(location = 3) out vec4 g_material;       // r: metallic, g: roughness, b: lighting_mode, a: _

// Forward decl; the batch_builder composer appends either the visual's
// discard snippet or an empty stub after this fragment's body.
void velk_visual_discard();

void main()
{
    velk_visual_discard();
    g_albedo      = v_color;
    g_normal      = vec4(normalize(v_world_normal), 0.0);
    g_world_pos   = vec4(v_world_pos, 0.0);
    g_material    = vec4(0.0, 0.5, 1.0 / 255.0 /*Standard*/, 0.0);
}
)";

// Default compute shader for the deferred lighting pass. Samples the
// G-buffer attachments + light buffer and writes the shaded color to
// the per-view output storage image.
//
// Shader-side responsibilities:
//   - Unlit path: pass albedo through unchanged.
//   - Standard path: evaluate Lambertian diffuse from each analytic
//     light (directional / point / spot) with distance + cone falloff.
//   - Shadow modulation lands in a later slice (B.3.d) alongside the
//     shared `velk_eval_shadow` composer.
[[maybe_unused]] constexpr string_view default_deferred_compute_src = R"(
#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 1, rgba8) uniform writeonly image2D gStorageImages[];

// Mirrors C++ GpuLight (80 bytes) in ray_tracer.cpp / deferred_lighter.cpp.
struct Light {
    uvec4 flags;           // x = type (0 dir, 1 point, 2 spot), y = shadow_tech_id, zw = _
    vec4  position;        // xyz world position (point / spot)
    vec4  direction;       // xyz world forward (dir / spot)
    vec4  color_intensity; // rgb colour, a intensity
    vec4  params;          // x range, y cos(inner), z cos(outer), w _
};

layout(buffer_reference, std430) readonly buffer LightList { Light data[]; };

layout(buffer_reference, std430) readonly buffer _EnvParamsBuf {
    vec4 params; // x = intensity, y = rotation_rad, zw = _
};

layout(buffer_reference, std430) readonly buffer _InvVpBuf {
    mat4 inv_vp;
};

// Mirrors the RT prelude's RtShape (128 bytes). Only geometric fields
// are populated by DeferredLighter; material_id / _data_addr / texture_id
// stay zero because shadow rays don't consult them.
struct RtShape {
    vec4 origin;
    vec4 u_axis;
    vec4 v_axis;
    vec4 w_axis;
    vec4 color;
    vec4 params;
    uint material_id;
    uint texture_id;
    uint shape_param;
    uint shape_kind;
    uint64_t material_data_addr;
    uint64_t _tail_pad;
};

layout(buffer_reference, std430) readonly buffer ShapeList { RtShape data[]; };

layout(push_constant) uniform PC {
    vec4 cam_pos;              // offset 0
    uint output_image_id;      // 16
    uint albedo_tex_id;        // 20
    uint normal_tex_id;        // 24
    uint worldpos_tex_id;      // 28
    uint material_tex_id;      // 32
    uint width;                // 36
    uint height;               // 40
    uint light_count;          // 44
    uint env_texture_id;       // 48
    uint _env_pad;             // 52
    uint shape_count;          // 56
    uint _shape_pad;           // 60
    LightList lights;          // 64
    _EnvParamsBuf env_params;  // 72
    _InvVpBuf inv_vp_buf;      // 80
    ShapeList shapes;          // 88
} pc;

// ===== Shadow ray support (duplicated from rt_compute_prelude_src) =====
// Extract to a shared include when a second shadow technique arrives
// and deferred needs composed dispatch.
struct Ray { vec3 origin; vec3 dir; };
struct RayHit { float t; vec2 uv; vec3 normal; uint shape_index; };

bool intersect_rect_d(Ray ray, RtShape shape, out RayHit hit)
{
    vec3 u_axis = shape.u_axis.xyz;
    vec3 v_axis = shape.v_axis.xyz;
    vec3 origin = shape.origin.xyz;
    float radius = shape.params.x;
    vec3 normal = cross(u_axis, v_axis);
    float nlen2 = dot(normal, normal);
    if (nlen2 < 1e-12) return false;
    float inv_nlen = inversesqrt(nlen2);
    vec3 n = normal * inv_nlen;
    float denom = dot(ray.dir, n);
    if (abs(denom) < 1e-6) return false;
    float t = dot(origin - ray.origin, n) / denom;
    if (t <= 0.0) return false;
    vec3 p = ray.origin + t * ray.dir;
    vec3 local = p - origin;
    float u_len2 = dot(u_axis, u_axis);
    float v_len2 = dot(v_axis, v_axis);
    float s = dot(local, u_axis) / u_len2;
    float tt = dot(local, v_axis) / v_len2;
    if (s < 0.0 || s > 1.0 || tt < 0.0 || tt > 1.0) return false;
    if (radius > 0.0) {
        float u_len = sqrt(u_len2);
        float v_len = sqrt(v_len2);
        vec2 size_w = vec2(u_len, v_len);
        vec2 p_w = vec2(s * u_len, tt * v_len);
        vec2 half_size = size_w * 0.5;
        vec2 centered = p_w - half_size;
        vec2 d = abs(centered) - half_size + radius;
        float sdf = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
        if (sdf > 0.0) return false;
    }
    hit.t = t;
    hit.uv = vec2(s, tt);
    hit.normal = n;
    return true;
}

bool intersect_cube_d(Ray ray, RtShape shape, out RayHit hit)
{
    vec3 U = shape.u_axis.xyz;
    vec3 V = shape.v_axis.xyz;
    vec3 W = shape.w_axis.xyz;
    float u_len2 = dot(U, U);
    float v_len2 = dot(V, V);
    float w_len2 = dot(W, W);
    if (u_len2 < 1e-12 || v_len2 < 1e-12 || w_len2 < 1e-12) return false;
    vec3 rel = ray.origin - shape.origin.xyz;
    vec3 ro_l = vec3(dot(rel, U) / u_len2, dot(rel, V) / v_len2, dot(rel, W) / w_len2);
    vec3 rd_l = vec3(dot(ray.dir, U) / u_len2, dot(ray.dir, V) / v_len2, dot(ray.dir, W) / w_len2);
    vec3 inv_d = 1.0 / rd_l;
    vec3 t0 = (vec3(0.0) - ro_l) * inv_d;
    vec3 t1 = (vec3(1.0) - ro_l) * inv_d;
    vec3 tmin_v = min(t0, t1);
    vec3 tmax_v = max(t0, t1);
    float tmin = max(max(tmin_v.x, tmin_v.y), tmin_v.z);
    float tmax = min(min(tmax_v.x, tmax_v.y), tmax_v.z);
    if (tmax < max(tmin, 0.0)) return false;
    float t = tmin > 0.0 ? tmin : tmax;
    if (t <= 0.0) return false;
    hit.t = t;
    hit.uv = vec2(0.0);
    hit.normal = vec3(0.0, 0.0, 1.0);
    return true;
}

bool intersect_sphere_d(Ray ray, RtShape shape, out RayHit hit)
{
    vec3 center = shape.origin.xyz
                + 0.5 * (shape.u_axis.xyz + shape.v_axis.xyz + shape.w_axis.xyz);
    float radius = shape.params.x;
    if (radius <= 0.0) return false;
    vec3 oc = ray.origin - center;
    float a = dot(ray.dir, ray.dir);
    float b = dot(oc, ray.dir);
    float c = dot(oc, oc) - radius * radius;
    float disc = b * b - a * c;
    if (disc < 0.0) return false;
    float sq = sqrt(disc);
    float t1 = (-b - sq) / a;
    float t2 = (-b + sq) / a;
    float t = t1 > 0.0 ? t1 : t2;
    if (t <= 0.0) return false;
    hit.t = t;
    hit.uv = vec2(0.0);
    hit.normal = normalize((ray.origin + t * ray.dir) - center);
    return true;
}

bool intersect_shape_d(Ray ray, RtShape shape, out RayHit hit)
{
    if (shape.shape_kind == 1u) return intersect_cube_d(ray, shape, hit);
    if (shape.shape_kind == 2u) return intersect_sphere_d(ray, shape, hit);
    return intersect_rect_d(ray, shape, hit);
}

bool trace_closest_hit_d(Ray ray, out RayHit hit)
{
    hit.t = 1e30;
    hit.shape_index = 0xffffffffu;
    for (uint i = 0u; i < pc.shape_count; ++i) {
        RtShape s = pc.shapes.data[i];
        RayHit h;
        if (intersect_shape_d(ray, s, h)) {
            if (h.t < hit.t) { hit = h; hit.shape_index = i; }
        }
    }
    return hit.shape_index != 0xffffffffu;
}

// RT shadow: one occlusion ray against the scene's shape buffer.
// Mirrors rt_shadow.cpp's velk_shadow_rt.
float velk_shadow_rt(uint light_idx, vec3 world_pos, vec3 world_normal)
{
    Light light = pc.lights.data[light_idx];
    vec3 L;
    float t_max;
    if (light.flags.x == 0u) {
        L = -light.direction.xyz;
        t_max = 1e30;
    } else {
        vec3 to_light = light.position.xyz - world_pos;
        t_max = length(to_light);
        if (t_max < 1e-6) return 1.0;
        L = to_light / t_max;
    }
    vec3 n = normalize(world_normal);
    Ray r;
    r.origin = world_pos + n * 0.5;
    r.dir    = L;
    RayHit hit;
    if (!trace_closest_hit_d(r, hit)) return 1.0;
    return (hit.t >= t_max) ? 1.0 : 0.0;
}

// Shadow dispatch. Currently only tech_id = 1 (rt_shadow) is wired; any
// other tech falls through to fully-lit. Extend via composition when
// more techniques land.
float velk_eval_shadow(uint tech_id, uint light_idx, vec3 world_pos, vec3 world_normal)
{
    if (tech_id == 1u) return velk_shadow_rt(light_idx, world_pos, world_normal);
    return 1.0;
}

// Equirect env sample along rd, applying rotation and intensity.
// Returns vec3(0) when the view has no environment.
vec3 env_miss_color(vec3 rd)
{
    if (pc.env_texture_id == 0u) return vec3(0.0);
    const float PI = 3.14159265358979323846;
    vec4 params = pc.env_params.params;
    float c = cos(params.y);
    float s = sin(params.y);
    vec3 dir = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
    float u = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;
    float v = asin(clamp(dir.y, -1.0, 1.0)) / PI + 0.5;
    return velk_texture(pc.env_texture_id, vec2(u, v)).rgb * params.x;
}

// GGX normal distribution, Smith geometry, Schlick Fresnel. Same forms
// used by the RT path's velk_fill_standard.
float ggx_d(float NdotH, float a)
{
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265358979323846 * denom * denom, 1e-6);
}

float smith_g1(float NdotX, float a)
{
    float k = (a + 1.0); k = k * k * 0.125; // (a+1)^2 / 8, Schlick-GGX for direct
    return NdotX / max(NdotX * (1.0 - k) + k, 1e-6);
}

float smith_g(float NdotV, float NdotL, float a)
{
    return smith_g1(NdotV, a) * smith_g1(NdotL, a);
}

vec3 fresnel_schlick(float cos_theta, vec3 F0)
{
    return F0 + (vec3(1.0) - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;

    vec2 uv = (vec2(coord) + 0.5) / vec2(float(pc.width), float(pc.height));
    vec4 albedo    = velk_texture(pc.albedo_tex_id, uv);
    vec3 world_n   = velk_texture(pc.normal_tex_id, uv).xyz;
    vec3 world_pos = velk_texture(pc.worldpos_tex_id, uv).xyz;
    vec4 mat       = velk_texture(pc.material_tex_id, uv);

    // Sky path: pixels with no G-buffer coverage (cleared to zero,
    // including normal) reconstruct a world-space view ray and sample
    // the environment. Falls back to black when the view has no env.
    if (dot(world_n, world_n) < 1e-6) {
        vec2 ndc = uv * 2.0 - 1.0;
        mat4 inv_vp = pc.inv_vp_buf.inv_vp;
        vec4 near_h = inv_vp * vec4(ndc, 0.0, 1.0);
        vec4 far_h  = inv_vp * vec4(ndc, 1.0, 1.0);
        vec3 near_w = near_h.xyz / near_h.w;
        vec3 far_w  = far_h.xyz  / far_h.w;
        vec3 rd = normalize(far_w - near_w);
        vec3 sky = env_miss_color(rd);
        imageStore(gStorageImages[nonuniformEXT(pc.output_image_id)], coord, vec4(sky, 1.0));
        return;
    }

    // LightingMode encoded in mat.b (0 = Unlit, 1 = Standard, ...).
    uint lighting_mode = uint(mat.b * 255.0 + 0.5);
    float metallic  = clamp(mat.r, 0.0, 1.0);
    float roughness = clamp(mat.g, 0.04, 1.0);

    vec3 rgb;
    if (lighting_mode == 0u) {
        // Unlit: emit albedo as-is.
        rgb = albedo.rgb;
    } else {
        vec3 N = normalize(world_n);
        vec3 V = normalize(pc.cam_pos.xyz - world_pos);
        float NdotV = max(dot(N, V), 0.0);

        vec3 F0 = mix(vec3(0.04), albedo.rgb, metallic);
        float a = roughness * roughness;

        // Direct lighting: sum Lambertian diffuse + analytic GGX specular
        // per light. Fresnel evaluated at the half-vector.
        vec3 direct_diffuse  = vec3(0.0);
        vec3 direct_specular = vec3(0.0);
        for (uint i = 0u; i < pc.light_count; ++i) {
            Light light = pc.lights.data[i];
            vec3 L;
            float atten = 1.0;
            if (light.flags.x == 0u) {
                L = -light.direction.xyz;
            } else {
                vec3 to_light = light.position.xyz - world_pos;
                float dist = length(to_light);
                L = to_light / max(dist, 1e-6);
                float range = max(light.params.x, 1e-6);
                float t = clamp(1.0 - dist / range, 0.0, 1.0);
                atten = t * t;
                if (light.flags.x == 2u) {
                    float cos_a = dot(-L, light.direction.xyz);
                    atten *= smoothstep(light.params.z, light.params.y, cos_a);
                }
            }
            float NdotL = max(dot(N, L), 0.0);
            if (NdotL <= 0.0 || atten <= 0.0) continue;
            float shadow = velk_eval_shadow(light.flags.y, i, world_pos, N);
            if (shadow <= 0.0) continue;
            vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * atten * shadow;

            direct_diffuse += albedo.rgb * NdotL * radiance;

            vec3 H = normalize(L + V);
            float NdotH = max(dot(N, H), 0.0);
            float VdotH = max(dot(V, H), 0.0);
            float D = ggx_d(NdotH, a);
            float G = smith_g(NdotV, NdotL, roughness);
            vec3  F = fresnel_schlick(VdotH, F0);
            vec3 spec = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-6);
            direct_specular += spec * NdotL * radiance;
        }

        // Env lighting: single-sample approximation. Diffuse reads along
        // N; specular reads along the mirror reflection (no roughness
        // prefilter yet, so rough surfaces get a sharper reflection than
        // physically correct).
        vec3 env_diffuse  = env_miss_color(N);
        vec3 env_specular = env_miss_color(reflect(-V, N));
        vec3 F_env = fresnel_schlick(NdotV, F0);
        vec3 kD_env = (vec3(1.0) - F_env) * (1.0 - metallic);

        rgb = direct_diffuse * (1.0 - metallic)
            + direct_specular
            + kD_env * albedo.rgb * env_diffuse
            + F_env * env_specular;
    }

    imageStore(gStorageImages[nonuniformEXT(pc.output_image_id)], coord, vec4(rgb, albedo.a));
}
)";

// Fullscreen composite pipeline: samples the deferred output texture
// and alpha-blends it onto the surface. One triangle covering NDC
// [-1, 3]; viewport-scaled by the view's subrect so the pass only
// writes where that view owns pixels. Push constants carry the source
// bindless texture id.
[[maybe_unused]] constexpr string_view deferred_composite_vertex_src = R"(
#version 450

layout(location = 0) out vec2 v_uv;

void main()
{
    // Standard fullscreen-triangle trick.
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    gl_Position = vec4(pos, 0.0, 1.0);
    v_uv = (pos + 1.0) * 0.5;
}
)";

[[maybe_unused]] constexpr string_view deferred_composite_fragment_src = R"(
#version 450
#include "velk.glsl"

layout(push_constant) uniform PC {
    uint src_tex_id;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = velk_texture(pc.src_tex_id, v_uv);
}
)";

// Compute ray tracer prelude. Fixed header that precedes all material
// snippets. Declares extensions, storage-image binding, shape struct,
// push constants, intersect_rect, and the full stochastic-RT toolkit
// (RNG, GGX sampling, trace_ray). velk_resolve_fill is forward-declared
// so trace_ray can call it; the composer generates its definition after
// all material snippets and before main.
[[maybe_unused]] constexpr string_view rt_compute_prelude_src = R"(
#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 1, rgba8) uniform writeonly image2D gStorageImages[];

struct RtShape {
    vec4 origin;        // xyz = world origin (corner for rect/cube, AABB corner for sphere)
    vec4 u_axis;        // xyz = local x axis, scaled by width
    vec4 v_axis;        // xyz = local y axis, scaled by height
    vec4 w_axis;        // xyz = local z axis, scaled by depth (cube only; zero otherwise)
    vec4 color;         // rgba base color (used when material_id == 0)
    vec4 params;        // x = corner radius (rect) or sphere radius; yzw reserved
    uint material_id;   // 0 = no material, use color; otherwise dispatched via switch
    uint texture_id;    // bindless texture index, 0 when unused
    uint shape_param;   // per-shape material data (e.g. glyph index for text)
    uint shape_kind;    // 0 = rect, 1 = cube, 2 = sphere
    uint64_t material_data_addr;
    uint64_t _tail_pad;
};

layout(buffer_reference, std430) readonly buffer ShapeList {
    RtShape data[];
};

// Scene light. Mirrors the C++ GpuLight struct (80 bytes).
struct Light {
    uvec4 flags;           // x = type (0=dir, 1=point, 2=spot), y = shadow_tech_id, zw = _
    vec4  position;        // xyz = world position (point / spot)
    vec4  direction;       // xyz = world forward axis (directional / spot)
    vec4  color_intensity; // rgb = colour, a = intensity multiplier
    vec4  params;          // x = range, y = cos(inner), z = cos(outer), w = _
};

layout(buffer_reference, std430) readonly buffer LightList {
    Light data[];
};

layout(push_constant) uniform PC {
    mat4 inv_view_projection;
    vec4 cam_pos;
    uvec4 extras;       // x=image_index, y=width, z=height, w=shape_count
    uvec4 env;          // x=env_material_id, y=env_texture_id, z=frame_counter, w=_
    ShapeList shapes;
    uint64_t env_data_addr;
    LightList lights;
    uint light_count;
    uint _lights_pad;
} pc;

// ===== Core ray / hit / fill-context types =====
struct Ray {
    vec3 origin;
    vec3 dir;
};

struct RayHit {
    float t;
    vec2 uv;
    vec3 normal;
    uint shape_index; // only set by trace_closest_hit
};

// Everything a material fill function receives. Bundling keeps the call
// sites small and makes adding new per-hit context cheap.
struct FillContext {
    uint64_t data_addr;    // material's per-draw GPU data
    uint texture_id;       // bindless texture slot (0 if unused)
    uint shape_param;      // per-shape material slot (e.g. glyph index)
    vec2 uv;               // hit uv (0..1 across the shape)
    vec4 base;             // shape base color
    vec3 ray_dir;          // incoming ray direction
    vec3 normal;           // surface normal at hit
    vec3 hit_pos;          // world-space hit point (undefined for env miss)
};

// Result of evaluating a material at a hit. GLSL forbids recursion, so
// materials cannot call trace_ray; instead they return the local emission
// they produce at this hit plus (optionally) a bounced ray for the
// iterative trace loop in main() to continue with.
//
//   emission    rgb = color contributed at this hit (already weighted for
//               alpha at primary); a = opacity for primary compositing.
//   throughput  multiplier applied to the next bounce's contribution.
//   next_dir    world-space direction of the next ray (ignored if
//               terminate == true).
//   terminate   true = no further bounces (flat UI material, text, env).
struct BrdfSample {
    vec4 emission;
    vec3 throughput;
    vec3 next_dir;
    bool terminate;
};

// ===== Shape intersection =====
// Three primitive kinds live in the prelude: rect (planar quad), cube
// (oriented box), sphere (centered in AABB). Visuals with a non-standard
// shape could still use IVisual::get_intersect_src() to contribute a
// snippet, but the RT path doesn't compose those yet.

// Ray vs. oriented rect parameterised by (origin + s*u_axis + t*v_axis) for
// s,t in [0,1]. |u_axis|, |v_axis| carry the world-space extents.
bool intersect_rect(Ray ray, RtShape shape, out RayHit hit)
{
    vec3 u_axis = shape.u_axis.xyz;
    vec3 v_axis = shape.v_axis.xyz;
    vec3 origin = shape.origin.xyz;
    float radius = shape.params.x;

    vec3 normal = cross(u_axis, v_axis);
    float nlen2 = dot(normal, normal);
    if (nlen2 < 1e-12) return false;
    float inv_nlen = inversesqrt(nlen2);
    vec3 n = normal * inv_nlen;

    float denom = dot(ray.dir, n);
    if (abs(denom) < 1e-6) return false;
    float t = dot(origin - ray.origin, n) / denom;
    if (t <= 0.0) return false;

    vec3 p = ray.origin + t * ray.dir;
    vec3 local = p - origin;
    float u_len2 = dot(u_axis, u_axis);
    float v_len2 = dot(v_axis, v_axis);
    float s = dot(local, u_axis) / u_len2;
    float tt = dot(local, v_axis) / v_len2;
    if (s < 0.0 || s > 1.0 || tt < 0.0 || tt > 1.0) return false;

    // Corner radius test in world-space units.
    if (radius > 0.0) {
        float u_len = sqrt(u_len2);
        float v_len = sqrt(v_len2);
        vec2 size_w = vec2(u_len, v_len);
        vec2 p_w = vec2(s * u_len, tt * v_len);
        vec2 half_size = size_w * 0.5;
        vec2 centered = p_w - half_size;
        vec2 d = abs(centered) - half_size + radius;
        float sdf = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
        if (sdf > 0.0) return false;
    }

    hit.t = t;
    hit.uv = vec2(s, tt);
    hit.normal = n;
    return true;
}

// Ray vs. oriented axis-aligned box. Origin is one corner; u/v/w are
// three edges (not necessarily orthonormal — length carries extent).
// Uses a slab test in the box's local frame. Normal is the face normal
// of whichever slab bounded the intersection. UV is the hit point
// projected into the 2D coordinates of the hit face.
bool intersect_cube(Ray ray, RtShape shape, out RayHit hit)
{
    vec3 U = shape.u_axis.xyz;
    vec3 V = shape.v_axis.xyz;
    vec3 W = shape.w_axis.xyz;
    float u_len2 = dot(U, U);
    float v_len2 = dot(V, V);
    float w_len2 = dot(W, W);
    if (u_len2 < 1e-12 || v_len2 < 1e-12 || w_len2 < 1e-12) return false;

    // Transform ray into the box's local frame (corner at 0, axes aligned
    // to U/V/W). Local coords are in world units along each axis.
    vec3 rel = ray.origin - shape.origin.xyz;
    vec3 ro_l = vec3(dot(rel, U) / u_len2,
                     dot(rel, V) / v_len2,
                     dot(rel, W) / w_len2);
    vec3 rd_l = vec3(dot(ray.dir, U) / u_len2,
                     dot(ray.dir, V) / v_len2,
                     dot(ray.dir, W) / w_len2);

    // Slab test against [0, 1]^3 in local frame (since U/V/W are full extents).
    vec3 inv_d = 1.0 / rd_l;
    vec3 t0 = (vec3(0.0) - ro_l) * inv_d;
    vec3 t1 = (vec3(1.0) - ro_l) * inv_d;
    vec3 tmin_v = min(t0, t1);
    vec3 tmax_v = max(t0, t1);
    float tmin = max(max(tmin_v.x, tmin_v.y), tmin_v.z);
    float tmax = min(min(tmax_v.x, tmax_v.y), tmax_v.z);
    if (tmax < max(tmin, 0.0)) return false;

    float t = tmin > 0.0 ? tmin : tmax;
    if (t <= 0.0) return false;

    // Determine which axis's slab bounded tmin (or tmax if we're inside).
    vec3 chosen = tmin > 0.0 ? tmin_v : tmax_v;
    float chosen_t = tmin > 0.0 ? tmin : tmax;
    vec3 n_l;
    vec2 face_uv;
    vec3 p_l = ro_l + chosen_t * rd_l;
    if (chosen.x == chosen_t) {
        n_l = vec3(rd_l.x > 0.0 ? -1.0 : 1.0, 0.0, 0.0);
        face_uv = vec2(p_l.y, p_l.z);
    } else if (chosen.y == chosen_t) {
        n_l = vec3(0.0, rd_l.y > 0.0 ? -1.0 : 1.0, 0.0);
        face_uv = vec2(p_l.x, p_l.z);
    } else {
        n_l = vec3(0.0, 0.0, rd_l.z > 0.0 ? -1.0 : 1.0);
        face_uv = vec2(p_l.x, p_l.y);
    }

    // Convert local-frame normal to world: normalize each axis and apply.
    vec3 n = normalize(n_l.x * U + n_l.y * V + n_l.z * W);

    hit.t = t;
    hit.uv = clamp(face_uv, 0.0, 1.0);
    hit.normal = n;
    return true;
}

// Ray vs. sphere. Sphere centered at the element's AABB centroid
// (origin + (u + v + w) * 0.5), radius = params.x. The sphere is
// inscribed in the bounding box; radius typically set to the minimum
// half-extent by the renderer.
bool intersect_sphere(Ray ray, RtShape shape, out RayHit hit)
{
    vec3 center = shape.origin.xyz
                + 0.5 * (shape.u_axis.xyz + shape.v_axis.xyz + shape.w_axis.xyz);
    float radius = shape.params.x;
    if (radius <= 0.0) return false;

    vec3 oc = ray.origin - center;
    float a = dot(ray.dir, ray.dir);
    float b = dot(oc, ray.dir);
    float c = dot(oc, oc) - radius * radius;
    float disc = b * b - a * c;
    if (disc < 0.0) return false;
    float sq = sqrt(disc);
    float t1 = (-b - sq) / a;
    float t2 = (-b + sq) / a;
    float t = t1 > 0.0 ? t1 : t2;
    if (t <= 0.0) return false;

    vec3 p = ray.origin + t * ray.dir;
    vec3 n = normalize(p - center);

    // Spherical equirect-style UV. Good enough for texturing; for pure
    // reflection-only spheres UV won't be read anyway.
    float u = atan(n.z, n.x) / (2.0 * 3.14159265) + 0.5;
    float v = asin(clamp(n.y, -1.0, 1.0)) / 3.14159265 + 0.5;

    hit.t = t;
    hit.uv = vec2(u, v);
    hit.normal = n;
    return true;
}

// Dispatch on shape_kind. Adding a new primitive means adding a case
// here and (for non-prelude shapes) wiring up IVisual::get_intersect_src.
bool intersect_shape(Ray ray, RtShape shape, out RayHit hit)
{
    if (shape.shape_kind == 1u) return intersect_cube(ray, shape, hit);
    if (shape.shape_kind == 2u) return intersect_sphere(ray, shape, hit);
    return intersect_rect(ray, shape, hit);
}

// ===== RNG: PCG-hash seeded by (pixel, frame). =====
uint g_rng_state;

uint pcg_hash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

void rng_init(uvec2 coord, uint frame) {
    g_rng_state = pcg_hash(coord.x ^ pcg_hash(coord.y ^ pcg_hash(frame + 1u)));
}

uint rng_next_uint() {
    g_rng_state = pcg_hash(g_rng_state);
    return g_rng_state;
}

float rng_next_float() {
    // 24-bit mantissa precision; top 8 bits unused.
    return float(rng_next_uint() >> 8u) * (1.0 / 16777216.0);
}

vec2 rng_next_vec2() { return vec2(rng_next_float(), rng_next_float()); }

// ===== GGX microfacet sampling (isotropic). =====
// Returns a unit half-vector H in world space distributed as GGX(roughness).
vec3 ggx_sample_half(vec3 N, float roughness, vec2 xi) {
    float a = roughness * roughness;
    float phi = 2.0 * 3.14159265 * xi.x;
    float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));

    vec3 H_local = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);

    // Build a tangent basis around N. Uses a standard "pick non-parallel up"
    // trick to avoid degenerate cross products.
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);
    return normalize(T * H_local.x + B * H_local.y + N * H_local.z);
}

// Sample a reflection direction: pick a microfacet H, reflect V around it.
vec3 ggx_sample_reflect(vec3 V, vec3 N, float roughness, vec2 xi) {
    vec3 H = ggx_sample_half(N, roughness, xi);
    return reflect(-V, H);
}

// ===== Closest-hit scene traversal =====
bool trace_closest_hit(Ray ray, out RayHit hit) {
    hit.t = 1e30;
    hit.shape_index = 0xffffffffu;
    uint count = pc.extras.w;
    for (uint i = 0u; i < count; ++i) {
        RtShape s = pc.shapes.data[i];
        RayHit h;
        if (intersect_shape(ray, s, h)) {
            if (h.t < hit.t) {
                hit = h;
                hit.shape_index = i;
            }
        }
    }
    return hit.shape_index != 0xffffffffu;
}

// Forward declaration. The composer emits the actual definition after
// material #includes. Materials are pure (no recursion into trace_ray).
BrdfSample velk_resolve_fill(uint mid, FillContext ctx);

// Forward declaration for the shadow-technique dispatch. Emitted by
// the composer after shadow-technique #includes. Materials that want
// to attenuate their direct-lighting contribution by occlusion call
// this with the light's shadow_tech_id; tech_id 0 returns 1.0 (fully
// lit, no shadow technique attached to that light).
float velk_eval_shadow(uint tech_id, uint light_idx, vec3 world_pos, vec3 world_normal);

// Sample the environment along a direction (or return black if the
// camera has no env). Inlined here rather than going through the env
// material's velk_fill_env, because materials may need to call this
// (e.g. StandardMaterial's diffuse term) and calling velk_resolve_fill
// from inside a fill would re-introduce the recursion GLSL forbids.
// Mirrors EnvMaterial's equirect sampling; stays in sync with its
// GPU data layout (vec4 params: x = intensity, y = rotation_rad).
layout(buffer_reference, std430) readonly buffer _EnvParamsBuf {
    vec4 params;
};

vec3 env_miss_color(vec3 rd) {
    if (pc.env.x == 0u) return vec3(0.0);
    _EnvParamsBuf d = _EnvParamsBuf(pc.env_data_addr);
    const float PI = 3.14159265358979323846;
    float c = cos(d.params.y);
    float s = sin(d.params.y);
    vec3 dir = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
    float u = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;
    float v = asin(clamp(dir.y, -1.0, 1.0)) / PI + 0.5;
    return velk_texture(pc.env.y, vec2(u, v)).rgb * d.params.x;
}
)";

// Compute ray tracer main body.
//
// Primary visibility runs the classic painter's loop (iterate all shapes
// in CPU-sorted depth order, composite each with its alpha over the
// accumulator) so 2D UI with stacked semi-transparent cards matches the
// rasterizer. Each primary hit evaluates its material; if the material
// returns a bounce (terminate == false, i.e. StandardMaterial), we run a
// small iterative secondary loop for that hit's specular path. GLSL has
// no recursion, so bounces can't be a function — they live inline here.
[[maybe_unused]] constexpr string_view rt_compute_main_src = R"(
const int kMaxBounces = 3;

vec3 trace_bounce(Ray ray, vec3 throughput)
{
    vec3 acc = vec3(0.0);
    for (int d = 1; d < kMaxBounces; ++d) {
        RayHit hit;
        if (!trace_closest_hit(ray, hit)) {
            acc += throughput * env_miss_color(ray.dir);
            return acc;
        }
        RtShape s = pc.shapes.data[hit.shape_index];
        FillContext ctx;
        ctx.data_addr = s.material_data_addr;
        ctx.texture_id = s.texture_id;
        ctx.shape_param = s.shape_param;
        ctx.uv = hit.uv;
        ctx.base = s.color;
        ctx.ray_dir = ray.dir;
        ctx.normal = hit.normal;
        ctx.hit_pos = ray.origin + hit.t * ray.dir;
        BrdfSample bs = velk_resolve_fill(s.material_id, ctx);

        float a = clamp(bs.emission.a, 0.0, 1.0);
        acc += throughput * bs.emission.rgb * a;
        if (bs.terminate) {
            if (a >= 0.999) return acc;
            // Transparent: continue straight through, reduced throughput.
            throughput *= (1.0 - a);
            ray.origin = ctx.hit_pos + ray.dir * 1e-3;
            continue;
        }
        throughput *= bs.throughput;
        ray.origin = ctx.hit_pos + hit.normal * 1e-3;
        ray.dir = bs.next_dir;
    }
    // Bounce budget exhausted. The remaining throughput is illuminated by
    // the environment along whatever direction we were about to trace;
    // without this the mirror-of-mirror chain would blackhole.
    acc += throughput * env_miss_color(ray.dir);
    return acc;
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    uint w = pc.extras.y;
    uint h = pc.extras.z;
    uint shape_count = pc.extras.w;
    if (coord.x >= int(w) || coord.y >= int(h)) return;

    rng_init(uvec2(coord), pc.env.z);

    vec2 ndc = (vec2(coord) + 0.5) / vec2(float(w), float(h)) * 2.0 - 1.0;
    vec4 near_h = pc.inv_view_projection * vec4(ndc, 0.0, 1.0);
    vec4 far_h  = pc.inv_view_projection * vec4(ndc, 1.0, 1.0);
    vec3 near_w = near_h.xyz / near_h.w;
    vec3 far_w  = far_h.xyz  / far_h.w;

    Ray primary;
    primary.origin = near_w;
    primary.dir = normalize(far_w - near_w);

    vec3 accum = env_miss_color(primary.dir);

    for (uint i = 0u; i < shape_count; ++i) {
        RtShape s = pc.shapes.data[i];
        RayHit hit;
        if (!intersect_shape(primary, s, hit)) continue;

        FillContext ctx;
        ctx.data_addr = s.material_data_addr;
        ctx.texture_id = s.texture_id;
        ctx.shape_param = s.shape_param;
        ctx.uv = hit.uv;
        ctx.base = s.color;
        ctx.ray_dir = primary.dir;
        ctx.normal = hit.normal;
        ctx.hit_pos = primary.origin + hit.t * primary.dir;
        BrdfSample bs = velk_resolve_fill(s.material_id, ctx);

        vec3 shape_rgb = bs.emission.rgb;
        if (!bs.terminate) {
            // Add the bounced contribution (specular reflection for
            // StandardMaterial). To denoise the stochastic GGX sample we
            // take kSpp samples per primary hit and average. The first
            // sample reuses the direction the material already picked;
            // the rest re-evaluate the material to draw fresh half-
            // vectors from the RNG. Cost scales linearly with kSpp.
            const int kSpp = 4;
            vec3 bounce = vec3(0.0);
            Ray refl;
            refl.origin = ctx.hit_pos + hit.normal * 1e-3;
            refl.dir = bs.next_dir;
            bounce += trace_bounce(refl, bs.throughput);
            for (int sp = 1; sp < kSpp; ++sp) {
                BrdfSample bs2 = velk_resolve_fill(s.material_id, ctx);
                refl.origin = ctx.hit_pos + hit.normal * 1e-3;
                refl.dir = bs2.next_dir;
                bounce += trace_bounce(refl, bs2.throughput);
            }
            shape_rgb += bounce * (1.0 / float(kSpp));
        }
        float a = clamp(bs.emission.a, 0.0, 1.0);
        accum = shape_rgb * a + accum * (1.0 - a);
    }

    imageStore(gStorageImages[nonuniformEXT(pc.extras.x)], coord, vec4(accum, 1.0));
}
)";

} // namespace velk

#endif // VELK_RENDER_DEFAULT_SHADERS_H
