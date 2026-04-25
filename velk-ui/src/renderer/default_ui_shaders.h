#ifndef VELK_RENDER_DEFAULT_SHADERS_H
#define VELK_RENDER_DEFAULT_SHADERS_H

#include <velk/string.h>
#include <velk/string_view.h>

#include <velk-render/ext/element_vertex.h>
#include <velk-ui/ext/material_shaders.h>

#include <cstdio>
#include <cstring>

namespace velk {

// The default vertex/fragment shader pair used when a visual or material
// does not supply its own. Both the forward and gbuffer default vertex
// shaders are now the single shared element vertex shader in
// velk-render/ext/element_vertex.h; no per-path variants remain.

[[maybe_unused]] constexpr string_view default_vertex_src =
    ::velk::ext::element_vertex_src;

[[maybe_unused]] constexpr string_view default_fragment_src = R"(
#version 450

// The shared element vertex shader writes the full canonical varying
// set (locations 0..6). Declare every input even when unused so the
// SPIR-V interface matches and the validator doesn't warn about
// dropped outputs.
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local_uv;
layout(location = 2) flat in vec2 v_size;
layout(location = 3) in vec3 v_world_pos;
layout(location = 4) in vec3 v_world_normal;
layout(location = 5) flat in uint v_shape_param;
layout(location = 6) in vec2 v_uv1;

layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = v_color;
}
)";

[[maybe_unused]] constexpr string_view default_gbuffer_vertex_src =
    ::velk::ext::element_vertex_src;

// Default fragment shader for the deferred G-buffer fill. Writes the
// instance color straight through to albedo, marks the fragment as
// "unlit" so the compute lighting pass passes albedo through unchanged.
// Materials that want lighting (StandardMaterial) override this.
[[maybe_unused]] constexpr string_view default_gbuffer_fragment_src = R"(
#version 450

// Canonical deferred varyings; the shared element vertex shader emits
// the full set (locations 0..6). Declare every input even when unused
// so the SPIR-V interface matches and the validator doesn't warn
// about dropped outputs.
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local_uv;
layout(location = 2) flat in vec2 v_size;
layout(location = 3) in vec3 v_world_pos;
layout(location = 4) in vec3 v_world_normal;
layout(location = 5) flat in uint v_shape_param;
layout(location = 6) in vec2 v_uv1;

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

// ===== Material eval-driver fragment templates =====
// Forward and deferred fragment shaders that materials with a
// `velk_eval_<name>` body share. The raster-pipeline composer
// concatenates these with the material's eval_src, vertex_src, and the
// visual's (optional) discard snippet, then compiles.
//
// Each template assumes:
//   - `velk.glsl` and `velk-ui.glsl` are #included by the composer
//     (which gives EvalContext / MaterialEval / VELK_LIGHTING_*).
//   - A literal  <%EVAL_FN%>  is replaced with the material's eval
//     function name (e.g. "velk_eval_gradient").
//   - A literal  <%DISCARD%>  is replaced with a float literal for the
//     alpha-discard threshold.
//   - The composer emits the eval_src (declares the material's own
//     buffer_reference types + velk_eval_<name>) BEFORE this template.
//   - The DrawData buffer_reference here uses OpaquePtr for the
//     instance and material slots — the fragment doesn't dereference
//     the typed layouts, it just forwards the material address via
//     ctx.data_addr and lets the eval's own typed pointer pick it up.

[[maybe_unused]] constexpr string_view forward_fragment_driver_template = R"(
layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(OpaquePtr, OpaquePtr)
    OpaquePtr material;
};
layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local_uv;
layout(location = 2) flat in vec2 v_size;
layout(location = 3) in vec3 v_world_pos;
layout(location = 4) in vec3 v_world_normal;
layout(location = 5) flat in uint v_shape_param;
layout(location = 6) in vec2 v_uv1;

layout(location = 0) out vec4 frag_color;

void main()
{
    EvalContext ctx;
    ctx.globals     = root.global_data;
    ctx.data_addr   = uint64_t(root.material);
    ctx.texture_id  = root.texture_id;  // per-drawcall, shared by all instances
    ctx.shape_param = v_shape_param;
    ctx.uv          = v_local_uv;
    ctx.uv1         = v_uv1;
    ctx.base        = v_color;
    ctx.ray_dir     = normalize(v_world_pos - root.global_data.cam_pos.xyz);
    ctx.normal      = v_world_normal;
    ctx.hit_pos     = v_world_pos;

    MaterialEval e = <%EVAL_FN%>(ctx);
    if (e.color.a < <%DISCARD%>) discard;
    frag_color = e.color;
}
)";

[[maybe_unused]] constexpr string_view deferred_fragment_driver_template = R"(
layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(OpaquePtr, OpaquePtr)
    OpaquePtr material;
};
layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local_uv;
layout(location = 2) flat in vec2 v_size;
layout(location = 3) in vec3 v_world_pos;
layout(location = 4) in vec3 v_world_normal;
layout(location = 5) flat in uint v_shape_param;
layout(location = 6) in vec2 v_uv1;

layout(location = 0) out vec4 g_albedo;
layout(location = 1) out vec4 g_normal;
layout(location = 2) out vec4 g_world_pos;
layout(location = 3) out vec4 g_material;

// Composer appends either the visual's discard snippet or an empty stub.
void velk_visual_discard();

void main()
{
    velk_visual_discard();

    EvalContext ctx;
    ctx.globals     = root.global_data;
    ctx.data_addr   = uint64_t(root.material);
    ctx.texture_id  = root.texture_id;
    ctx.shape_param = v_shape_param;
    ctx.uv          = v_local_uv;
    ctx.uv1         = v_uv1;
    ctx.base        = v_color;
    ctx.ray_dir     = normalize(v_world_pos - root.global_data.cam_pos.xyz);
    ctx.normal      = v_world_normal;
    ctx.hit_pos     = v_world_pos;

    MaterialEval e = <%EVAL_FN%>(ctx);
    if (e.color.a < <%DISCARD%>) discard;

    vec3 N = normalize(length(e.normal) > 0.0 ? e.normal : v_world_normal);
    g_albedo    = e.color;
    g_normal    = vec4(N, 0.0);
    g_world_pos = vec4(v_world_pos, 0.0);
    g_material  = vec4(e.metallic, e.roughness, float(e.lighting_mode) / 255.0, 0.0);
}
)";

/**
 * @brief Composes a full fragment shader from a driver template and a
 *        material's eval source.
 *
 * Substitutes `<%EVAL_FN%>` with @p eval_fn and `<%DISCARD%>` with a
 * float literal of @p discard_threshold, then prepends a preamble
 * (`#version`, shared includes) and the material's @p eval_src. The
 * result is the complete fragment source ready for `compile_pipeline`.
 */
inline string compose_eval_fragment(string_view driver_template,
                                    string_view eval_src,
                                    string_view eval_fn,
                                    float discard_threshold)
{
    string out;
    out.append(string_view("#version 450\n"
                           "#define VELK_RASTER 1\n"
                           "#include \"velk.glsl\"\n"
                           "#include \"velk-ui.glsl\"\n"));
    out.append(eval_src);
    out.append(string_view("\n"));

    char thr_buf[32];
    int tn = std::snprintf(thr_buf, sizeof(thr_buf), "%f", discard_threshold);
    string_view thr(thr_buf, tn > 0 ? static_cast<size_t>(tn) : 0);

    // Tiny placeholder replacement: scan for <%EVAL_FN%> and <%DISCARD%>.
    size_t i = 0;
    while (i < driver_template.size()) {
        if (i + 10 <= driver_template.size()
            && std::memcmp(driver_template.data() + i, "<%EVAL_FN%>", 11) == 0) {
            out.append(eval_fn);
            i += 11;
        } else if (i + 11 <= driver_template.size()
                   && std::memcmp(driver_template.data() + i, "<%DISCARD%>", 11) == 0) {
            out.append(thr);
            i += 11;
        } else {
            out.append(string_view(driver_template.data() + i, 1));
            ++i;
        }
    }
    return out;
}

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

// GlobalData / RtShape / RtShapeList / BvhNode / BvhNodeList come from velk.glsl.
// GlobalData carries inverse_view_projection + scene BVH (nodes + shapes).

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
    uint _pad0;                // 52
    LightList lights;          // 56
    _EnvParamsBuf env_params;  // 64
    GlobalData globals;        // 72
} pc;

// ===== Shadow ray support (duplicated from rt_compute_prelude_src) =====
// Extract to a shared include when a second shadow technique arrives
// and deferred needs composed dispatch.
struct Ray { vec3 origin; vec3 dir; };
struct RayHit { float t; vec2 uv; vec3 normal; uint shape_index; };

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

bool intersect_cube(Ray ray, RtShape shape, out RayHit hit)
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
    hit.t = t;
    hit.uv = vec2(0.0);
    hit.normal = normalize((ray.origin + t * ray.dir) - center);
    return true;
}

// Forward declaration. The DeferredLighter composer appends the
// dispatch body (plus any visual-contributed intersect snippets) when
// compiling the compute pipeline variant for the current scene's
// intersect set. Built-in kinds forward to the *_d functions above;
// visual-registered kinds call their registered snippets.
bool intersect_shape(Ray ray, RtShape shape, out RayHit hit);

// Ray-vs-AABB slab test. Returns true if ray intersects the box within
// [0, t_max] and writes t_near (clamped to >= 0) to t_hit.
bool ray_aabb(Ray ray, vec3 bmin, vec3 bmax, float t_max, out float t_hit)
{
    vec3 inv_d = 1.0 / ray.dir;
    vec3 t0 = (bmin - ray.origin) * inv_d;
    vec3 t1 = (bmax - ray.origin) * inv_d;
    vec3 tmn = min(t0, t1);
    vec3 tmx = max(t0, t1);
    float tnear = max(max(tmn.x, tmn.y), tmn.z);
    float tfar  = min(min(tmx.x, tmx.y), tmx.z);
    if (tfar < max(tnear, 0.0) || tnear > t_max) return false;
    t_hit = max(tnear, 0.0);
    return true;
}

// Any-hit BVH traversal for shadow rays: early-exit on the first
// confirmed blocker within t_max. Stack-depth 32 comfortably covers
// any realistic UI scene depth.
bool trace_any_hit_bvh(Ray ray, float t_max)
{
    if (pc.globals.bvh_node_count == 0u) return false;
    uint stack[32];
    int sp = 0;
    stack[sp++] = pc.globals.bvh_root;
    while (sp > 0) {
        uint ni = stack[--sp];
        BvhNode node = pc.globals.bvh_nodes.data[ni];
        float t_hit;
        if (!ray_aabb(ray, node.aabb_min.xyz, node.aabb_max.xyz, t_max, t_hit)) continue;

        for (uint i = 0u; i < node.shape_count; ++i) {
            RtShape s = pc.globals.bvh_shapes.data[node.first_shape + i];
            RayHit h;
            if (intersect_shape(ray, s, h) && h.t > 0.0 && h.t < t_max) return true;
        }

        for (uint i = 0u; i < node.child_count; ++i) {
            if (sp < 32) stack[sp++] = node.first_child + i;
        }
    }
    return false;
}

// RT shadow: one occlusion ray against the scene BVH. Mirrors
// rt_shadow.cpp's velk_shadow_rt but walks the acceleration structure.
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
    // Bias the ray origin toward the light to avoid self-intersection.
    // Earlier we biased along the normal, which fails for double-sided
    // thin geometry (awnings, banners) where the visible face's normal
    // can point away from the light: the bias would push the origin
    // into the back face and immediately self-shadow. L-biasing always
    // moves into the lit half-space regardless of which way the normal
    // happens to face. Magnitude is small to work at meter scale; for
    // pixel-scale scenes a per-camera or per-light bias would be ideal.
    Ray r;
    r.origin = world_pos + L * 0.005;
    r.dir    = L;
    // Pull t_max slightly short of the light position so the ray
    // doesn't hit the light's own fixture mesh on the way in.
    t_max = max(t_max - 0.005, 0.0);
    return trace_any_hit_bvh(r, t_max) ? 0.0 : 1.0;
}

// Shadow dispatch. Currently only tech_id = 1 (rt_shadow) is wired; any
// other tech falls through to fully-lit. Extend via composition when
// more techniques land.
float velk_eval_shadow(uint tech_id, uint light_idx, vec3 world_pos, vec3 world_normal)
{
    if (tech_id == 1u) return velk_shadow_rt(light_idx, world_pos, world_normal);
    return 1.0;
}

// Equirect env sample with explicit mip LOD. The env texture ships
// with a bilinear-downsampled mip chain as a rough roughness
// prefilter; higher `lod` values read blurrier mips. Returns vec3(0)
// when the view has no environment.
vec3 env_miss_color_lod(vec3 rd, float lod)
{
    if (pc.env_texture_id == 0u) return vec3(0.0);
    const float PI = 3.14159265358979323846;
    vec4 params = pc.env_params.params;
    float c = cos(params.y);
    float s = sin(params.y);
    vec3 dir = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
    float u = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;
    float v = asin(clamp(dir.y, -1.0, 1.0)) / PI + 0.5;
    return textureLod(velk_textures[nonuniformEXT(pc.env_texture_id)],
                      vec2(u, v), lod).rgb * params.x;
}

vec3 env_miss_color(vec3 rd) { return env_miss_color_lod(rd, 0.0); }

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
        mat4 inv_vp = pc.globals.inverse_view_projection;
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

        // Env lighting: single-sample approximation. Diffuse reads
        // along N with a deep LOD so it reads roughly the irradiance
        // average; specular reads the mirror reflection with LOD scaled
        // by roughness as a cheap GGX-prefilter stand-in (bilinear mips
        // rather than true GGX-convolved, but it reads right).
        float env_max_lod = float(textureQueryLevels(
            velk_textures[nonuniformEXT(pc.env_texture_id)])) - 1.0;
        float spec_lod = roughness * env_max_lod;
        float diffuse_lod = env_max_lod;
        vec3 env_diffuse  = env_miss_color_lod(N, diffuse_lod);
        vec3 env_specular = env_miss_color_lod(reflect(-V, N), spec_lod);
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
#define VELK_COMPUTE 1
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#include "velk.glsl"
#include "velk-ui.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 1, rgba8) uniform writeonly image2D gStorageImages[];

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
    RtShapeList shapes;         // primary-ray buffer, painter-sorted back-to-front
    RtShapeList bvh_shapes;     // element-grouped buffer; BvhNode ranges index here
    BvhNodeList bvh_nodes;
    uint bvh_root;
    uint bvh_node_count;
    uint64_t env_data_addr;
    LightList lights;
    uint light_count;
    uint _lights_pad;
    GlobalData globals;         // this view's FrameGlobals; routed into ctx.globals for eval bodies.
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

// EvalContext + MaterialEval shared between raster and RT come from
// velk-ui.glsl (included above).

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
//   sample_count_hint  material's preferred number of bounce samples at
//               this hit (e.g. 1 for a mirror, higher for rough GGX).
//               Tracer caps against a global per-bounce budget.
struct BrdfSample {
    vec4 emission;
    vec3 throughput;
    vec3 next_dir;
    bool terminate;
    uint sample_count_hint;
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

// Forward declaration: the RT composer generates the dispatch body
// after material / shadow-tech / intersect snippets have been
// included. Built-in cases (rect/cube/sphere) forward to the
// functions above; visual-registered intersects get `case <id>:`
// entries in the generated switch.
bool intersect_shape(Ray ray, RtShape shape, out RayHit hit);

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

// Ray-vs-AABB slab test. Shared by the BVH walkers below.
bool ray_aabb(Ray ray, vec3 bmin, vec3 bmax, float t_max, out float t_hit)
{
    vec3 inv_d = 1.0 / ray.dir;
    vec3 t0 = (bmin - ray.origin) * inv_d;
    vec3 t1 = (bmax - ray.origin) * inv_d;
    vec3 tmn = min(t0, t1);
    vec3 tmx = max(t0, t1);
    float tnear = max(max(tmn.x, tmn.y), tmn.z);
    float tfar  = min(min(tmx.x, tmx.y), tmx.z);
    if (tfar < max(tnear, 0.0) || tnear > t_max) return false;
    t_hit = max(tnear, 0.0);
    return true;
}

// ===== Closest-hit BVH traversal. Used by bounces + shadows. The
// resulting `hit.shape_index` indexes into `pc.bvh_shapes` (not
// `pc.shapes`, which only primary rays consume). Stack depth bounds
// the max fan-out per node across the UI tree walk; wide scene roots
// (a grid of 40+ tiles, a dashboard of cards) need more than 32.
const int kBvhStackSize = 128;
bool trace_closest_hit(Ray ray, out RayHit hit) {
    hit.t = 1e30;
    hit.shape_index = 0xffffffffu;
    if (pc.bvh_node_count == 0u) return false;
    uint stack[kBvhStackSize];
    int sp = 0;
    stack[sp++] = pc.bvh_root;
    while (sp > 0) {
        uint ni = stack[--sp];
        BvhNode node = pc.bvh_nodes.data[ni];
        float t_enter;
        if (!ray_aabb(ray, node.aabb_min.xyz, node.aabb_max.xyz, hit.t, t_enter)) continue;
        for (uint i = 0u; i < node.shape_count; ++i) {
            uint idx = node.first_shape + i;
            RtShape s = pc.bvh_shapes.data[idx];
            RayHit h;
            if (intersect_shape(ray, s, h) && h.t > 0.0 && h.t < hit.t) {
                hit = h;
                hit.shape_index = idx;
            }
        }
        for (uint i = 0u; i < node.child_count; ++i) {
            if (sp < kBvhStackSize) stack[sp++] = node.first_child + i;
        }
    }
    return hit.shape_index != 0xffffffffu;
}

// Any-hit BVH traversal for shadow rays: first confirmed blocker wins.
bool trace_any_hit(Ray ray, float t_max) {
    if (pc.bvh_node_count == 0u) return false;
    uint stack[kBvhStackSize];
    int sp = 0;
    stack[sp++] = pc.bvh_root;
    while (sp > 0) {
        uint ni = stack[--sp];
        BvhNode node = pc.bvh_nodes.data[ni];
        float t_enter;
        if (!ray_aabb(ray, node.aabb_min.xyz, node.aabb_max.xyz, t_max, t_enter)) continue;
        for (uint i = 0u; i < node.shape_count; ++i) {
            RtShape s = pc.bvh_shapes.data[node.first_shape + i];
            RayHit h;
            if (intersect_shape(ray, s, h) && h.t > 0.0 && h.t < t_max) return true;
        }
        for (uint i = 0u; i < node.child_count; ++i) {
            if (sp < kBvhStackSize) stack[sp++] = node.first_child + i;
        }
    }
    return false;
}

// Forward declaration. The composer emits the actual definition after
// material #includes. Materials are pure (no recursion into trace_ray).
BrdfSample velk_resolve_fill(uint mid, EvalContext ctx);

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

// Shared PBR shading helper. Converts a MaterialEval produced by a
// material's velk_eval_<name> into a BrdfSample. Every Lit material
// routes through this instead of open-coding its own PBR body.
//
// Placement: composer emits this string between `velk_eval_shadow` and
// `velk_resolve_fill` so it can call the former and be called by the
// latter. Depends on pc.lights, pc.light_count, env_miss_color,
// velk_eval_shadow, ggx_sample_half, rng_next_vec2 — all in scope at
// that point.
[[maybe_unused]] constexpr string_view rt_pbr_shade_src = R"(
BrdfSample velk_pbr_shade(MaterialEval eval, EvalContext ctx)
{
    vec3 N = normalize(eval.normal);
    vec3 V = normalize(-ctx.ray_dir);
    float metallic  = clamp(eval.metallic, 0.0, 1.0);
    float roughness = clamp(eval.roughness, 0.04, 1.0);
    vec3 base = eval.color.rgb;
    float ao = clamp(eval.occlusion, 0.0, 1.0);

    // KHR_materials_specular: dielectric F0 tinted by specular_color_factor
    // and weighted by specular_factor. Metals continue to use base as F0.
    vec3 dielectric_F0 = 0.04 * eval.specular_color_factor * eval.specular_factor;
    vec3 F0 = mix(dielectric_F0, base, metallic);
    float VdotN = max(dot(V, N), 0.0);
    vec3 F = F0 + (vec3(1.0) - F0) * pow(1.0 - VdotN, 5.0);
    F *= eval.specular_factor;

    // Direct lighting from scene lights. Each light contributes a
    // Lambertian diffuse term scaled by its distance / spot attenuation
    // and modulated by its shadow technique's visibility.
    vec3 direct = vec3(0.0);
    for (uint li = 0u; li < pc.light_count; ++li) {
        Light light = pc.lights.data[li];
        vec3 L;
        float atten = 1.0;
        if (light.flags.x == 0u) {
            L = -light.direction.xyz;
        } else {
            vec3 to_light = light.position.xyz - ctx.hit_pos;
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
        float shadow = velk_eval_shadow(light.flags.y, li, ctx.hit_pos, N);
        vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * atten * shadow;
        direct += base * (1.0 - metallic) * NdotL * radiance;
    }

    // Diffuse term: crude "irradiance at the normal" via a single env
    // sample, modulated by AO. Upgrade to preconvolved irradiance or
    // stochastic cosine sampling when visibly needed.
    vec3 env_at_normal = env_miss_color(N);
    vec3 diffuse = base * (1.0 - metallic) * env_at_normal * ao;
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

    // Specular: sample a GGX half-vector, reflect V around it. Main's
    // iterative loop evaluates the reflected ray and multiplies by F.
    vec3 H = ggx_sample_half(N, roughness, rng_next_vec2());
    vec3 Lr = reflect(-V, H);

    BrdfSample bs;
    bs.emission = vec4(kD * diffuse + direct + eval.emissive, eval.color.a);
    bs.throughput = F;
    bs.next_dir = Lr;
    bs.terminate = false;
    // Sample count scales with GGX lobe width: 1 at mirror, up to 16 at
    // fully rough. Tracer clamps against its own per-bounce cap.
    bs.sample_count_hint = uint(1.0 + roughness * roughness * 15.0);
    return bs;
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
const int kMaxBounces = 4;

vec3 trace_bounce(Ray ray, vec3 throughput)
{
    vec3 acc = vec3(0.0);
    for (int d = 1; d < kMaxBounces; ++d) {
        RayHit hit;
        if (!trace_closest_hit(ray, hit)) {
            acc += throughput * env_miss_color(ray.dir);
            return acc;
        }
        // trace_closest_hit returns BVH-space indices (pc.bvh_shapes).
        RtShape s = pc.bvh_shapes.data[hit.shape_index];
        EvalContext ctx;
        ctx.globals = pc.globals;
        ctx.data_addr = s.material_data_addr;
        ctx.texture_id = s.texture_id;
        ctx.shape_param = s.shape_param;
        ctx.uv = hit.uv;
        // Analytic RT shapes have one UV set by construction; triangle-mesh RT
        // (and its second UV stream) lands with the broader BLAS refactor.
        ctx.uv1 = hit.uv;
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

    // Per-pixel primary sample count. Each sample fires one jittered
    // primary ray through the painter-sorted loop; the results are
    // averaged. Reduces edge-aliasing on shape boundaries and damps
    // bounce noise from rough specular hits. Cost scales linearly
    // with kPrimarySamples; 1 = no AA (raw stochastic output),
    // 4 = typical quality / cost balance.
    const uint kPrimarySamples = 4u;

    vec3 final = vec3(0.0);
    for (uint psample = 0u; psample < kPrimarySamples; ++psample) {
        // Sub-pixel jitter. Uniform 0..1 offset into the pixel; with
        // multiple samples the average lands near the pixel center
        // while still anti-aliasing edges. RNG is seeded per coord
        // + frame so jitter patterns are stable within a frame and
        // different between frames.
        vec2 jitter = vec2(rng_next_float(), rng_next_float());
        vec2 ndc = (vec2(coord) + jitter) / vec2(float(w), float(h)) * 2.0 - 1.0;
        vec4 near_h = pc.inv_view_projection * vec4(ndc, 0.0, 1.0);
        vec4 far_h  = pc.inv_view_projection * vec4(ndc, 1.0, 1.0);
        vec3 near_w = near_h.xyz / near_h.w;
        vec3 far_w  = far_h.xyz  / far_h.w;

        Ray primary;
        primary.origin = near_w;
        primary.dir = normalize(far_w - near_w);

        // Painter-sorted back-to-front iteration over the primary
        // buffer. Co-planar UI shapes (cards, text, overlays stacked
        // at z=0) need this to composite in authored layer order;
        // closest-hit BVH would advance past a whole plane after the
        // first hit and drop every other shape sharing that t. BVH
        // is used for bounces + shadows where closest-hit is the
        // right semantics.
        //
        // Future: a proper front-to-back BVH walk for primary is
        // possible once each BVH node can be flagged "coplanar group"
        // at build time (all its shapes share a plane within
        // tolerance) - the walker would iterate the group's shapes
        // painter-style instead of doing closest-hit inside it, and
        // fall back to closest-hit between groups. Parked while
        // primary cost is still O(shapes) * cheap-intersect; revisit
        // if profiling shows the primary pass is hot.
        vec3 accum = env_miss_color(primary.dir);

        for (uint i = 0u; i < shape_count; ++i) {
            RtShape s = pc.shapes.data[i];
            RayHit hit;
            if (!intersect_shape(primary, s, hit)) continue;

            EvalContext ctx;
            ctx.globals = pc.globals;
            ctx.data_addr = s.material_data_addr;
            ctx.texture_id = s.texture_id;
            ctx.shape_param = s.shape_param;
            ctx.uv = hit.uv;
            // Analytic RT shapes have one UV set by construction; triangle-mesh RT
            // (and its second UV stream) lands with the broader BLAS refactor.
            ctx.uv1 = hit.uv;
            ctx.base = s.color;
            ctx.ray_dir = primary.dir;
            ctx.normal = hit.normal;
            ctx.hit_pos = primary.origin + hit.t * primary.dir;
            BrdfSample bs = velk_resolve_fill(s.material_id, ctx);

            vec3 shape_rgb = bs.emission.rgb;
            if (!bs.terminate) {
                // Per-hit sample count: material's preferred count
                // (scales with roughness / lobe width), clamped by
                // the tracer's global cap. Mirror surfaces collapse
                // to 1; rough surfaces spend the budget to flatten
                // GGX noise.
                const uint kSppCap = 12u;
                uint spp = clamp(bs.sample_count_hint, 1u, kSppCap);
                vec3 bounce = vec3(0.0);
                Ray refl;
                refl.origin = ctx.hit_pos + hit.normal * 1e-3;
                refl.dir = bs.next_dir;
                bounce += trace_bounce(refl, bs.throughput);
                for (uint sp = 1u; sp < spp; ++sp) {
                    BrdfSample bs2 = velk_resolve_fill(s.material_id, ctx);
                    refl.origin = ctx.hit_pos + hit.normal * 1e-3;
                    refl.dir = bs2.next_dir;
                    bounce += trace_bounce(refl, bs2.throughput);
                }
                shape_rgb += bounce * (1.0 / float(spp));
            }
            float a = clamp(bs.emission.a, 0.0, 1.0);
            accum = shape_rgb * a + accum * (1.0 - a);
        }

        final += accum;
    }

    imageStore(gStorageImages[nonuniformEXT(pc.extras.x)], coord,
               vec4(final * (1.0 / float(kPrimarySamples)), 1.0));
}
)";

} // namespace velk

#endif // VELK_RENDER_DEFAULT_SHADERS_H
