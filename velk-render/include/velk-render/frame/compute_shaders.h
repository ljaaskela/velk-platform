#ifndef VELK_RENDER_DEFAULT_SHADERS_H
#define VELK_RENDER_DEFAULT_SHADERS_H

#include <velk/string.h>
#include <velk/string_view.h>

#include <velk-render/interface/intf_frame_snippet_registry.h>
#include <velk-render/frame/raster_shaders.h>

#include <cstdio>
#include <cstring>

namespace velk {

// Raster default shaders + driver templates + compose_eval_fragment
// live in <velk-render/frame/raster_shaders.h>. This file holds the
// scene-side shader bits: the velk-ui GLSL include, the deferred-lighting
// compute shader, and the RT compute prelude/main.

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
[[maybe_unused]] constexpr string_view deferred_compute_prelude_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 1, rgba8)   uniform writeonly image2D gStorageImages[];
layout(set = 0, binding = 2, rgba32f) uniform writeonly image2D gStorageImagesF32[];
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

// Scene TLAS, bound by index (set = 1) instead of chased through a
// buffer_device_address in globals. The renderer stamps the same
// scene-wide BVH buffers into set_global_buffer each frame; indexed by
// first_child / first_shape are relative to this frame's ring region;
// VELK_NODE_BASE / VELK_SHAPE_BASE add the region base (see IGpuArena).
layout(set = 1, binding = 0, std430) readonly buffer VelkBvhNodes { BvhNode data[]; } velk_bvh_nodes;
layout(set = 1, binding = 1, std430) readonly buffer VelkBvhShapes { RtShape data[]; } velk_bvh_shapes;
// velk_globals (set = 1 slot 2) is declared in velk.glsl. pc.globals_base
// selects this view's record.
#define VELK_GLOBALS velk_globals.data[pc.globals_base]
#define VELK_NODE_BASE VELK_GLOBALS.bvh_node_base
#define VELK_SHAPE_BASE VELK_GLOBALS.bvh_shape_base

// Mirrors C++ GpuLight (80 bytes) in ray_tracer.cpp / deferred_lighter.cpp.
struct Light {
    uvec4 flags;           // x = type (0 dir, 1 point, 2 spot), y = shadow_tech_id, zw = _
    vec4  position;        // xyz world position (point / spot)
    vec4  direction;       // xyz world forward (dir / spot)
    vec4  color_intensity; // rgb colour, a intensity
    vec4  params;          // x range, y cos(inner), z cos(outer), w light size (dir: angular radius rad; point/spot: world radius)
};

// Scene lights bound by index (set = 1 slot 5); this view's run starts at
// pc.lights_base. Same buffer the RT compute reads.
layout(set = 1, binding = 5, std430) readonly buffer VelkLights { Light data[]; } velk_lights;

// Material records as raw words (set = 1 slot 4). This shader does not
// evaluate materials, but it composes the same intersect snippets the RT
// compute does, and an intersect may read its shape's record (text reads its
// glyph-data bases from it) at the word base carried on the shape.
layout(set = 1, binding = 4, std430) readonly buffer VelkMaterialWords { uint data[]; } velk_material_words;

// Mesh-shape transforms (set = 1 slot 10); read via velk_mesh_instance(shape),
// which resolves shape.mesh_instance_base. Same buffer the RT compute reads.
layout(set = 1, binding = 10, std430) readonly buffer VelkMeshInstances { MeshInstanceData data[]; } velk_mesh_instances;

// Per-primitive RT geometry metadata (set = 1 slot 11), read via
// velk_mesh_static(inst), plus the BLAS runs it points at (slots 12 / 13).
// A primitive's runs are allocated once at load and never move.
layout(set = 1, binding = 11, std430) readonly buffer VelkMeshStatic { MeshStaticData data[]; } velk_mesh_static_records;
layout(set = 1, binding = 12, std430) readonly buffer VelkBlasNodes { BvhNode data[]; } velk_blas_nodes;
layout(set = 1, binding = 13, std430) readonly buffer VelkBlasTris  { uint    data[]; } velk_blas_tris;

// RtShape / BvhNode come from velk.glsl.
// View-level globals (inverse_view_projection, BVH, present_counter)
// are dereferenced via `globals.X`; the address is in push-constant
// slot [0..8) (see velk.glsl GlobalData).
)" R"(

// scalar layout: tight packing so the per-dispatch CPU struct (whose
// `vec4 cam_pos` sits at offset 0 of the struct) lines up with the
// GPU's view of cam_pos at offset 8 of the push-constant block. Default
// std430 would round vec4 up to a 16-byte alignment and shift every
// subsequent field by 8 bytes vs. what the CPU writes.
layout(push_constant, scalar) uniform PC {
    uint globals_base;         // offset 0  (4 bytes; view's FrameGlobals index)
    uint _pad_globals;         // 4         (keeps cam_pos at offset 8)
    vec4 cam_pos;              // 8         (CPU push starts here)
    uint output_image_id;      // 24
    uint albedo_tex_id;        // 28
    uint normal_tex_id;        // 32
    uint worldpos_tex_id;      // 36
    uint material_tex_id;      // 40
    uint emissive_tex_id;      // 44
    uint width;                // 48
    uint height;               // 52
    uint light_count;          // 56
    uint env_texture_id;       // 60
    uint shadow_debug_image_id;// 64  RGBA32F storage image; 0 = disabled
    uint ltc_matrix_id;        // 68  LTC transform table; 0 = area specular off
    uint lights_base;          // 72  index into velk_lights (set = 1 slot 5)
    uint ltc_magnitude_id;     // 76  LTC energy / Fresnel table
    vec2 env_params;           // 80  x = intensity, y = rotation_rad (inline)
    uint irr_image_id;         // 88  demodulated diffuse irradiance out (denoised downstream)
    uint _pad1;                // 92  pads block to 96 (CPU struct is alignas(16))
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

// Forward decl: ray_aabb is defined further down with the BVH walker.
bool ray_aabb(Ray ray, vec3 bmin, vec3 bmax, float t_max, out float t_hit);

// Triangle-mesh intersector (shape_kind == 255). Walks the per-mesh
// BLAS that lives in the trailing region of the primitive's
// MeshStaticData buffer. Ray is transformed into mesh-local space so
// vertex data stays untouched and instances share buffers.
bool intersect_mesh(Ray ray, RtShape shape, out RayHit hit)
{
    // World-space AABB quick reject.
    {
        float t_aabb;
        if (!ray_aabb(ray, shape.origin.xyz, shape.u_axis.xyz, 1e30, t_aabb)) return false;
    }
    MeshInstanceData inst = velk_mesh_instance(shape);
    if (inst.mesh_static_base == VELK_INVALID_MESH_STATIC) return false;
    MeshStaticData st = velk_mesh_static(inst);
    if (st.triangle_count == 0u || st.vertex_stride == 0u) return false;
    if (st.blas_node_count == 0u) return false;

    // Ray into mesh-local space. Normalize the local direction so MT's
    // numerical thresholds (`abs(det) < 1e-7`, `tt < 1e-4`) stay
    // calibrated against mesh-edge magnitudes regardless of world
    // scale. Without this, instances scaled up by a large world
    // matrix produce a tiny local `ld`, which makes every triangle's
    // `det` collapse below the rejection threshold and silently drops
    // the entire instance from shadow casting (the bistro mm-scale
    // chairs were the original symptom). Track the original local-dir
    // length so the returned `hit.t` can be converted back to
    // world-space distance for the caller's `t_max` comparison.
    vec3 lo = (inst.inv_world * vec4(ray.origin, 1.0)).xyz;
    vec3 ld_unnorm = (inst.inv_world * vec4(ray.dir, 0.0)).xyz;
    float ld_scale = length(ld_unnorm);
    if (ld_scale < 1e-30) return false;
    vec3 ld = ld_unnorm / ld_scale;

    uint floats_per_vert = st.vertex_stride >> 2u;  // stride is bytes; floats = bytes/4

    // This primitive's BLAS runs inside the shared node / triangle arenas;
    // node and triangle indices below are relative to these bases.
    uint blas_node_base = st.blas_node_base;
    uint blas_tri_base  = st.blas_tri_base;

    Ray local_ray;
    local_ray.origin = lo;
    local_ray.dir    = ld;

    bool  found = false;
    float best_t = 1e30;
    float best_u = 0.0;
    float best_v = 0.0;
    uint  best_o0 = 0u;
    uint  best_o1 = 0u;
    uint  best_o2 = 0u;

    // BLAS walk. Stack depth 32 fits any reasonable tree (a perfectly
    // balanced binary BVH at depth 32 holds 2^32 leaves).
    uint stack[32];
    int sp = 0;
    if (st.blas_node_count == 0u) return false;
    stack[sp++] = st.blas_root;
    while (sp > 0) {
        uint ni = uint(stack[--sp]);
        BvhNode node = velk_blas_nodes.data[blas_node_base + ni];
        float t_aabb;
        if (!ray_aabb(local_ray, node.aabb_min.xyz, node.aabb_max.xyz, best_t, t_aabb)) continue;

        if (node.shape_count > 0u) {
            // Leaf: test triangles.
            for (uint k = 0u; k < node.shape_count; ++k) {
                uint tri = velk_blas_tris.data[blas_tri_base + node.first_shape + k];
                uint base = tri * 3u;
                uint i0 = velk_mesh_index(st, base + 0u);
                uint i1 = velk_mesh_index(st, base + 1u);
                uint i2 = velk_mesh_index(st, base + 2u);
                uint o0 = i0 * floats_per_vert;
                uint o1 = i1 * floats_per_vert;
                uint o2 = i2 * floats_per_vert;
                vec3 v0 = vec3(velk_mesh_vertex(st, o0), velk_mesh_vertex(st, o0 + 1u), velk_mesh_vertex(st, o0 + 2u));
                vec3 v1 = vec3(velk_mesh_vertex(st, o1), velk_mesh_vertex(st, o1 + 1u), velk_mesh_vertex(st, o1 + 2u));
                vec3 v2 = vec3(velk_mesh_vertex(st, o2), velk_mesh_vertex(st, o2 + 1u), velk_mesh_vertex(st, o2 + 2u));

                // Möller-Trumbore. The det rejection threshold scales
                // with the actual edge magnitudes so meshes whose
                // local-space coordinates are tiny (e.g. instances
                // with a large baked-in world scale) don't have every
                // triangle silently filtered out as "near-parallel".
                // The same scaling applies to the per-hit `tt` floor
                // so it's a meaningful "ignore self-intersection"
                // distance regardless of mesh scale.
                vec3 e1 = v1 - v0;
                vec3 e2 = v2 - v0;
                vec3 p  = cross(ld, e2);
                float det = dot(e1, p);
                float det_scale = max(length(e1) * length(p), 1e-30);
                if (abs(det) < 1e-7 * det_scale) continue;
                float inv_det = 1.0 / det;
                vec3 to_v0 = lo - v0;
                float u = dot(to_v0, p) * inv_det;
                if (u < 0.0 || u > 1.0) continue;
                vec3 q = cross(to_v0, e1);
                float v = dot(ld, q) * inv_det;
                if (v < 0.0 || u + v > 1.0) continue;
                float tt = dot(e2, q) * inv_det;
                float tt_floor = 1e-4 * max(length(e1), length(e2));
                if (tt < tt_floor || tt >= best_t) continue;
                best_t = tt;
                best_u = u;
                best_v = v;
                best_o0 = o0;
                best_o1 = o1;
                best_o2 = o2;
                found = true;
            }
        } else {
            // Front-to-back ordered descent for binary inner nodes.
            // Push the far child first so the near one pops next; for
            // closest-hit on a triangle BLAS this lets the far subtree
            // get culled by `best_t` once a near hit lands.
            if (node.child_count == 2u) {
                BvhNode l = velk_blas_nodes.data[blas_node_base + node.first_child];
                BvhNode r = velk_blas_nodes.data[blas_node_base + node.first_child + 1u];
                float t_l, t_r;
                bool h_l = ray_aabb(local_ray, l.aabb_min.xyz, l.aabb_max.xyz, best_t, t_l);
                bool h_r = ray_aabb(local_ray, r.aabb_min.xyz, r.aabb_max.xyz, best_t, t_r);
                if (h_l && h_r) {
                    if (t_l <= t_r) {
                        if (sp < 32) stack[sp++] = node.first_child + 1u;
                        if (sp < 32) stack[sp++] = node.first_child;
                    } else {
                        if (sp < 32) stack[sp++] = node.first_child;
                        if (sp < 32) stack[sp++] = node.first_child + 1u;
                    }
                } else if (h_l) {
                    if (sp < 32) stack[sp++] = node.first_child;
                } else if (h_r) {
                    if (sp < 32) stack[sp++] = node.first_child + 1u;
                }
            } else {
                for (uint c = 0u; c < node.child_count; ++c) {
                    if (sp < 32) stack[sp++] = node.first_child + c;
                }
            }
        }
    }
    if (!found) return false;

    // Interpolate per-vertex normal and UV from the closest hit's
    // barycentrics. Vertex layout (VelkVertex3D, 32 B): pos[0..2],
    // normal[3..5], uv[6..7]. Möller-Trumbore's (u, v) make
    // (1-u-v, u, v) the weights for (V0, V1, V2).
    float w = 1.0 - best_u - best_v;
    vec3 n0 = vec3(velk_mesh_vertex(st, best_o0 + 3u), velk_mesh_vertex(st, best_o0 + 4u), velk_mesh_vertex(st, best_o0 + 5u));
    vec3 n1 = vec3(velk_mesh_vertex(st, best_o1 + 3u), velk_mesh_vertex(st, best_o1 + 4u), velk_mesh_vertex(st, best_o1 + 5u));
    vec3 n2 = vec3(velk_mesh_vertex(st, best_o2 + 3u), velk_mesh_vertex(st, best_o2 + 4u), velk_mesh_vertex(st, best_o2 + 5u));
    vec3 best_n = normalize(n0 * w + n1 * best_u + n2 * best_v);
    vec2 uv0 = vec2(velk_mesh_vertex(st, best_o0 + 6u), velk_mesh_vertex(st, best_o0 + 7u));
    vec2 uv1 = vec2(velk_mesh_vertex(st, best_o1 + 6u), velk_mesh_vertex(st, best_o1 + 7u));
    vec2 uv2 = vec2(velk_mesh_vertex(st, best_o2 + 6u), velk_mesh_vertex(st, best_o2 + 7u));
    vec2 best_uv = uv0 * w + uv1 * best_u + uv2 * best_v;

    // Convert local hit to world-space ray parameter.
    vec3 hit_local = lo + ld * best_t;
    vec3 hit_world = (inst.world * vec4(hit_local, 1.0)).xyz;
    hit.t      = length(hit_world - ray.origin);
    hit.uv     = best_uv;
    hit.normal = normalize(mat3(inst.world) * best_n);
    hit.shape_index = 0u;
    return true;
}
)"
                                                                      R"(
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
bool trace_any_hit(Ray ray, float t_max)
{
    if (VELK_GLOBALS.bvh_node_count == 0u) return false;
    uint stack[32];
    int sp = 0;
    stack[sp++] = VELK_GLOBALS.bvh_root;
    while (sp > 0) {
        uint ni = stack[--sp];
        BvhNode node = velk_bvh_nodes.data[VELK_NODE_BASE +ni];
        float t_hit;
        if (!ray_aabb(ray, node.aabb_min.xyz, node.aabb_max.xyz, t_max, t_hit)) continue;

        for (uint i = 0u; i < node.shape_count; ++i) {
            RtShape s = velk_bvh_shapes.data[VELK_SHAPE_BASE +node.first_shape + i];
            RayHit h;
            if (intersect_shape(ray, s, h) && h.t > 0.0 && h.t < t_max) return true;
        }

        // Front-to-back ordered descent for binary inner nodes:
        // visit the child whose AABB the ray hits NEAREST first, so
        // any-hit can return early without walking the far subtree.
        // Falls back to natural order for non-binary or single-child
        // nodes (none today, but kept defensive).
        if (node.child_count == 2u) {
            BvhNode l = velk_bvh_nodes.data[VELK_NODE_BASE +node.first_child];
            BvhNode r = velk_bvh_nodes.data[VELK_NODE_BASE +node.first_child + 1u];
            float t_l, t_r;
            bool h_l = ray_aabb(ray, l.aabb_min.xyz, l.aabb_max.xyz, t_max, t_l);
            bool h_r = ray_aabb(ray, r.aabb_min.xyz, r.aabb_max.xyz, t_max, t_r);
            // Push far child first so the near one pops next.
            if (h_l && h_r) {
                if (t_l <= t_r) {
                    if (sp < 32) stack[sp++] = node.first_child + 1u;
                    if (sp < 32) stack[sp++] = node.first_child;
                } else {
                    if (sp < 32) stack[sp++] = node.first_child;
                    if (sp < 32) stack[sp++] = node.first_child + 1u;
                }
            } else if (h_l) {
                if (sp < 32) stack[sp++] = node.first_child;
            } else if (h_r) {
                if (sp < 32) stack[sp++] = node.first_child + 1u;
            }
        } else {
            for (uint i = 0u; i < node.child_count; ++i) {
                if (sp < 32) stack[sp++] = node.first_child + i;
            }
        }
    }
    return false;
}

// Shadow dispatch is composed at pipeline-build time from the snippet
// registry's frame_shadow_techs() set. The composer appends each
// tech's snippet #include plus a velk_eval_shadow switch keyed by the
// registry id. tech_id 0 means "no shadow technique" and returns 1.0.
float velk_eval_shadow(uint tech_id, uint light_idx, vec3 world_pos, vec3 world_normal);

// Equirect env sample with explicit mip LOD. The env texture ships
// with a bilinear-downsampled mip chain as a rough roughness
// prefilter; higher `lod` values read blurrier mips. Returns vec3(0)
// when the view has no environment.
vec3 env_miss_color_lod(vec3 rd, float lod)
{
    if (pc.env_texture_id == 0u) return vec3(0.0);
    const float PI = 3.14159265358979323846;
    vec2 params = pc.env_params;
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

// Roughness-aware Schlick for environment / IBL specular: caps the grazing
// reflectance (F90) at (1 - roughness), so rough surfaces don't reflect the
// full environment at glancing angles. Without this, rough geometry edges
// (e.g. painted facades) get an over-bright Fresnel rim of the env.
vec3 fresnel_schlick_roughness(float cos_theta, vec3 F0, float roughness)
{
    vec3 F90 = max(vec3(1.0 - roughness), F0);
    return F0 + (F90 - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

// ACES Filmic tone map (Krzysztof Narkowicz fit). Maps 0..inf HDR
// radiance to 0..1 SDR while preserving mid-tone contrast. Stand-in
// until the renderer grows a dedicated HDR target + composite pass with
// per-camera exposure (see design-notes/tone-mapping.md).
vec3 velk_tonemap_aces(vec3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
)";

// Deferred lighting main: full PBR + env IBL + stochastic single-light shadow.
// Composed on top of `deferred_compute_prelude_src` (plus the intersect_shape /
// velk_eval_shadow switches the composer appends).
//
// Outputs TWO images for the temporal denoiser downstream:
//   irr_image_id   = demodulated diffuse IRRADIANCE (no albedo) - view-
//                    independent, noisy (one stochastic light + jittered
//                    shadow), reprojected + accumulated by the denoise pass.
//   output_image_id= the SHARP "rest": analytic specular + env specular +
//                    emissive (+ unlit albedo / sky). The denoise pass reads
//                    this back, adds albedo*(1-metallic)*denoised_irradiance
//                    in place, and produces the final HDR image.
// Split from the main body below only because MSVC caps a single string
// literal at 16380 bytes. Appended after the prelude, whose Light / RtShape /
// BvhNode declarations it depends on.
[[maybe_unused]] constexpr string_view deferred_area_light_src = R"(
// Analytic area-light shading. The clamped-cosine integral over the emitter's
// spherical polygon, evaluated in closed form: no samples, no noise, nothing
// for the denoiser to resolve. Validated against Monte Carlo in
// design-notes/spike_analytic/analytic.cpp before being written here.

// Vertex capacity of every polygon handled here. The widest case is a
// sphere's 8-gon silhouette, which each clip can grow by one vertex per
// plane: one horizon, one light plane, and one per light-polygon edge.
const int kOccPolyMax = 16;

// One edge's contribution to the vector form factor.
vec3 velk_ff_edge(vec3 a, vec3 b)
{
    float c = clamp(dot(a, b), -1.0, 1.0);
    vec3  e = cross(a, b);
    float el = length(e);
    return (el > 1e-8) ? e * (acos(c) / el) : vec3(0.0);
}

// Clips a polygon against the half-space dot(nrm, p) >= 0, where the plane
// passes through the shading point. Used for both world-relative points and
// directions: the plane goes through the origin, so clipping the chord and
// clipping the great-circle arc give the same direction once normalised.
int velk_clip_half(inout vec3 p[kOccPolyMax], int n, vec3 nrm)
{
    vec3 o[kOccPolyMax];
    int m = 0;
    for (int i = 0; i < n; ++i) {
        vec3 a = p[i];
        vec3 b = p[i + 1 == n ? 0 : i + 1];
        float da = dot(nrm, a);
        float db = dot(nrm, b);
        if (da >= 0.0 && m < kOccPolyMax) { o[m] = a; ++m; }
        if ((da >= 0.0) != (db >= 0.0) && m < kOccPolyMax) {
            o[m] = mix(a, b, da / (da - db));
            ++m;
        }
    }
    for (int i = 0; i < m; ++i) p[i] = o[i];
    return m;
}

// Clips against a plane through an arbitrary point rather than the origin.
int velk_clip_at(inout vec3 p[kOccPolyMax], int n, vec3 pt, vec3 nrm)
{
    vec3 o[kOccPolyMax];
    int m = 0;
    for (int i = 0; i < n; ++i) {
        vec3 a = p[i];
        vec3 b = p[i + 1 == n ? 0 : i + 1];
        float da = dot(nrm, a - pt);
        float db = dot(nrm, b - pt);
        if (da >= 0.0 && m < kOccPolyMax) { o[m] = a; ++m; }
        if ((da >= 0.0) != (db >= 0.0) && m < kOccPolyMax) {
            o[m] = mix(a, b, da / (da - db));
            ++m;
        }
    }
    for (int i = 0; i < m; ++i) p[i] = o[i];
    return m;
}

// Vector form factor of a spherical polygon given as directions.
vec3 velk_form_factor(vec3 p[kOccPolyMax], int n)
{
    vec3 f = vec3(0.0);
    for (int i = 0; i < n; ++i) {
        f += velk_ff_edge(normalize(p[i]), normalize(p[i + 1 == n ? 0 : i + 1]));
    }
    return f;
}

// Clips a spherical polygon against another convex one, by that polygon's
// great-circle edge planes. Normals are oriented against the clip polygon's
// own centroid so winding does not matter.
int velk_clip_by_poly(inout vec3 p[kOccPolyMax], int n, vec3 c[kOccPolyMax], int cn)
{
    vec3 centre = vec3(0.0);
    for (int i = 0; i < cn; ++i) centre += normalize(c[i]);
    centre = normalize(centre);

    for (int i = 0; i < cn && n >= 3; ++i) {
        vec3 nrm = cross(normalize(c[i]), normalize(c[i + 1 == cn ? 0 : i + 1]));
        if (length(nrm) < 1e-8) continue;
        nrm = normalize(nrm);
        if (dot(nrm, centre) < 0.0) nrm = -nrm;
        n = velk_clip_half(p, n, nrm);
    }
    return n;
}

// One occluder polygon's blocked form factor, given its corners already in the
// shading frame. Discards anything below the shading horizon or beyond the
// light plane, keeps what overlaps the light, and returns the magnitude of
// what is left. The magnitude, because the clipped region carries the
// OCCLUDER's winding, which follows an arbitrary axis order rather than the
// light's.
float velk_occ_form_factor(vec3 op[kOccPolyMax], int n,
                           vec3 lc, vec3 lnf, vec3 lp[kOccPolyMax], int ln_count)
{
    n = velk_clip_half(op, n, vec3(0.0, 0.0, 1.0));
    if (n < 3) return 0.0;
    n = velk_clip_at(op, n, lc, lnf);
    if (n < 3) return 0.0;
    n = velk_clip_by_poly(op, n, lp, ln_count);
    if (n < 3) return 0.0;
    return abs(velk_form_factor(op, n).z);
}

)"
                                                                      R"(
// Inverse of the LTC transform for (roughness, N.V), from the fitted table.
// The table stores M as (m00, m02, m11, m20) with m22 == 1, and M has no
// coupling between y and the xz plane, so the inverse is closed-form rather
// than a general 3x3 solve. Returned unnormalised: the polygon integral
// normalises its vertices as directions, so any uniform scale on M cancels.
mat3 velk_ltc_minv(float roughness, float ndotv)
{
    // Table axes match the generator: x = roughness, y = sqrt(1 - N.V), which
    // spends resolution near grazing where the lobe changes fastest.
    vec2 uv = vec2(clamp(roughness, 0.0, 1.0), sqrt(clamp(1.0 - ndotv, 0.0, 1.0)));
    vec4 t = velk_texture(pc.ltc_matrix_id, uv);
    float a = t.x, b = t.y, c = t.z, d = t.w;
    float k = a - b * d;
    if (abs(k) < 1e-6) k = (k < 0.0) ? -1e-6 : 1e-6;
    // Columns, GLSL-style: mat3(col0, col1, col2).
    return mat3(vec3(1.0, 0.0, -d),
                vec3(0.0, k / max(c, 1e-6), 0.0),
                vec3(-b, 0.0, a));
}

// One occluder polygon's contribution to BOTH terms, from a single clip of the
// shared spatial rejection. `op` holds its corners in the shading frame.
//
// Diffuse and specular differ only by the transform applied before the
// integral, so the traversal, the corner construction and the "is it even
// between the point and the light" tests are done once. The spatial tests must
// happen in the ORIGINAL space (they are about position, not lobe shape); the
// transform is applied only to the surviving region.
void velk_occ_accum(vec3 op[kOccPolyMax], int n,
                    vec3 lc, vec3 lnf, vec3 lp[kOccPolyMax], int ln_count,
                    mat3 minv, vec3 lps[kOccPolyMax], int ls_count,
                    inout float f_occ, inout float s_occ)
{
    n = velk_clip_half(op, n, vec3(0.0, 0.0, 1.0));
    if (n < 3) return;
    n = velk_clip_at(op, n, lc, lnf);
    if (n < 3) return;

    vec3 od[kOccPolyMax];
    for (int i = 0; i < n; ++i) od[i] = op[i];
    int nd = velk_clip_by_poly(od, n, lp, ln_count);
    if (nd >= 3) f_occ += abs(velk_form_factor(od, nd).z);

    if (ls_count >= 3) {
        vec3 os[kOccPolyMax];
        for (int i = 0; i < n; ++i) os[i] = minv * op[i];
        int ns = velk_clip_half(os, n, vec3(0.0, 0.0, 1.0));
        if (ns >= 3) {
            ns = velk_clip_by_poly(os, ns, lps, ls_count);
            if (ns >= 3) s_occ += abs(velk_form_factor(os, ns).z);
        }
    }
}

// Diffuse irradiance (.x) and the LTC specular integral (.y) from a square area
// light at `world_pos`, with scene geometry subtracted analytically from BOTH.
// Rects, boxes and spheres block; triangle meshes do not, having no silhouette
// cheap enough to clip yet.
//
// The specular shadow is analytic here rather than a stochastically estimated
// shadowed/unshadowed ratio: the LTC transform maps the GGX lobe onto a clamped
// cosine, so the identical polygon subtraction applies to transformed vertices.
// That keeps specular noise-free too, and costs one extra clip per occluder
// rather than a second traversal.
//
// C2a scope: each occluder's overlap with the light is subtracted
// independently. That is exact for a single blocker and for blockers that do
// not overlap ON THE LIGHT, and over-darkens where two do, since the shared
// region is subtracted twice. Front-to-back convex subtraction (which keeps
// the visible region as disjoint pieces and is what the Step A harness
// validated) is C2b.
vec2 velk_area_irradiance(Light light, vec3 world_pos, vec3 N, vec3 V, float roughness)
{
    // Shading frame with N as +Z and the view in the +XZ half-plane. The
    // diffuse integral only reads .z so it does not care about the rotation
    // about N, while LTC requires exactly this frame - so one frame serves
    // both and every polygon is built once.
    vec3 nt = V - N * dot(V, N);
    nt = (dot(nt, nt) > 1e-12) ? normalize(nt)
                              : ((abs(N.y) < 0.9) ? normalize(cross(vec3(0.0, 1.0, 0.0), N))
                                                  : normalize(cross(vec3(1.0, 0.0, 0.0), N)));
    vec3 nb = cross(N, nt);

    vec3 ln = normalize(light.direction.xyz);
    vec3 lt = (abs(ln.y) < 0.9) ? normalize(cross(vec3(0.0, 1.0, 0.0), ln))
                                : normalize(cross(vec3(1.0, 0.0, 0.0), ln));
    vec3 lb = cross(ln, lt);
    float hs = max(light.params.w, 1e-4);

    // Lift the shading point off its own surface, the analytic equivalent of a
    // shadow-ray bias. Without it a surface is exactly coplanar with its own
    // shading plane, the horizon clip keeps all four of its vertices (da >= 0),
    // and the resulting degenerate polygon lying on the horizon subtracts a
    // large, meaningless form factor. Which way each pixel falls then depends
    // on float noise in the G-buffer world position, which is generated per
    // triangle: the surface self-shadows in a triangular pattern.
    // Scaled by the light distance so it holds across scene scales.
    vec3 to_light = light.position.xyz - world_pos;
    vec3 shade_pos = world_pos + N * max(1e-3 * length(to_light), 1e-4);

    vec3 ctr = light.position.xyz - shade_pos;
    vec3 ea = lt * hs;
    vec3 eb = lb * hs;

    vec3 lw[4];
    lw[0] = ctr - ea - eb;
    lw[1] = ctr + ea - eb;
    lw[2] = ctr + ea + eb;
    lw[3] = ctr - ea + eb;

    vec3 lp[kOccPolyMax];
    for (int i = 0; i < 4; ++i) {
        lp[i] = vec3(dot(lw[i], nt), dot(lw[i], nb), dot(lw[i], N));
    }
    // Unclipped corners are kept: the specular polygon is transformed from
    // these and clipped in ITS own space, since the horizon that matters there
    // is the transformed one.
    vec3 lraw[4];
    for (int i = 0; i < 4; ++i) {
        lraw[i] = vec3(dot(lw[i], nt), dot(lw[i], nb), dot(lw[i], N));
        lp[i] = lraw[i];
    }
    int ln_count = velk_clip_half(lp, 4, vec3(0.0, 0.0, 1.0));
    if (ln_count < 3) return vec2(0.0);

    vec3 f_total = velk_form_factor(lp, ln_count);
    float f_max = abs(f_total.z);

    // Specular: the same emitter through the LTC transform. A zero table id
    // means the fit is unavailable, and the light stays diffuse-only rather
    // than losing its highlight to a black or garbage lookup.
    mat3 minv = mat3(1.0);
    vec3 lps[kOccPolyMax];
    int ls_count = 0;
    float s_max = 0.0;
    if (pc.ltc_matrix_id != 0u) {
        minv = velk_ltc_minv(roughness, clamp(dot(N, V), 0.0, 1.0));
        for (int i = 0; i < 4; ++i) lps[i] = minv * lraw[i];
        ls_count = velk_clip_half(lps, 4, vec3(0.0, 0.0, 1.0));
        if (ls_count >= 3) s_max = abs(velk_form_factor(lps, ls_count).z);
    }

    // Light plane in the shading frame, with its normal oriented so the
    // shading point tests positive. Occluders are then kept on that same
    // side: anything behind the light shadows nothing.
    vec3 lc = vec3(dot(ctr, nt), dot(ctr, nb), dot(ctr, N));
    vec3 lnf = normalize(vec3(dot(ln, nt), dot(ln, nb), dot(ln, N)));
    if (dot(lnf, -lc) < 0.0) lnf = -lnf;

    // Shaft planes, in world space, bounding everything that could possibly
    // block this pixel: the shading plane, the light's plane, and the four
    // sides of the pyramid from the shading point through the light quad.
    // Built once per pixel and tested against each BVH node's AABB, which is
    // what keeps the walk off the shapes that cannot matter. The planes are
    // deliberately built from the UNCLIPPED light quad, so they stay
    // conservative. Normals are left unnormalised: the AABB test scales with
    // them, so it does not care.
    vec3 cull_n[6];
    vec3 cull_p[6];
    cull_n[0] = N;
    cull_p[0] = shade_pos;
    cull_n[1] = (dot(ln, shade_pos - light.position.xyz) > 0.0) ? ln : -ln;
    cull_p[1] = light.position.xyz;
    for (int i = 0; i < 4; ++i) {
        vec3 nr = cross(lw[i], lw[(i + 1) & 3]);
        if (dot(nr, ctr) < 0.0) nr = -nr;
        cull_n[2 + i] = nr;
        cull_p[2 + i] = shade_pos;
    }

    float f_occ = 0.0;
    float s_occ = 0.0;
    if (VELK_GLOBALS.bvh_node_count > 0u) {
        uint stack[32];
        int sp = 0;
        stack[sp++] = VELK_GLOBALS.bvh_root;
        // Stops early once the light is fully blocked: nothing further can
        // change the result, and this is the common case deep in a shadow.
        // BOTH terms have to be saturated, since the specular lobe can still
        // see a sliver of emitter after the diffuse hemisphere is covered.
        while (sp > 0 && (f_occ < f_max || s_occ < s_max)) {
            uint ni = stack[--sp];
            BvhNode node = velk_bvh_nodes.data[VELK_NODE_BASE + ni];

            vec3 bc = 0.5 * (node.aabb_min.xyz + node.aabb_max.xyz);
            vec3 be = 0.5 * (node.aabb_max.xyz - node.aabb_min.xyz);
            bool outside = false;
            for (int p = 0; p < 6; ++p) {
                if (dot(cull_n[p], bc - cull_p[p]) + dot(abs(cull_n[p]), be) < 0.0) {
                    outside = true;
                    break;
                }
            }
            if (outside) continue;

            for (uint i = 0u; i < node.shape_count; ++i) {
                RtShape s = velk_bvh_shapes.data[VELK_SHAPE_BASE + node.first_shape + i];
                if (s.shape_kind == 255u) continue;  // meshes: no silhouette yet

                // Shape geometry, relative to the shading point. Origin is a
                // corner and the axes span the shape, matching what the
                // intersectors above assume.
                vec3 o = s.origin.xyz - shade_pos;
                vec3 u = s.u_axis.xyz;
                vec3 v = s.v_axis.xyz;
                vec3 w = s.w_axis.xyz;
                vec3 op[kOccPolyMax];

                if (s.shape_kind == 1u) {
                    // Box. Each opposing face pair contributes the one face that
                    // turns toward the shading point, or NEITHER when the
                    // shading point lies between the pair's two planes: seen
                    // edge-on, that pair is not part of the silhouette at all,
                    // and taking one of its faces anyway subtracts a sliver
                    // that blocks nothing. The faces that do qualify tile the
                    // silhouette without overlapping on the sphere, so
                    // subtracting them independently is exact and no silhouette
                    // has to be extracted. Checked against Monte Carlo in
                    // design-notes/spike_analytic/analytic.cpp.
                    for (int f = 0; f < 3; ++f) {
                        vec3 ea2 = (f == 0) ? u : ((f == 1) ? v : w);
                        vec3 eb2 = (f == 0) ? v : ((f == 1) ? w : u);
                        vec3 ec2 = (f == 0) ? w : ((f == 1) ? u : v);
                        vec3 fc0 = o + 0.5 * (ea2 + eb2);
                        vec3 base;
                        if (dot(ec2, fc0) > 0.0)            base = o;
                        else if (dot(ec2, fc0 + ec2) < 0.0) base = o + ec2;
                        else                                continue;
                        vec3 fw[4];
                        fw[0] = base;
                        fw[1] = base + ea2;
                        fw[2] = base + ea2 + eb2;
                        fw[3] = base + eb2;
                        for (int k = 0; k < 4; ++k) {
                            op[k] = vec3(dot(fw[k], nt), dot(fw[k], nb), dot(fw[k], N));
                        }
                        velk_occ_accum(op, 4, lc, lnf, lp, ln_count,
                                       minv, lps, ls_count, f_occ, s_occ);
                    }
                } else if (s.shape_kind == 2u) {
                    // Sphere. Its silhouette is a cone, approximated by a
                    // regular polygon about the cone axis. An inscribed polygon
                    // would miss the slivers between chord and arc, so the
                    // half-angle is widened by sqrt(pi / ((n/2) sin(2pi/n)))
                    // to match the cap's area instead.
                    vec3 sc = o + 0.5 * (u + v + w);
                    float sd = length(sc);
                    float sr = s.params.x;
                    if (sd <= sr) continue;  // shading point inside the sphere
                    float ha = min(asin(clamp(sr / sd, 0.0, 1.0)) * 1.0539, 1.5);
                    vec3 sdir = sc / sd;
                    vec3 st1 = (abs(sdir.y) < 0.9)
                             ? normalize(cross(vec3(0.0, 1.0, 0.0), sdir))
                             : normalize(cross(vec3(1.0, 0.0, 0.0), sdir));
                    vec3 st2 = cross(sdir, st1);
                    float ca = cos(ha);
                    float sa = sin(ha);
                    for (int k = 0; k < 8; ++k) {
                        float ph = 6.28318531 * float(k) * 0.125;
                        vec3 dv = sdir * ca + (st1 * cos(ph) + st2 * sin(ph)) * sa;
                        // Placed at the centre's distance rather than left as a
                        // direction: the light-plane clip tests points, so the
                        // silhouette has to sit at the right depth.
                        vec3 pw = dv * sd;
                        op[k] = vec3(dot(pw, nt), dot(pw, nb), dot(pw, N));
                    }
                    velk_occ_accum(op, 8, lc, lnf, lp, ln_count,
                                   minv, lps, ls_count, f_occ, s_occ);
                } else {
                    // Rect. Every shape emitted from a draw entry is one,
                    // spanned by origin + u_axis + v_axis, whether or not its
                    // visual registered an intersect snippet: a registered kind
                    // (3 and up) only refines coverage INSIDE that rect (rounded
                    // corners, glyph SDF), which is treated here as fully
                    // covering.
                    vec3 ow[4];
                    ow[0] = o;
                    ow[1] = o + u;
                    ow[2] = o + u + v;
                    ow[3] = o + v;
                    for (int k = 0; k < 4; ++k) {
                        op[k] = vec3(dot(ow[k], nt), dot(ow[k], nb), dot(ow[k], N));
                    }
                    velk_occ_accum(op, 4, lc, lnf, lp, ln_count,
                                   minv, lps, ls_count, f_occ, s_occ);
                }
            }

            for (uint c = 0u; c < node.child_count && sp < 30; ++c) {
                stack[sp++] = node.first_child + c;
            }
        }
    }

    float e = (f_max - f_occ) / (2.0 * 3.14159265);
    float s = (s_max - s_occ) / (2.0 * 3.14159265);
    return vec2(max(e, 0.0), max(s, 0.0));
}
)";

[[maybe_unused]] constexpr string_view deferred_lighting_main_src = R"(
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
        mat4 inv_vp = VELK_GLOBALS.inverse_view_projection;
        vec4 near_h = inv_vp * vec4(ndc, 0.0, 1.0);
        vec4 far_h  = inv_vp * vec4(ndc, 1.0, 1.0);
        vec3 near_w = near_h.xyz / near_h.w;
        vec3 far_w  = far_h.xyz  / far_h.w;
        vec3 rd = normalize(far_w - near_w);
        vec3 sky = env_miss_color(rd);
        // Sky is sharp (no diffuse irradiance): write it to the "rest" image
        // and zero irradiance. The denoise/composite pass passes it through
        // (albedo == 0 for sky pixels, so it adds nothing).
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(sky, 1.0));
        imageStore(gStorageImagesF16[nonuniformEXT(pc.irr_image_id)], coord, vec4(0.0));
        return;
    }

    // Emissive radiance, added to the lit output below (sky pixels return
    // above without reading it). HDR; folded in before tonemap so bloom
    // can pick up bright emitters.
    vec3 emissive = velk_texture(pc.emissive_tex_id, uv).rgb;

    // LightingMode encoded in mat.b (0 = Unlit, 1 = Standard, ...).
    uint lighting_mode = uint(mat.b * 255.0 + 0.5);
    float metallic  = clamp(mat.r, 0.0, 1.0);
    float roughness = clamp(mat.g, 0.04, 1.0);

    // irr = demodulated diffuse irradiance (no albedo; denoised downstream);
    // rest = the sharp, view-dependent remainder (specular / env spec / emissive).
    vec3 irr  = vec3(0.0);
    vec3 rest = vec3(0.0);
    if (lighting_mode == 0u) {
        // Unlit: emit albedo as-is (no diffuse irradiance).
        rest = albedo.rgb;
    } else {
        vec3 N = normalize(world_n);
        vec3 V = normalize(pc.cam_pos.xyz - world_pos);
        float NdotV = max(dot(N, V), 0.0);

        vec3 F0 = mix(vec3(0.04), albedo.rgb, metallic);
        float a = roughness * roughness;

        // Direct lighting. The shadowed diffuse term is estimated by
        // resampled importance sampling (RIS): in one pass over the lights we
        // reservoir-pick a SINGLE light (weighted by its unshadowed diffuse
        // luminance) and trace one jittered shadow ray for it, dividing by the
        // pick pdf for an unbiased estimate of the sum over all lights. This
        // keeps the shadow cost at one ray/pixel regardless of light count
        // (scales to many / dynamic lights); the noise it introduces is what
        // the temporal + spatial denoiser resolves. Specular is summed
        // analytically over ALL lights but UNSHADOWED (no rays, stays sharp;
        // a v1 approximation — specular shadowing is a later refinement).
        vec3 direct_specular = vec3(0.0);
        uint  res_light = 0xffffffffu;
        float res_w = 0.0;        // chosen light's reservoir weight
        float res_NdotL = 0.0;
        vec3  res_radiance = vec3(0.0);
        float total_w = 0.0;
        uint seed = (uint(coord.x) * 1973u + uint(coord.y) * 9277u
                     + VELK_GLOBALS.present_counter * 26699u) | 1u;
        // Analytic area lights, accumulated outside the reservoir. Their
        // diffuse term is exact and noise-free, so they neither need a shadow
        // ray nor belong in the stochastic estimator.
        // Both terms are analytic, including their shadows, so neither is
        // denoised: the diffuse half is demodulated into `rest` below and the
        // specular half joins the other sharp specular sources.
        vec3 area_irr = vec3(0.0);
        vec3 area_spec = vec3(0.0);

        for (uint i = 0u; i < pc.light_count; ++i) {
            Light light = velk_lights.data[pc.lights_base + i];
            if (light.flags.x == 3u) {
                vec2 es = velk_area_irradiance(light, world_pos, N, V, roughness);
                vec3 radiance = light.color_intensity.rgb * light.color_intensity.a;
                if (es.x > 0.0) {
                    // pi converts clamped-cosine units to the same scale as a
                    // delta light's N.L, so authored intensities stay comparable.
                    area_irr += radiance * es.x * 3.14159265;
                }
                if (es.y > 0.0 && pc.ltc_magnitude_id != 0u) {
                    // Split-sum: the fitted pair is (lobe energy, Fresnel
                    // weight), applied the same way the env specular applies
                    // its BRDF LUT, so both specular sources stay on one scale.
                    vec2 t2 = velk_texture(pc.ltc_magnitude_id,
                                           vec2(clamp(roughness, 0.0, 1.0),
                                                sqrt(clamp(1.0 - max(dot(N, V), 0.0), 0.0, 1.0)))).xy;
                    area_spec += radiance * es.y * (F0 * t2.x + (1.0 - F0) * t2.y)
                                 * 3.14159265;
                }
                continue;
            }
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
            vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * atten;

            // Analytic specular (unshadowed), summed over all lights.
            vec3 H = normalize(L + V);
            float NdotH = max(dot(N, H), 0.0);
            float VdotH = max(dot(V, H), 0.0);
            float D = ggx_d(NdotH, a);
            float G = smith_g(NdotV, NdotL, roughness);
            vec3  F = fresnel_schlick(VdotH, F0);
            direct_specular += (D * G) * F / max(4.0 * NdotV * NdotL, 1e-6) * NdotL * radiance;

            // Reservoir step: weight = luminance of this light's unshadowed
            // diffuse contribution.
            vec3 dcontrib = albedo.rgb * NdotL * radiance;
            float w = dot(dcontrib, vec3(0.2126, 0.7152, 0.0722));
            total_w += w;
            seed = seed * 1664525u + 1013904223u;
            if (w > 0.0 && float(seed) * (1.0 / 4294967296.0) < w / total_w) {
                res_light = i;
                res_w = w;
                res_NdotL = NdotL;
                res_radiance = radiance;
            }
        }

        // Shadowed diffuse IRRADIANCE estimate from the single chosen light
        // (one ray), demodulated (no albedo - reapplied at composite).
        vec3 direct_diffuse = vec3(0.0);
        if (res_light != 0xffffffffu && res_w > 0.0) {
            Light chosen = velk_lights.data[pc.lights_base + res_light];
            float shadow = velk_eval_shadow(chosen.flags.y, res_light, world_pos, N);
            float pdf = res_w / total_w;
            direct_diffuse = res_NdotL * res_radiance * (shadow / pdf);
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
        vec3 F_env = fresnel_schlick_roughness(NdotV, F0, roughness);
        vec3 kD_env = (vec3(1.0) - F_env) * (1.0 - metallic);

        // RT ambient occlusion: one short any-hit ray along N. Without
        // this the env term is unshadowed and dominates outdoor scenes,
        // so geometry-cast shadows are invisible even when per-light
        // shadow rays correctly occlude. Binary visibility is crude
        // (real RT-AO would integrate hemispherical samples) but catches
        // the dominant case where surfaces sit under overhangs / inside
        // pockets reading sky they cannot actually see. Range is short
        // (interior-crevice scale) because at scene scale a longer ray
        // hits something for nearly every pixel in dense scenes, and
        // binary occlusion at that point goes black instead of softly
        // darker. Skipped when no BVH exists so non-RT scenes are
        // unaffected.
        // Contact-shadow / ambient occlusion: one short any-hit ray
        // along the surface normal. Approximates "how much of the
        // hemisphere above this surface is blocked by close geometry".
        // Range is short (interior-crevice scale) — we don't try to
        // catch full furniture-sized contact shadows here because a
        // longer ray on dense scenes blacks out vertical surfaces
        // (every wall has something within a meter). For real
        // long-range contact shadow we'd want stochastic hemisphere
        // sampling + temporal accumulation; binary single-ray
        // visibility just covers the obvious crevice case.
        float ao = 1.0;
        if (VELK_GLOBALS.bvh_node_count != 0u) {
            const float ao_range = 0.3;
            Ray ao_r;
            ao_r.origin = world_pos + N * 0.01;
            ao_r.dir    = N;
            ao = trace_any_hit(ao_r, ao_range) ? 0.0 : 1.0;
        }

        // AO modulates env_diffuse only (the hemisphere-irradiance
        // approximation that the AO ray actually approximates). It
        // does NOT modulate env_specular — a mirror reflection is
        // the radiance arriving along the reflection direction, which
        // is occluded by whatever sits along *that* ray, not by what
        // sits along the surface normal. Multiplying specular by AO
        // makes a metallic sphere on a floor go black on its lower
        // hemisphere because its outward-pointing normals all point
        // toward the nearby floor.
        // Diffuse IRRADIANCE (demodulated, view-independent) for the denoiser:
        // the stochastic direct term + env diffuse * AO. albedo and the
        // (1-metallic) factor are reapplied at composite. (Env diffuse's
        // (1-F_env) energy factor is dropped so it shares the direct term's
        // albedo*(1-metallic) demodulation; ~minor.)
        irr = direct_diffuse + env_diffuse * ao;
        // Sharp, view-dependent terms (NOT denoised).
        rest = direct_specular + F_env * env_specular;
        // Area lights are integrated in closed form, including their
        // visibility, so their diffuse term carries no noise and must not be
        // denoised: blurring it would throw away the sharp penumbra the
        // analytic path exists to produce. It goes to the sharp output
        // instead, carrying the albedo * (1 - metallic) factor that composite
        // would otherwise have applied on its way out of the irradiance
        // image, so the result is identical bar the blur.
        rest += albedo.rgb * (1.0 - metallic) * area_irr + area_spec;
    }

    // Emissive is additive radiance; part of the sharp output.
    rest += emissive;

    // Two outputs (raw HDR linear): "rest" (sharp) on output_image_id, diffuse
    // irradiance on irr_image_id. The denoise/composite pass reprojects +
    // accumulates the irradiance and folds albedo*(1-metallic)*irr into rest.
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(rest, albedo.a));
    imageStore(gStorageImagesF16[nonuniformEXT(pc.irr_image_id)], coord, vec4(irr, 1.0));
}
)";

// Diffuse-irradiance TEMPORAL accumulation. Reprojects the noisy stochastic
// diffuse irradiance by world position (irradiance is view-independent, so the
// reprojection is exact under camera motion) and folds it into a running mean.
// Writes only the ping-pong history (irradiance+count, world-position+validity);
// the spatial+composite pass below filters this history and produces the final
// image. `reset` discards history on the first frame / resize.
[[maybe_unused]] constexpr string_view deferred_denoise_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

// velk_globals (set = 1 slot 2) is declared in velk.glsl.
#define VELK_GLOBALS velk_globals.data[pc.globals_base]

layout(push_constant, scalar) uniform PC {
    uint globals_base;         // offset 0  (view's FrameGlobals index)
    uint _pad_globals;
    uint normal_id;
    uint worldpos_id;
    uint irr_id;               // current-frame noisy diffuse irradiance (sampled)
    uint hist_irr_prev_id;     // sampled
    uint hist_pos_prev_id;     // sampled
    uint hist_mom_prev_id;     // sampled (.r = accumulated luminance 2nd moment)
    uint hist_irr_cur_id;      // storage
    uint hist_pos_cur_id;      // storage
    uint hist_mom_cur_id;      // storage
    uint width;
    uint height;
    uint reset;
} pc;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;
    vec2 uv = (vec2(coord) + 0.5) / vec2(float(pc.width), float(pc.height));

    vec3 world_n = velk_texture(pc.normal_id, uv).xyz;

    // Sky / no coverage: store empty history (validity 0).
    if (dot(world_n, world_n) < 1e-6) {
        imageStore(gStorageImagesF16[nonuniformEXT(pc.hist_irr_cur_id)], coord, vec4(0.0));
        imageStore(gStorageImagesF16[nonuniformEXT(pc.hist_pos_cur_id)], coord, vec4(0.0));
        imageStore(gStorageImagesF16[nonuniformEXT(pc.hist_mom_cur_id)], coord, vec4(0.0));
        return;
    }

    const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);
    vec3 cur_irr   = velk_texture(pc.irr_id, uv).rgb;
    vec3 world_pos = velk_texture(pc.worldpos_id, uv).xyz;
    float cur_lum  = dot(cur_irr, LUMA);

    // Temporal reprojection: where was this world point last frame?
    vec3 irr_acc = cur_irr;
    float count = 1.0;
    float m2_acc = cur_lum * cur_lum; // accumulated 2nd moment of luminance
    if (pc.reset == 0u) {
        vec4 prev_clip = VELK_GLOBALS.prev_view_projection * vec4(world_pos, 1.0);
        if (prev_clip.w > 1e-6) {
            vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
            if (all(greaterThanEqual(prev_uv, vec2(0.0))) &&
                all(lessThanEqual(prev_uv, vec2(1.0)))) {
                vec4 hp = velk_texture(pc.hist_pos_prev_id, prev_uv);
                vec4 hi = velk_texture(pc.hist_irr_prev_id, prev_uv);
                // Same surface? World-space distance, tolerance scaled by depth.
                if (hp.w > 0.5 && length(hp.xyz - world_pos) < 0.03 * max(prev_clip.w, 1.0)) {
                    // Fixed accumulation window. Shorter = shorter dynamic-
                    // occluder trail, slightly noisier static; the spatial pass'
                    // variance-guided filter cleans the residual. Tunable.
                    count = min(hi.a + 1.0, 20.0);
                    float w = 1.0 / count;
                    irr_acc = mix(hi.rgb, cur_irr, w);
                    float m2_prev = velk_texture(pc.hist_mom_prev_id, prev_uv).r;
                    m2_acc = mix(m2_prev, cur_lum * cur_lum, w);
                }
            }
        }
    }

    imageStore(gStorageImagesF16[nonuniformEXT(pc.hist_irr_cur_id)], coord, vec4(irr_acc, count));
    imageStore(gStorageImagesF16[nonuniformEXT(pc.hist_pos_cur_id)], coord, vec4(world_pos, 1.0));
    imageStore(gStorageImagesF16[nonuniformEXT(pc.hist_mom_cur_id)], coord, vec4(m2_acc, 0.0, 0.0, 0.0));
}
)";

// Variance-guided spatial (a-trous-style) filter + composite. Reads the
// accumulated irradiance history (NOT modifying it - the unfiltered mean stays
// the temporal history) over a normal + plane-distance + LUMINANCE weighted
// neighborhood. The luminance edge-stop tolerance scales with the per-pixel
// temporal VARIANCE (variance = 2nd moment - mean^2, both luminance): noisy /
// just-changed pixels (high variance) blur freely, converged pixels (low
// variance) keep their detail, and a real shadow edge (large luminance jump)
// is preserved at any variance. Low sample counts widen the support + loosen
// the luminance gate (the variance estimate is unreliable there). Then
// composites the final image (albedo*(1-metallic)*filtered + sharp rest, read
// back in place from the output) for the surface blit.
[[maybe_unused]] constexpr string_view deferred_spatial_composite_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

// velk_globals (set = 1 slot 2) is declared in velk.glsl.
#define VELK_GLOBALS velk_globals.data[pc.globals_base]

layout(push_constant, scalar) uniform PC {
    uint globals_base;         // offset 0  (view's FrameGlobals index; cam_pos via VELK_GLOBALS)
    uint _pad_globals;
    uint albedo_id;
    uint material_id;
    uint normal_id;
    uint worldpos_id;
    uint hist_irr_id;          // accumulated irradiance + count (sampled, neighborhood)
    uint hist_mom_id;          // accumulated luminance 2nd moment (.r)
    uint output_id;            // deferred_output: "rest" on read, final on write
    uint width;
    uint height;
} pc;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;
    vec2 dims = vec2(float(pc.width), float(pc.height));
    vec2 uv = (vec2(coord) + 0.5) / dims;

    vec3 world_n = velk_texture(pc.normal_id, uv).xyz;
    vec4 rest    = velk_texture(pc.output_id, uv);

    // Sky / no coverage: pass the sharp value through unchanged.
    if (dot(world_n, world_n) < 1e-6) {
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], coord, rest);
        return;
    }

    const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);
    vec3 N         = normalize(world_n);
    vec3 world_pos = velk_texture(pc.worldpos_id, uv).xyz;
    vec4 albedo    = velk_texture(pc.albedo_id, uv);
    float metallic = clamp(velk_texture(pc.material_id, uv).r, 0.0, 1.0);

    vec4 c = velk_texture(pc.hist_irr_id, uv);
    vec3 center_irr = c.rgb;
    float count = c.a;
    float center_lum = dot(center_irr, LUMA);

    // Per-pixel temporal variance of luminance -> noise level.
    float m2 = velk_texture(pc.hist_mom_id, uv).r;
    float variance = max(m2 - center_lum * center_lum, 0.0);
    // Luminance edge-stop tolerance: scene-scaled by sigma, with a small
    // relative floor so converged-flat areas don't divide by ~0. Loosened a lot
    // while the count is low (variance estimate unreliable -> trust neighbours).
    float low_count = mix(8.0, 1.0, clamp(count / 8.0, 0.0, 1.0));
    float phi_l = (4.0 * sqrt(variance) + 0.02 * center_lum + 1e-4) * low_count;

    // Wider taps when unconverged; tightens to a 5x5 once converged.
    int step = int(mix(3.0, 1.0, clamp(count / 16.0, 0.0, 1.0)) + 0.5);
    float depth = max(length(VELK_GLOBALS.cam_pos.xyz - world_pos), 1e-3);

    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            ivec2 c2 = coord + ivec2(i, j) * step;
            if (c2.x < 0 || c2.y < 0 || c2.x >= int(pc.width) || c2.y >= int(pc.height)) continue;
            vec2 uv2 = (vec2(c2) + 0.5) / dims;
            vec3 n2 = velk_texture(pc.normal_id, uv2).xyz;
            if (dot(n2, n2) < 1e-6) continue; // sky neighbour
            vec3 p2  = velk_texture(pc.worldpos_id, uv2).xyz;
            vec3 ir2 = velk_texture(pc.hist_irr_id, uv2).rgb;
            float wn = pow(max(dot(N, normalize(n2)), 0.0), 32.0);
            float wp = exp(-abs(dot(p2 - world_pos, N)) / (0.05 * depth));
            float wl = exp(-abs(center_lum - dot(ir2, LUMA)) / phi_l); // variance-guided
            float wk = exp(-float(i * i + j * j) * 0.25);
            float w = wn * wp * wl * wk;
            sum += ir2 * w;
            wsum += w;
        }
    }
    vec3 irr_out = (wsum > 1e-5) ? (sum / wsum) : center_irr;

    vec3 final = albedo.rgb * (1.0 - metallic) * irr_out + rest.rgb;
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], coord, vec4(final, rest.a));
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
#include "velk.glsl"
#include "velk-ui.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 1, rgba8) uniform writeonly image2D gStorageImages[];

// Scene TLAS bound by index (set = 1); same buffers as the deferred
// pass. Replaces the RtRoot BDA fields bvh_nodes / bvh_shapes for the
// array reads (bvh_root / bvh_node_count scalars still come from RtRoot).
layout(set = 1, binding = 0, std430) readonly buffer VelkBvhNodes { BvhNode data[]; } velk_bvh_nodes;
layout(set = 1, binding = 1, std430) readonly buffer VelkBvhShapes { RtShape data[]; } velk_bvh_shapes;
// BVH bases (and the rest of the per-view frame state: inv-VP, cam, bvh root
// / counts, present_counter) come from the bound FrameGlobals record, indexed
// by pc.globals_base — see VELK_GLOBALS below.
#define VELK_NODE_BASE VELK_GLOBALS.bvh_node_base
#define VELK_SHAPE_BASE VELK_GLOBALS.bvh_shape_base

// Scene light. Mirrors the C++ GpuLight struct (80 bytes).
struct Light {
    uvec4 flags;           // x = type (0=dir, 1=point, 2=spot), y = shadow_tech_id, zw = _
    vec4  position;        // xyz = world position (point / spot)
    vec4  direction;       // xyz = world forward axis (directional / spot)
    vec4  color_intensity; // rgb = colour, a = intensity multiplier
    vec4  params;          // x = range, y = cos(inner), z = cos(outer), w = light size (dir: angular radius rad; point/spot: world radius)
};

// Scene lights bound by index (set = 1 slot 5); this view's run starts at
// pc.lights_base. Same buffer the deferred-lighting compute reads.
layout(set = 1, binding = 5, std430) readonly buffer VelkLights { Light data[]; } velk_lights;

// Primary-ray painter-sorted RtShape list bound by index (set = 1 slot 6);
// this view's run starts at pc.shapes_base. Separate from velk_bvh_shapes
// (slot 1): the BVH holds build-order shapes for bounce/shadow traversal,
// this holds the back-to-front sort the primary loop composites.
layout(set = 1, binding = 6, std430) readonly buffer VelkShapes { RtShape data[]; } velk_shapes;

// Mesh-shape transforms (set = 1 slot 10); read via velk_mesh_instance(shape),
// which resolves shape.mesh_instance_base. Same buffer the deferred pass reads.
layout(set = 1, binding = 10, std430) readonly buffer VelkMeshInstances { MeshInstanceData data[]; } velk_mesh_instances;

// Per-primitive RT geometry metadata (set = 1 slot 11), read via
// velk_mesh_static(inst). A primitive's record is allocated once at load and
// never moves. The BLAS arenas (slots 12 / 13) are not declared here: this
// shader's mesh intersector is a linear triangle scan, and only the deferred
// pass walks the acceleration structure.
layout(set = 1, binding = 11, std430) readonly buffer VelkMeshStatic { MeshStaticData data[]; } velk_mesh_static_records;

// Material records as raw words (set = 1 slot 4). The raster path binds this
// same slot as a typed block, which one pipeline can do because it compiles
// for a single material type; this shader composes many, so it reads the
// bytes and each material's generated velk_unpack_<T> rebuilds its own struct
// from them. Materials whose layout cannot be derived stay on device
// addresses and never touch this buffer.
layout(set = 1, binding = 4, std430) readonly buffer VelkMaterialWords { uint data[]; } velk_material_words;

// This shader composes many materials, so slot 4 is bound above as raw words
// and must not be re-declared as a typed block: VELK_MATERIAL(T) becomes a
// no-op, and the composer replaces each material's VELK_MATERIAL(T) line with
// its generated velk_unpack_<T>. The load pastes the type name onto that
// function, so every snippet's `VELK_LOAD_MATERIAL(T, ctx)` resolves to its
// own reader. A material whose record cannot be modelled gets no reader and
// fails to compile here, rather than silently reading the wrong bytes.
#undef VELK_MATERIAL
#undef VELK_LOAD_MATERIAL
#define VELK_MATERIAL(T)
#define VELK_LOAD_MATERIAL(T, ctx) velk_unpack_##T((ctx).material_base)

// RT root: per-dispatch state pushed inline as a push constant (no device
// address anywhere). scalar layout matches the C++ `RtRoot` struct.
//
// The camera matrices, BVH root/counts/bases and present_counter live in
// the bound FrameGlobals record (indexed by globals_base), not here. Only
// the RT-specific per-dispatch state remains, all indices / inline data.
// Baked into the cached secondary at record time like the deferred PC;
// globals_base rotates with the globals ring but the cached pass re-records
// on view change, and static globals match across ring slots.
layout(push_constant, scalar) uniform PC {
    uint shapes_base;           // index into velk_shapes (set = 1 slot 6)
    uint globals_base;          // FrameGlobals index (set = 1 slot 2)
    uint light_count;
    uint lights_base;           // index into velk_lights (set = 1 slot 5)
    vec2 env_params;            // x = intensity, y = rotation_rad (inline)
    uvec4 extras;               // x=image_index, y=width, z=height, w=shape_count
    uvec4 env;                  // x=env_material_id, y=env_texture_id, zw=_
} pc;
// This view's FrameGlobals, read by index from the bound globals buffer
// (set = 1 slot 2, declared in velk.glsl) instead of a device address.
#define VELK_GLOBALS velk_globals.data[pc.globals_base]

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

// Forward decl: ray_aabb is defined further down with the BVH walker.
bool ray_aabb(Ray ray, vec3 bmin, vec3 bmax, float t_max, out float t_hit);

// Triangle-mesh intersector for the RT path (shape_kind == 255). Same
// math as the deferred_lighting compute's copy near the top of this
// file; duplicated here because the RT prelude is a separate string
// fed to a different compute pipeline. When a second consumer arrives
// we'll factor the body out into a shared GLSL snippet.
bool intersect_mesh(Ray ray, RtShape shape, out RayHit hit)
{
    {
        float t_aabb;
        if (!ray_aabb(ray, shape.origin.xyz, shape.u_axis.xyz, 1e30, t_aabb)) return false;
    }
)" R"(
    MeshInstanceData inst = velk_mesh_instance(shape);
    if (inst.mesh_static_base == VELK_INVALID_MESH_STATIC) return false;
    MeshStaticData st = velk_mesh_static(inst);
    if (st.triangle_count == 0u || st.vertex_stride == 0u) return false;

    vec3 lo = (inst.inv_world * vec4(ray.origin, 1.0)).xyz;
    vec3 ld = (inst.inv_world * vec4(ray.dir,    0.0)).xyz;

    uint floats_per_vert = st.vertex_stride >> 2u;

    bool  found = false;
    float best_t = 1e30;
    float best_u = 0.0;
    float best_v = 0.0;
    uint  best_o0 = 0u;
    uint  best_o1 = 0u;
    uint  best_o2 = 0u;

    for (uint t = 0u; t < st.triangle_count; ++t) {
        uint i0 = velk_mesh_index(st, t * 3u + 0u);
        uint i1 = velk_mesh_index(st, t * 3u + 1u);
        uint i2 = velk_mesh_index(st, t * 3u + 2u);
        uint o0 = i0 * floats_per_vert;
        uint o1 = i1 * floats_per_vert;
        uint o2 = i2 * floats_per_vert;
        vec3 v0 = vec3(velk_mesh_vertex(st, o0), velk_mesh_vertex(st, o0 + 1u), velk_mesh_vertex(st, o0 + 2u));
        vec3 v1 = vec3(velk_mesh_vertex(st, o1), velk_mesh_vertex(st, o1 + 1u), velk_mesh_vertex(st, o1 + 2u));
        vec3 v2 = vec3(velk_mesh_vertex(st, o2), velk_mesh_vertex(st, o2 + 1u), velk_mesh_vertex(st, o2 + 2u));
        vec3 e1 = v1 - v0;
        vec3 e2 = v2 - v0;
        vec3 p  = cross(ld, e2);
        float det = dot(e1, p);
        if (abs(det) < 1e-7) continue;
        float inv_det = 1.0 / det;
        vec3 to_v0 = lo - v0;
        float u = dot(to_v0, p) * inv_det;
        if (u < 0.0 || u > 1.0) continue;
        vec3 q = cross(to_v0, e1);
        float v = dot(ld, q) * inv_det;
        if (v < 0.0 || u + v > 1.0) continue;
        float tt = dot(e2, q) * inv_det;
        if (tt < 1e-4 || tt >= best_t) continue;
        best_t = tt;
        best_u = u;
        best_v = v;
        best_o0 = o0;
        best_o1 = o1;
        best_o2 = o2;
        found = true;
    }
    if (!found) return false;

    // Interpolate per-vertex normal and UV from the closest hit's
    // barycentrics. Vertex layout (VelkVertex3D, 32 B): pos[0..2],
    // normal[3..5], uv[6..7]. Möller-Trumbore's (u, v) make
    // (1-u-v, u, v) the weights for (V0, V1, V2).
    float w = 1.0 - best_u - best_v;
    vec3 n0 = vec3(velk_mesh_vertex(st, best_o0 + 3u), velk_mesh_vertex(st, best_o0 + 4u), velk_mesh_vertex(st, best_o0 + 5u));
    vec3 n1 = vec3(velk_mesh_vertex(st, best_o1 + 3u), velk_mesh_vertex(st, best_o1 + 4u), velk_mesh_vertex(st, best_o1 + 5u));
    vec3 n2 = vec3(velk_mesh_vertex(st, best_o2 + 3u), velk_mesh_vertex(st, best_o2 + 4u), velk_mesh_vertex(st, best_o2 + 5u));
    vec3 best_n = normalize(n0 * w + n1 * best_u + n2 * best_v);
    vec2 uv0 = vec2(velk_mesh_vertex(st, best_o0 + 6u), velk_mesh_vertex(st, best_o0 + 7u));
    vec2 uv1 = vec2(velk_mesh_vertex(st, best_o1 + 6u), velk_mesh_vertex(st, best_o1 + 7u));
    vec2 uv2 = vec2(velk_mesh_vertex(st, best_o2 + 6u), velk_mesh_vertex(st, best_o2 + 7u));
    vec2 best_uv = uv0 * w + uv1 * best_u + uv2 * best_v;

    vec3 hit_local = lo + ld * best_t;
    vec3 hit_world = (inst.world * vec4(hit_local, 1.0)).xyz;
    hit.t      = length(hit_world - ray.origin);
    hit.uv     = best_uv;
    hit.normal = normalize(mat3(inst.world) * best_n);
    return true;
}
)" R"(
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
    if (VELK_GLOBALS.bvh_node_count == 0u) return false;
    uint stack[kBvhStackSize];
    int sp = 0;
    stack[sp++] = VELK_GLOBALS.bvh_root;
    while (sp > 0) {
        uint ni = stack[--sp];
        BvhNode node = velk_bvh_nodes.data[VELK_NODE_BASE +ni];
        float t_enter;
        if (!ray_aabb(ray, node.aabb_min.xyz, node.aabb_max.xyz, hit.t, t_enter)) continue;
        for (uint i = 0u; i < node.shape_count; ++i) {
            uint idx = node.first_shape + i;
            RtShape s = velk_bvh_shapes.data[VELK_SHAPE_BASE +idx];
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
    if (VELK_GLOBALS.bvh_node_count == 0u) return false;
    uint stack[kBvhStackSize];
    int sp = 0;
    stack[sp++] = VELK_GLOBALS.bvh_root;
    while (sp > 0) {
        uint ni = stack[--sp];
        BvhNode node = velk_bvh_nodes.data[VELK_NODE_BASE +ni];
        float t_enter;
        if (!ray_aabb(ray, node.aabb_min.xyz, node.aabb_max.xyz, t_max, t_enter)) continue;
        for (uint i = 0u; i < node.shape_count; ++i) {
            RtShape s = velk_bvh_shapes.data[VELK_SHAPE_BASE +node.first_shape + i];
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
// Mirrors EnvMaterial's equirect sampling; env params (x = intensity,
// y = rotation_rad) ride the push constant inline (pc.env_params).
vec3 env_miss_color(vec3 rd) {
    if (pc.env.x == 0u) return vec3(0.0);
    const float PI = 3.14159265358979323846;
    float c = cos(pc.env_params.y);
    float s = sin(pc.env_params.y);
    vec3 dir = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
    float u = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;
    float v = asin(clamp(dir.y, -1.0, 1.0)) / PI + 0.5;
    return velk_texture(pc.env.y, vec2(u, v)).rgb * pc.env_params.x;
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
        Light light = velk_lights.data[pc.lights_base + li];
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
        RtShape s = velk_bvh_shapes.data[VELK_SHAPE_BASE +hit.shape_index];
        EvalContext ctx;
        ctx.material_base = s.material_base;
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
        ctx.tangent = vec4(0.0); // RT has no tangent basis (no normal mapping)
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

    rng_init(uvec2(coord), VELK_GLOBALS.present_counter);

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
        vec4 near_h = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 0.0, 1.0);
        vec4 far_h  = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 1.0, 1.0);
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
            RtShape s = velk_shapes.data[pc.shapes_base + i];
            RayHit hit;
            if (!intersect_shape(primary, s, hit)) continue;

            EvalContext ctx;
            ctx.material_base = s.material_base;
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
            ctx.tangent = vec4(0.0); // RT has no tangent basis (no normal mapping)
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

/**
 * @brief Composes the full RT compute pipeline source from registered
 *        material / shadow / intersect snippets.
 *
 * RT-side equivalent of compose_eval_fragment. Unlike the raster driver
 * templates (which substitute a single material's eval function name
 * via `<%EVAL_FN%>`), the RT pipeline dispatches to every active
 * material in a switch — so the composer is a procedural builder rather
 * than a placeholder template. Sections, in order:
 *
 *   1. rt_compute_prelude_src                (shared prelude)
 *   2. material #include lines               (one per active material)
 *   3. shadow tech #include lines
 *   4. intersect #include lines
 *   5. velk_eval_shadow switch               (calls registered shadow snippets)
 *   6. rt_pbr_shade_src                      (uses velk_eval_shadow)
 *   7. velk_resolve_fill switch              (calls eval_fn, may call velk_pbr_shade)
 *   8. intersect_shape switch                (built-in kinds + registered snippets)
 *   9. rt_compute_main_src                   (primary loop + bounce logic)
 */
inline string compose_rt_compute(const IFrameSnippetRegistry& snippets)
{
    const auto& material_ids        = snippets.frame_materials();
    const auto& shadow_tech_ids     = snippets.frame_shadow_techs();
    const auto& intersect_ids       = snippets.frame_intersects();
    const auto& material_info       = snippets.material_info_by_id();
    const auto& shadow_tech_info    = snippets.shadow_tech_info_by_id();
    const auto& intersect_info      = snippets.intersect_info_by_id();

    string src;
    src += rt_compute_prelude_src;
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size()) continue;
        const auto& mi = material_info[id - 1];
        src += string_view("#include \"", 10);
        src += mi.include_name;
        src += string_view("\"\n", 2);
    }
    for (auto id : shadow_tech_ids) {
        if (id == 0 || id > shadow_tech_info.size()) continue;
        const auto& ti = shadow_tech_info[id - 1];
        src += string_view("#include \"", 10);
        src += ti.include_name;
        src += string_view("\"\n", 2);
    }
    for (auto id : intersect_ids) {
        if (id < 3 || id - 3 >= intersect_info.size()) continue;
        const auto& ii = intersect_info[id - 3];
        src += string_view("#include \"", 10);
        src += ii.include_name;
        src += string_view("\"\n", 2);
    }

    auto append_literal = [&src](const char* s) {
        src += string_view(s, std::strlen(s));
    };

    char buf[128];

    // velk_eval_shadow: dispatch by shadow_tech id.
    append_literal("float velk_eval_shadow(uint tech_id, uint light_idx, vec3 world_pos, vec3 world_normal) {\n");
    append_literal("    switch (tech_id) {\n");
    for (auto id : shadow_tech_ids) {
        if (id == 0 || id > shadow_tech_info.size()) continue;
        const auto& ti = shadow_tech_info[id - 1];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += ti.fn_name;
        append_literal("(light_idx, world_pos, world_normal);\n");
    }
    append_literal("        default: return 1.0;\n");
    append_literal("    }\n");
    append_literal("}\n");

    // Shared PBR shading helper — defined after velk_eval_shadow (which
    // it calls) and before velk_resolve_fill (which calls it for Lit
    // materials).
    src += rt_pbr_shade_src;

    // velk_resolve_fill: dispatch by material id; route Lit through
    // velk_pbr_shade and Unlit straight to emission.
    append_literal("BrdfSample velk_resolve_fill(uint mid, EvalContext ctx) {\n");
    append_literal("    switch (mid) {\n");
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size()) continue;
        const auto& mi = material_info[id - 1];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: { MaterialEval e = ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += mi.fn_name;
        append_literal("(ctx);"
                       " if (e.lighting_mode == VELK_LIGHTING_STANDARD)"
                       " return velk_pbr_shade(e, ctx);"
                       " BrdfSample bs;"
                       " bs.emission = e.color;"
                       " bs.throughput = vec3(0.0);"
                       " bs.next_dir = vec3(0.0);"
                       " bs.terminate = true;"
                       " bs.sample_count_hint = 1u;"
                       " return bs; }\n");
    }
    append_literal("        default: { BrdfSample bs; bs.emission = ctx.base; bs.throughput = vec3(0.0); bs.next_dir = vec3(0.0); bs.terminate = true; bs.sample_count_hint = 1u; return bs; }\n");
    append_literal("    }\n");
    append_literal("}\n");

    // intersect_shape: built-in rect/cube/sphere/mesh kinds + registered
    // visual-contributed kinds (3+).
    append_literal("bool intersect_shape(Ray ray, RtShape shape, out RayHit hit) {\n");
    append_literal("    switch (shape.shape_kind) {\n");
    append_literal("        case 1u: return intersect_cube(ray, shape, hit);\n");
    append_literal("        case 2u: return intersect_sphere(ray, shape, hit);\n");
    append_literal("        case 255u: return intersect_mesh(ray, shape, hit);\n");
    for (auto id : intersect_ids) {
        if (id < 3 || id - 3 >= intersect_info.size()) continue;
        const auto& ii = intersect_info[id - 3];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += ii.fn_name;
        append_literal("(ray, shape, hit);\n");
    }
    append_literal("        default: return intersect_rect(ray, shape, hit);\n");
    append_literal("    }\n");
    append_literal("}\n");

    src += rt_compute_main_src;
    return src;
}

} // namespace velk

#endif // VELK_RENDER_DEFAULT_SHADERS_H
