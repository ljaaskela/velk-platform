#ifndef VELK_RENDER_DEFAULT_SHADERS_H
#define VELK_RENDER_DEFAULT_SHADERS_H

#include <velk/string.h>
#include <velk/string_view.h>

#include <velk-render/interface/intf_frame_snippet_registry.h>
#include <velk-render/frame/light_pool.h>
#include <velk-render/frame/raster_shaders.h>
#include <velk-render/frame/ray_walk.h>

#include <cstdio>
#include <cstring>

namespace velk {

// Raster default shaders + driver templates + compose_eval_fragment
// live in <velk-render/frame/raster_shaders.h>. This file holds the
// scene-side shader bits: the velk-ui GLSL include, the deferred-lighting
// compute shader, and the RT compute prelude/main.

// Rays and the built-in shape intersects, shared by every compute shader that
// traces. Registered as the "velk_intersect.glsl" include. The including shader
// declares RtShape through velk.glsl.
[[maybe_unused]] constexpr string_view velk_intersect_src = R"(
struct Ray {
    vec3 origin;
    vec3 dir;
};

struct RayHit {
    float t;
    vec2 uv;
    vec3 normal;
    uint object;      // the object id of what was hit, only set by trace_closest_hit
    // Where on its object the hit is, and which of its axes the surface faces, as the
    // G-buffer's surface attachment has them. Only set by trace_closest_hit.
    vec3 local;
    uint side;
    // What was hit, as the visibility buffer names a pixel, for velk_hit_context: its draw,
    // its instance's index, its cluster (VELK_NO_CLUSTER for none), the vertex indices of its
    // triangle's corners, and the weights of the second and third. corners.x is
    // VELK_NO_CORNERS where an analytic shape was hit, whose uv and normal are the surface's.
    // Only set by trace_closest_hit.
    uint draw;
    uint instance;
    uint cluster;
    uvec3 corners;
    vec2 weights;
    // The kept cut record that was hit (VELK_NO_RECORD for none), as velk_ids_record names a
    // pixel's. Only set by trace_closest_hit, and not kept by velk_hit_words.
    uint record;
};

#define VELK_NO_CORNERS 0xFFFFFFFFu

// What a hit was, as 7 words for a pass after the trace (velk_hit_context): its draw, its
// instance, its cluster and its corners, then the corners' weights, or for an analytic shape
// its uv, packed. The draw is first, so that a word for a miss can stand in its place.
void velk_hit_words(RayHit hit, out uvec4 a, out uvec3 b)
{
    a = uvec4(hit.draw, hit.instance, hit.cluster, hit.corners.x);
    b = uvec3(hit.corners.y, hit.corners.z,
              hit.corners.x == VELK_NO_CORNERS ? packHalf2x16(hit.uv) : packUnorm2x16(hit.weights));
}

RayHit velk_hit_from_words(uvec4 a, uvec3 b)
{
    RayHit hit;
    hit.t = 0.0;
    hit.normal = vec3(0.0);
    hit.object = 0u;
    hit.local = vec3(0.0);
    hit.side = 0u;
    hit.draw = a.x;
    hit.instance = a.y;
    hit.cluster = a.z;
    hit.corners = uvec3(a.w, b.x, b.y);
    hit.uv = a.w == VELK_NO_CORNERS ? unpackHalf2x16(b.z) : vec2(0.0);
    hit.weights = a.w == VELK_NO_CORNERS ? vec2(0.0) : unpackUnorm2x16(b.z);
    hit.record = VELK_NO_RECORD;
    return hit;
}

// Ray vs. oriented rect parameterised by (origin + s*u_axis + t*v_axis) for
// s,t in [0,1]. |u_axis|, |v_axis| carry the extents.
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

    // Corner radius test, in the same units as the axes.
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

// Ray vs. oriented box. Origin is one corner; u/v/w are three edges, whose length
// carries the extent. Uses a slab test in the box's frame. Normal is the face
// normal of whichever slab bounded the intersection. UV is the hit point projected
// into the 2D coordinates of the hit face.
bool intersect_cube(Ray ray, RtShape shape, out RayHit hit)
{
    vec3 U = shape.u_axis.xyz;
    vec3 V = shape.v_axis.xyz;
    vec3 W = shape.w_axis.xyz;
    float u_len2 = dot(U, U);
    float v_len2 = dot(V, V);
    float w_len2 = dot(W, W);
    if (u_len2 < 1e-12 || v_len2 < 1e-12 || w_len2 < 1e-12) return false;

    // The ray in the box's frame: corner at 0, axes aligned to U/V/W.
    vec3 rel = ray.origin - shape.origin.xyz;
    vec3 ro_l = vec3(dot(rel, U) / u_len2,
                     dot(rel, V) / v_len2,
                     dot(rel, W) / w_len2);
    vec3 rd_l = vec3(dot(ray.dir, U) / u_len2,
                     dot(ray.dir, V) / v_len2,
                     dot(ray.dir, W) / w_len2);

    // Slab test against [0, 1]^3, since U/V/W are full extents.
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

    // Which axis's slab bounded tmin (or tmax if we're inside).
    vec3 chosen = tmin > 0.0 ? tmin_v : tmax_v;
    float chosen_t = tmin > 0.0 ? tmin : tmax;
    // Outward, whichever side the ray is on: against the ray going in, along it coming out.
    float going = tmin > 0.0 ? -1.0 : 1.0;
    vec3 n_l;
    vec2 face_uv;
    vec3 p_l = ro_l + chosen_t * rd_l;
    if (chosen.x == chosen_t) {
        n_l = vec3(rd_l.x > 0.0 ? going : -going, 0.0, 0.0);
        face_uv = vec2(p_l.y, p_l.z);
    } else if (chosen.y == chosen_t) {
        n_l = vec3(0.0, rd_l.y > 0.0 ? going : -going, 0.0);
        face_uv = vec2(p_l.x, p_l.z);
    } else {
        n_l = vec3(0.0, 0.0, rd_l.z > 0.0 ? going : -going);
        face_uv = vec2(p_l.x, p_l.y);
    }

    vec3 n = normalize(n_l.x * U + n_l.y * V + n_l.z * W);

    hit.t = t;
    hit.uv = clamp(face_uv, 0.0, 1.0);
    hit.normal = n;
    return true;
}

// Ray vs. sphere, centered at origin + (u + v + w) * 0.5 with radius params.x.
bool intersect_sphere(Ray ray, RtShape shape, out RayHit hit)
{
    vec3 center = shape.origin.xyz
                + 0.5 * (shape.u_axis.xyz + shape.v_axis.xyz + shape.w_axis.xyz);
    float radius = shape.params.x;
    if (radius <= 0.0) return false;

    // Solved from the ray's closest point to the center, so a distant origin does
    // not cost precision.
    vec3 oc = ray.origin - center;
    float a = dot(ray.dir, ray.dir);
    float t_closest = -dot(oc, ray.dir) / a;
    vec3 closest = oc + t_closest * ray.dir;
    float h2 = (radius * radius - dot(closest, closest)) / a;
    if (h2 < 0.0) return false;
    float h = sqrt(h2);
    float t1 = t_closest - h;
    float t2 = t_closest + h;
    float t = t1 > 0.0 ? t1 : t2;
    if (t <= 0.0) return false;

    vec3 n = normalize(closest + (t - t_closest) * ray.dir);

    // Spherical equirect-style UV.
    float u = atan(n.z, n.x) / (2.0 * 3.14159265) + 0.5;
    float v = asin(clamp(n.y, -1.0, 1.0)) / 3.14159265 + 0.5;

    hit.t = t;
    hit.uv = vec2(u, v);
    hit.normal = n;
    return true;
}
)";

/**
 * Fills the G-buffer from the visibility buffer: for each pixel, finds its triangle, where
 * the view ray meets it, and what the material gives there. Goes after compose_rt_compute's
 * shared body, whose push constants it borrows.
 */
inline constexpr string_view visibility_resolve_main_src = R"(
layout(set = 0, binding = 0) uniform usampler2D velk_textures_uint[];

#define RESOLVE_IDS pc.extras.x
#define RESOLVE_WIDTH pc.extras.y
#define RESOLVE_HEIGHT pc.extras.z
#define RESOLVE_ALBEDO pc.extras.w
#define RESOLVE_NORMAL pc.env.x
#define RESOLVE_POSITION pc.env.y
#define RESOLVE_MATERIAL pc.env.z
#define RESOLVE_EMISSIVE pc.env.w
// The resolve has no lights, so their field carries one more attachment.
#define RESOLVE_SURFACE pc.lights_base
// And their count the word of the depth plane that compute raster drew to, a word a pixel
// (instance_tiny_draw_src). After it come the pixels' ids, two words a pixel
// (velk_pixel_ids): compute raster's where it drew, the hardware's written here elsewhere.
#define RESOLVE_TINY pc.light_count

layout(set = 1, binding = 16, std430) buffer ResolveWords { uint data[]; } resolve_words;

// Where the view ray through a point of the screen, in pixels, meets a triangle's plane. A
// ray rather than dividing by each corner's w, which fails for corners behind the camera. It
// starts on the near plane so that it also suits orthographic views. Leaves the results
// alone for a ray along the plane.
void resolve_hit(GlobalData g, vec2 pixel, vec3 p0, vec3 e1, vec3 e2, inout vec3 weights, inout vec3 point)
{
    vec2 ndc = pixel / vec2(float(RESOLVE_WIDTH), float(RESOLVE_HEIGHT)) * 2.0 - 1.0;
    vec4 near_point = g.inverse_view_projection * vec4(ndc, 1.0, 1.0);
    vec4 far_point = g.inverse_view_projection * vec4(ndc, 1e-4, 1.0);
    vec3 origin = near_point.xyz / near_point.w;
    vec3 dir = normalize(far_point.xyz / far_point.w - origin);

    vec3 h = cross(dir, e2);
    float det = dot(e1, h);
    if (abs(det) > 0.0) {
        vec3 s = origin - p0;
        vec3 q = cross(s, e1);
        float b1 = dot(s, h) / det;
        float b2 = dot(dir, q) / det;
        weights = vec3(1.0 - b1 - b2, b1, b2);
        // Along the ray, so that imprecision in a large triangle's weights only moves
        // the point towards or away from the camera.
        point = origin + dir * (dot(e2, q) / det);
    }
}

vec3 resolve_shading_normal(MaterialEval e, EvalContext ctx)
{
    return normalize(length(e.normal) > 0.0 ? e.normal : ctx.normal);
}

// Which of the object's axes `facing`, in the object's own axes, is mostly along: 0 to 5,
// positive x first.
float resolve_side(vec3 facing)
{
    vec3 across = abs(facing);
    return across.x >= across.y && across.x >= across.z ? (facing.x < 0.0 ? 1.0 : 0.0)
         : across.y >= across.z ? (facing.y < 0.0 ? 3.0 : 2.0)
         : (facing.z < 0.0 ? 5.0 : 4.0);
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(RESOLVE_WIDTH) || coord.y >= int(RESOLVE_HEIGHT)) return;

    // Compute raster drew here, nearer than the hardware, and left its ids. Its depth is
    // cleared for the next frame.
    uint pixel_index = uint(coord.y) * RESOLVE_WIDTH + uint(coord.x);
    uint ids_at = RESOLVE_TINY + RESOLVE_WIDTH * RESOLVE_HEIGHT + pixel_index * 2u;
    uvec2 ids;
    if (resolve_words.data[RESOLVE_TINY + pixel_index] != 0u) {
        resolve_words.data[RESOLVE_TINY + pixel_index] = 0u;
        ids = uvec2(resolve_words.data[ids_at], resolve_words.data[ids_at + 1u]);
    } else {
        ids = texelFetch(velk_textures_uint[nonuniformEXT(RESOLVE_IDS)], coord, 0).xy;
        resolve_words.data[ids_at] = ids.x;
        resolve_words.data[ids_at + 1u] = ids.y;
    }
    if (ids.x == 0u) {
        imageStore(gStorageImages[nonuniformEXT(RESOLVE_ALBEDO)], coord, vec4(0.0));
        imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_NORMAL)], coord, vec4(0.0));
        imageStore(gStorageImagesF32[nonuniformEXT(RESOLVE_POSITION)], coord, vec4(0.0));
        imageStore(gStorageImages[nonuniformEXT(RESOLVE_MATERIAL)], coord, vec4(0.0));
        imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_EMISSIVE)], coord, vec4(0.0));
        imageStore(gStorageImagesF32[nonuniformEXT(RESOLVE_SURFACE)], coord, vec4(0.0));
        return;
    }

    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint triangle = velk_ids_triangle(d, ids);
    ElementInstance inst = velk_draw_instance(d, velk_ids_slot(d, ids));
    GlobalData g = velk_globals.data[d.globals_base];

    uint cluster = velk_ids_cluster(d, ids);
    uvec3 corners = velk_surface_corners(d, cluster, triangle);
    VelkSurfaceVertex v0 = velk_surface_vertex(d, cluster, inst, g, corners.x);
    VelkSurfaceVertex v1 = velk_surface_vertex(d, cluster, inst, g, corners.y);
    VelkSurfaceVertex v2 = velk_surface_vertex(d, cluster, inst, g, corners.z);

    vec3 e1 = v1.rel - v0.rel;
    vec3 e2 = v2.rel - v0.rel;
    vec2 pixel = vec2(coord) + 0.5;
    vec3 weights = vec3(1.0 / 3.0);
    vec3 rel = (v0.rel + v1.rel + v2.rel) / 3.0;
    resolve_hit(g, pixel, v0.rel, e1, e2, weights, rel);

    // The next pixels along, on this triangle's plane even where another covers them.
    vec3 weights_x = weights;
    vec3 weights_y = weights;
    vec3 rel_x = rel;
    vec3 rel_y = rel;
    resolve_hit(g, pixel + vec2(1.0, 0.0), v0.rel, e1, e2, weights_x, rel_x);
    resolve_hit(g, pixel + vec2(0.0, 1.0), v0.rel, e1, e2, weights_y, rel_y);
    mat3x2 uvs = mat3x2(v0.uv, v1.uv, v2.uv);
    mat3x2 uv1s = mat3x2(v0.uv1, v1.uv1, v2.uv1);
    velk_uv_dx[0] = uvs * (weights_x - weights);
    velk_uv_dy[0] = uvs * (weights_y - weights);
    velk_uv_dx[1] = uv1s * (weights_x - weights);
    velk_uv_dy[1] = uv1s * (weights_y - weights);

    EvalContext ctx = velk_surface_context(d, g, v0, v1, v2, weights, rel);
    velk_uv_set = 0u;
    MaterialEval e = velk_eval_material(d.material_id, ctx);
    vec3 N = resolve_shading_normal(e, ctx);

    // Widens roughness where the shading normal changes quickly between pixels, so that a
    // tight highlight does not alias (Tokuyoshi 2019). The next pixels are taken on this
    // triangle, so an edge between surfaces does not count.
    EvalContext ctx_x = velk_surface_context(d, g, v0, v1, v2, weights_x, rel_x);
    velk_uv_set = 0u;
    vec3 dndx = resolve_shading_normal(velk_eval_material(d.material_id, ctx_x), ctx_x) - N;
    EvalContext ctx_y = velk_surface_context(d, g, v0, v1, v2, weights_y, rel_y);
    velk_uv_set = 0u;
    vec3 dndy = resolve_shading_normal(velk_eval_material(d.material_id, ctx_y), ctx_y) - N;
    float variance = 0.25 * (dot(dndx, dndx) + dot(dndy, dndy));
    float roughness = sqrt(clamp(e.roughness * e.roughness + min(2.0 * variance, 0.18), 0.0, 1.0));

    mat3 world_axes = mat3(inst.world_matrix);
    float lod_error = velk_cluster_error(cluster, max(max(length(world_axes[0]), length(world_axes[1])), length(world_axes[2])));

    imageStore(gStorageImages[nonuniformEXT(RESOLVE_ALBEDO)], coord, e.color);
    imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_NORMAL)], coord, vec4(N, lod_error));
    imageStore(gStorageImagesF32[nonuniformEXT(RESOLVE_POSITION)], coord, vec4(rel, 0.0));
    imageStore(gStorageImages[nonuniformEXT(RESOLVE_MATERIAL)], coord,
               vec4(e.metallic, roughness, float(e.lighting_mode) / 255.0, 0.0));
    imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_EMISSIVE)], coord, vec4(e.emissive, 0.0));

    // Where on the object this is, in the object's own axes but world units, and which of
    // those axes the surface faces. From the vertices as stored and the unmapped normal, so
    // neither a swaying material nor a normal map moves it.
    mat3 to_world = mat3(inst.world_matrix);
    vec3 axis_scale = vec3(length(to_world[0]), length(to_world[1]), length(to_world[2]));
    vec3 local = v0.local * weights.x + v1.local * weights.y + v2.local * weights.z;
    imageStore(gStorageImagesF32[nonuniformEXT(RESOLVE_SURFACE)], coord,
               vec4(local * axis_scale, resolve_side(transpose(to_world) * ctx.normal)));
}
)";

/// Draws the edges of the view of the camera that chooses what a view draws (ICamera::choose_as)
/// over that view's image: its four sides out to `reach` world units, and the rectangle
/// there. A thread per point along an edge, kChooserEdgeSteps of them per edge, 8 edges.
inline constexpr uint32_t kChooserEdgeSteps = 4096;
inline constexpr string_view chooser_frustum_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint output_id;
    uint width;
    uint height;
    uint view_globals;
    uint chooser_globals;
    float reach;
} pc;

const uint kSteps = 4096u;

// The direction through a corner of the chooser's view, in world axes.
vec3 corner_direction(GlobalData chooser, vec2 ndc)
{
    vec4 p = chooser.inverse_view_projection * vec4(ndc, 0.5, 1.0);
    return normalize(p.xyz / p.w);
}

void main()
{
    uint edge = gl_GlobalInvocationID.x / kSteps;
    if (edge >= 8u) return;
    float t = float(gl_GlobalInvocationID.x % kSteps) / float(kSteps - 1u);

    GlobalData view = velk_globals.data[pc.view_globals];
    GlobalData chooser = velk_globals.data[pc.chooser_globals];
    const vec2 corners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    // The chooser's position from this view's camera.
    vec3 eye = chooser.cam_pos.xyz - view.cam_pos.xyz;
    vec3 point;
    if (edge < 4u) {
        point = eye + corner_direction(chooser, corners[edge]) * (pc.reach * t);
    } else {
        vec3 a = eye + corner_direction(chooser, corners[edge - 4u]) * pc.reach;
        vec3 b = eye + corner_direction(chooser, corners[(edge - 3u) & 3u]) * pc.reach;
        point = mix(a, b, t);
    }
    vec4 clip = view.view_projection * vec4(point, 1.0);
    if (clip.w <= 1e-5) return;
    vec2 at = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(pc.width), float(pc.height));
    if (any(lessThan(at, vec2(0.0))) || at.x >= float(pc.width) || at.y >= float(pc.height)) return;
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], ivec2(at), vec4(1.0, 1.0, 0.0, 1.0));
}
)";

/// Draws the bounds of a visual's partitions over a view's image (DebugView::Partition): red for
/// a partition that chooses its levels of detail again this frame, cyan for one that keeps
/// them. `table_word` is the visual's partition table (kCullEntryWords a partition), `cut_word`
/// its kept cut in this view (kCutHeaderWords). A thread per point along an edge,
/// kPartitionEdgeSteps of them per edge, 12 edges a partition.
inline constexpr uint32_t kPartitionEdgeSteps = 256;
inline constexpr string_view partition_bounds_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint output_id;
    uint width;
    uint height;
    uint view_globals;
    uint table_word;
    uint partition_count;
    uint cut_word;
} pc;

const uint kSteps = 256u;

void main()
{
    uint index = gl_GlobalInvocationID.x;
    uint partition_index = index / (12u * kSteps);
    if (partition_index >= pc.partition_count) return;
    uint edge = (index / kSteps) % 12u;
    float t = float(index % kSteps) / float(kSteps - 1u);

    uint entry = pc.table_word + partition_index * VELK_CULL_ENTRY_WORDS;
    if (velk_culling.data[entry + 7u] == 0u) return;
    vec3 lo = vec3(uintBitsToFloat(velk_culling.data[entry]), uintBitsToFloat(velk_culling.data[entry + 1u]),
                   uintBitsToFloat(velk_culling.data[entry + 2u]));
    vec3 hi = vec3(uintBitsToFloat(velk_culling.data[entry + 3u]), uintBitsToFloat(velk_culling.data[entry + 4u]),
                   uintBitsToFloat(velk_culling.data[entry + 5u]));
    uint state = velk_culling.data[pc.cut_word + 22u] + partition_index * VELK_CUT_PARTITION_WORDS;
    bool choosing = velk_culling.data[state + 8u] != 0u;

    // Edges 0..3 along x, 4..7 along y, 8..11 along z, at the four corners of the other two.
    uint axis = edge / 4u;
    uint corner = edge % 4u;
    vec3 other = vec3((corner & 1u) != 0u ? 1.0 : 0.0, (corner & 2u) != 0u ? 1.0 : 0.0, 0.0);
    vec3 at;
    if (axis == 0u) at = vec3(t, other.x, other.y);
    else if (axis == 1u) at = vec3(other.x, t, other.y);
    else at = vec3(other.x, other.y, t);
    vec3 point = mix(lo, hi, at);

    GlobalData view = velk_globals.data[pc.view_globals];
    vec4 clip = view.view_projection * vec4(point - view.cam_pos.xyz, 1.0);
    if (clip.w <= 1e-5) return;
    vec2 pixel = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(pc.width), float(pc.height));
    if (any(lessThan(pixel, vec2(0.0))) || pixel.x >= float(pc.width) || pixel.y >= float(pc.height)) return;
    vec3 color = choosing ? vec3(1.0, 0.15, 0.1) : vec3(0.1, 0.8, 0.9);
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], ivec2(pixel), vec4(color, 1.0));
}
)";

/// Draws the boxes of one level of the trees over a kept cut's partitions (DebugView::RayTree)
/// over a view's image: at `level` 0 its leaves of four records, at higher ones its nodes',
/// and a partition's root where its tree is not that tall, coloured by level. `cut_word` is
/// the kept cut (kCutHeaderWords). With `top_word` set instead, the view's top tree over the
/// partitions (kRayTopHeaderWords, DebugView::RayTop): at 0 each partition's box. A thread per
/// point along an edge, kRayTreeEdgeSteps of them per edge, 12 edges a box, two boxes a node.
inline constexpr uint32_t kRayTreeEdgeSteps = 32;
inline constexpr string_view ray_tree_bounds_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint output_id;
    uint width;
    uint height;
    uint view_globals;
    uint cut_word;
    uint level;
    uint top_word;
} pc;

const uint kSteps = 32u;

void main()
{
    uint index = gl_GlobalInvocationID.x;
    uint node_index = index / (24u * kSteps);
    uint side = (index / (12u * kSteps)) % 2u;
    uint edge = (index / kSteps) % 12u;
    float t = float(index % kSteps) / float(kSteps - 1u);

    bool top = pc.top_word != 0xFFFFFFFFu;
    uint node;
    if (top) {
        uint cuts = velk_culling.data[pc.top_word];
        uint leaves = velk_culling.data[pc.top_word + 1u];
        if (node_index >= velk_culling.data[pc.top_word + 2u]) return;
        node = pc.top_word + 4u + cuts * 8u + leaves * 2u + node_index * 16u;
    } else {
        uint cut = pc.cut_word;
        if (node_index >= velk_culling.data[cut + 24u]) return;
        uint parity = velk_culling.data[cut + 12u];
        node = velk_culling.data[cut + 26u + parity] + node_index * 16u;
    }
    uint level = velk_culling.data[node + 14u];
    bool root = velk_culling.data[node + 15u] != 0u;
    // A node's children are a level below it; a short tree shows its root's.
    if (level != pc.level + 1u && !(root && level < pc.level + 1u)) return;
    if (velk_culling.data[node + 12u + side] == 0xFFFFFFFFu) return;
    uint box = node + side * 6u;
    vec3 lo = vec3(uintBitsToFloat(velk_culling.data[box]), uintBitsToFloat(velk_culling.data[box + 1u]),
                   uintBitsToFloat(velk_culling.data[box + 2u]));
    vec3 hi = vec3(uintBitsToFloat(velk_culling.data[box + 3u]), uintBitsToFloat(velk_culling.data[box + 4u]),
                   uintBitsToFloat(velk_culling.data[box + 5u]));
    if (any(greaterThan(lo, hi))) return;

    // Edges 0..3 along x, 4..7 along y, 8..11 along z, at the four corners of the other two.
    uint axis = edge / 4u;
    uint corner = edge % 4u;
    vec2 other = vec2((corner & 1u) != 0u ? 1.0 : 0.0, (corner & 2u) != 0u ? 1.0 : 0.0);
    vec3 at;
    if (axis == 0u) at = vec3(t, other.x, other.y);
    else if (axis == 1u) at = vec3(other.x, t, other.y);
    else at = vec3(other.x, other.y, t);
    vec3 point = mix(lo, hi, at);

    GlobalData view = velk_globals.data[pc.view_globals];
    vec4 clip = view.view_projection * vec4(point - view.cam_pos.xyz, 1.0);
    if (clip.w <= 1e-5) return;
    vec2 pixel = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(pc.width), float(pc.height));
    if (any(lessThan(pixel, vec2(0.0))) || pixel.x >= float(pc.width) || pixel.y >= float(pc.height)) return;
    // Hue by the level the box is at, so roots of short trees stand apart.
    float shown = float(level - 1u);
    vec3 color = 0.55 + 0.45 * cos(6.2831853 * (shown * 0.17 + vec3(0.0, 0.33, 0.67)));
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], ivec2(pixel), vec4(color, 1.0));
}
)";

/// Shows the visibility buffer as flat colours: mode 1 per draw, 2 per instance, 3 per
/// triangle. Mode 4 shows the depth pyramid's fourth level.
/// Modes 5 and 6 show what compute drew and how large triangles are on screen
/// (triangle_size_color). Modes 7 to 9 show level of detail clusters (cluster_color): one
/// colour per cluster of each instance, by level, and by width on screen as the camera that
/// chooses sees it (FrameGlobals at chooser_globals). Modes 10 to 12 show instances dimmed,
/// for partition_bounds_compute_src and ray_tree_bounds_compute_src to draw over. With `dim_outside`, clusters outside the
/// chooser's view are dimmed. The numbers are DebugView's.
inline constexpr string_view visibility_debug_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint ids_word;
    uint output_id;
    uint width;
    uint height;
    uint mode;
    uint pyramid_word;
    uint chooser_globals;
    uint dim_outside;
    uint pool_word;       // the view's light pool (light_pool.h), for mode 15
    float light_pixels;   // kLightPixels
} pc;

#define VELK_LIGHT_WORDS velk_culling
)" VELK_LIGHT_POOL_GLSL R"(
vec3 id_color(uint id)
{
    uint h = (id + 1u) * 2654435761u;
    h ^= h >> 15u;
    h *= 2246822519u;
    h ^= h >> 13u;
    return vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0;
}

// A colour per kept cut record by what it is (entry, cluster, slot), which stays from frame to
// frame while the record's place in the cut's stores does not.
vec3 record_color(uint record)
{
    return id_color(velk_culling.data[record + VELK_RECORD_ENTRY] * 2654435761u
                    ^ velk_culling.data[record + VELK_RECORD_CLUSTER] * 2246822519u
                    ^ velk_culling.data[record + VELK_RECORD_SLOT] * 3266489917u);
}

struct DebugInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};
layout(set = 1, binding = 3, scalar) readonly buffer DebugInstances { DebugInstance data[]; } debug_instances;

// Mode 5: a pixel compute drew magenta, the rest grey. Mode 6: compute's magenta, the rest by
// how large their triangle is on screen, the square root of its area in pixels: under a half
// red, under 1 yellow, under 2 green, under 8 cyan, and larger blue.
vec3 triangle_size_color(uvec2 ids)
{
    if ((ids.x & VELK_IDS_TINY) != 0u) return vec3(1.0, 0.0, 1.0);
    if (pc.mode == 5u) return vec3(0.3);
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    // A quad's slot is its word, not an instance this view's instances hold.
    if ((d.flags & VELK_DRAW_QUADS) != 0u) return vec3(0.25);
    uint triangle = velk_ids_triangle(d, ids);
    DebugInstance inst = debug_instances.data[d.instances_base + velk_ids_slot(d, ids)];
    GlobalData g = velk_globals.data[d.globals_base];
    uvec3 corners = uvec3(triangle) + uvec3(0u, 1u, 2u);
    uint cluster = velk_ids_cluster(d, ids);
    if (d.index_base != VELK_NO_VISIBLE_LIST) {
        uint word = d.index_base + velk_cluster_first_index(cluster) + triangle * 3u;
        corners = uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
    }
    vec2 p[3];
    for (int k = 0; k < 3; ++k) {
        uint at = velk_cluster_vertex_word(d, cluster) + corners[k] * VELK_VERTEX3D_WORDS;
        vec3 local = vec3(uintBitsToFloat(velk_mesh_words.data[at]), uintBitsToFloat(velk_mesh_words.data[at + 1u]),
                          uintBitsToFloat(velk_mesh_words.data[at + 2u]));
        vec4 clip = g.view_projection * vec4(velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g), 1.0);
        if (clip.w <= 0.0) return vec3(0.0, 0.0, 1.0);
        p[k] = clip.xy / clip.w * 0.5 * vec2(float(pc.width), float(pc.height));
    }
    vec2 e1 = p[1] - p[0];
    vec2 e2 = p[2] - p[0];
    float across = sqrt(0.5 * abs(e1.x * e2.y - e1.y * e2.x));
    return across < 0.5 ? vec3(1.0, 0.0, 0.0)
         : across < 1.0 ? vec3(1.0, 1.0, 0.0)
         : across < 2.0 ? vec3(0.0, 1.0, 0.0)
         : across < 8.0 ? vec3(0.0, 1.0, 1.0)
         : vec3(0.0, 0.0, 1.0);
}

// Mode 15: where light comes from. A record with points: a colour per point of light, blended as
// a pixel reads them (its triangle's corners, each its own point or the three it links to), so
// each patch is one point's reach, darker where they are the record's own than where they are
// those of the records for light only it links to; one with one value flat in its colour; a quad in the colour
// of the node its light is on; magenta as far as that light is not traced yet; what has no light
// yet grey, and what is from no record white.
vec3 light_color(uvec2 ids, ivec2 coord)
{
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint record = velk_ids_record(d, ids);
    if (record == VELK_NO_RECORD) return vec3(1.0);
    GlobalData g = velk_globals.data[d.globals_base];
    mat4 vp = g.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
    if (velk_culling.data[record + VELK_RECORD_ENTRY] == VELK_CUT_QUAD) {
        uint node = velk_light_quad_node(velk_culling.data[record + VELK_RECORD_QUAD_TREE],
                                         velk_culling.data[record + VELK_RECORD_QUAD_NODE], g.cam_pos.xyz, pixels_per_unit,
                                         pc.light_pixels);
        uint value = velk_culling.data[node + 11u];
        if (value == VELK_LIGHT_NONE) return vec3(0.25);
        return (value & VELK_LIGHT_ONE_TRACED) == 0u ? vec3(1.0, 0.0, 1.0) : id_color(node);
    }
    uint light = velk_culling.data[record + VELK_RECORD_LIGHT];
    if (light == VELK_LIGHT_NONE || light == VELK_LIGHT_PENDING) return vec3(0.25);
    if (light >= VELK_LIGHT_ONE_VALUE) {
        return (light & VELK_LIGHT_ONE_TRACED) == 0u ? vec3(1.0, 0.0, 1.0) : record_color(record) * 0.8;
    }
    if (pc.pool_word == 0xFFFFFFFFu || light >= velk_culling.data[pc.pool_word + VELK_POOL_PAGES]) return vec3(1.0, 0.0, 0.0);
    uint first = velk_light_page(pc.pool_word, light);
    vec3 base = record_color(record) * 0.35;

    // The pixel's triangle on screen, in pixels.
    uint triangle = velk_ids_triangle(d, ids);
    uint cluster = velk_ids_cluster(d, ids);
    if (cluster == VELK_NO_CLUSTER) return base;
    DebugInstance inst = debug_instances.data[d.instances_base + velk_ids_slot(d, ids)];
    uint word = d.index_base + velk_cluster_first_index(cluster) + triangle * 3u;
    uvec3 corners = uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
    vec3 local[3];
    vec2 p[3];
    vec2 size = vec2(float(pc.width), float(pc.height));
    for (int k = 0; k < 3; ++k) {
        uint at = velk_cluster_vertex_word(d, cluster) + corners[k] * VELK_VERTEX3D_WORDS;
        local[k] = vec3(uintBitsToFloat(velk_mesh_words.data[at]), uintBitsToFloat(velk_mesh_words.data[at + 1u]),
                        uintBitsToFloat(velk_mesh_words.data[at + 2u]));
        vec4 clip = vp * vec4(velk_rel_position(inst.world_matrix, inst.offset.xyz + local[k] * inst.size.xyz, g), 1.0);
        if (clip.w <= 0.0) return base;
        p[k] = (clip.xy / clip.w * 0.5 + 0.5) * size;
    }
    vec2 pixel = vec2(coord) + 0.5;
    // Where the pixel is in it, on screen: enough to find the points around it.
    vec2 e1 = p[1] - p[0];
    vec2 e2 = p[2] - p[0];
    float det = e1.x * e2.y - e1.y * e2.x;
    if (abs(det) < 1e-6) return base;
    vec2 q = pixel - p[0];
    float b1 = (q.x * e2.y - q.y * e2.x) / det;
    float b2 = (e1.x * q.y - e1.y * q.x) / det;
    vec3 b = clamp(vec3(1.0 - b1 - b2, b1, b2), 0.0, 1.0);
    vec3 color = vec3(0.0);
    uint words[9];
    uint points[9];
    float weights[9];
    uint owners[9];
    uint read = velk_light_pixel_points(pc.pool_word, first, triangle, corners, b / max(b.x + b.y + b.z, 1e-6), words, points,
                                        weights, owners);
    float untrusted = 0.0;
    float retracing = 0.0;
    for (uint k = 0u; k < read; ++k) {
        color += weights[k] * id_color(words[k] * 2654435761u);
        uint traces = velk_light_point_traces(owners[k], velk_culling.data[words[k] + 1u]);
        float trust = velk_culling.data[words[k]] == VELK_LIGHT_NONE
                    ? 0.0 : min(float(traces), VELK_LIGHT_TRUSTED_TRACES) / VELK_LIGHT_TRUSTED_TRACES;
        untrusted += weights[k] * (1.0 - trust);
        if (traces >= VELK_LIGHT_RETRACE && traces < VELK_LIGHT_RETRACED) retracing += weights[k];
    }
    // Darker where the record has its own points, coarse enough itself.
    color = velk_culling.data[first + VELK_PAGE_KIND] == VELK_LIGHT_LINKED ? color : color * 0.45;
    // Magenta as far as the points it reads are not fully traced yet (VELK_LIGHT_TRUSTED_TRACES),
    // cyan as far as they are traced again after a change.
    color = mix(color, vec3(0.0, 1.0, 1.0), clamp(retracing, 0.0, 1.0));
    return mix(color, vec3(1.0, 0.0, 1.0), clamp(untrusted, 0.0, 1.0));
}

const vec3 kLevelColors[8] = vec3[8](
    vec3(1.0, 0.0, 0.0), vec3(1.0, 0.5, 0.0), vec3(1.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0),
    vec3(0.0, 1.0, 1.0), vec3(0.0, 0.3, 1.0), vec3(0.6, 0.0, 1.0), vec3(1.0, 0.0, 0.7));

// Mode 7: a colour per cluster of each instance. Mode 8: by the cluster's level, full detail
// red, then orange, yellow, green, cyan, blue, purple, pink. Mode 9: by the width on screen of
// the sphere around the cluster's own triangles: under 8 pixels blue, under 16 cyan, under 32
// green, under 64 yellow, under 128 orange, larger red. What is not drawn by cluster is grey.
vec3 cluster_color(uvec2 ids)
{
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint cluster = velk_ids_cluster(d, ids);
    if (cluster == VELK_NO_CLUSTER) return vec3(0.25);
    uint slot = velk_ids_slot(d, ids);
    if (pc.mode == 7u) return id_color(cluster * 7919u + slot * 104729u);
    if (pc.mode == 8u) return kLevelColors[min(velk_culling.data[cluster + 2u], 7u)];

    DebugInstance inst = debug_instances.data[d.instances_base + slot];
    GlobalData g = velk_globals.data[pc.chooser_globals];
    vec3 local = vec3(uintBitsToFloat(velk_culling.data[cluster + 13u]), uintBitsToFloat(velk_culling.data[cluster + 14u]),
                      uintBitsToFloat(velk_culling.data[cluster + 15u]));
    mat3 to_world = mat3(inst.world_matrix);
    float scale = max(max(length(to_world[0] * inst.size.x), length(to_world[1] * inst.size.y)), length(to_world[2] * inst.size.z));
    float radius = uintBitsToFloat(velk_culling.data[cluster + 16u]) * scale;
    vec3 center = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g);
    mat4 vp = g.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
    float across = 2.0 * radius * pixels_per_unit / max(length(center) - radius, 1e-3);
    return across < 8.0 ? vec3(0.0, 0.0, 1.0)
         : across < 16.0 ? vec3(0.0, 1.0, 1.0)
         : across < 32.0 ? vec3(0.0, 1.0, 0.0)
         : across < 64.0 ? vec3(1.0, 1.0, 0.0)
         : across < 128.0 ? vec3(1.0, 0.5, 0.0)
         : vec3(1.0, 0.0, 0.0);
}

// Whether the cluster a pixel shows is outside the view of the camera that chose it.
bool outside_chooser(uvec2 ids)
{
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint cluster = velk_ids_cluster(d, ids);
    if (cluster == VELK_NO_CLUSTER) return false;
    DebugInstance inst = debug_instances.data[d.instances_base + velk_ids_slot(d, ids)];
    GlobalData g = velk_globals.data[pc.chooser_globals];
    vec3 local = vec3(uintBitsToFloat(velk_culling.data[cluster + 13u]), uintBitsToFloat(velk_culling.data[cluster + 14u]),
                      uintBitsToFloat(velk_culling.data[cluster + 15u]));
    mat3 to_world = mat3(inst.world_matrix);
    float scale = max(max(length(to_world[0] * inst.size.x), length(to_world[1] * inst.size.y)), length(to_world[2] * inst.size.z));
    float radius = uintBitsToFloat(velk_culling.data[cluster + 16u]) * scale;
    vec3 center = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g);
    mat4 vp = g.view_projection;
    vec4 clip = vp * vec4(center, 1.0);
    vec3 r0 = vec3(vp[0][0], vp[1][0], vp[2][0]);
    vec3 r1 = vec3(vp[0][1], vp[1][1], vp[2][1]);
    vec3 r2 = vec3(vp[0][2], vp[1][2], vp[2][2]);
    vec3 r3 = vec3(vp[0][3], vp[1][3], vp[2][3]);
    float w_reach = radius * length(r3);
    return abs(clip.x) > clip.w + w_reach + radius * length(r0) || abs(clip.y) > clip.w + w_reach + radius * length(r1)
        || abs(clip.z) > clip.w + w_reach + radius * length(r2);
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;

    if (pc.mode == 4u) {
        const uint level = 3u;
        uint width = max(velk_culling.data[pc.pyramid_word + 1u] >> level, 1u);
        uint height = max(velk_culling.data[pc.pyramid_word + 2u] >> level, 1u);
        uint first = velk_culling.data[pc.pyramid_word + 3u + level];
        uvec2 at = uvec2(coord) * uvec2(width, height) / uvec2(pc.width, pc.height);
        float depth = uintBitsToFloat(velk_culling.data[first + at.y * width + at.x]);
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], coord, vec4(vec3(pow(depth, 0.25)), 1.0));
        return;
    }

    uvec2 ids = velk_pixel_ids(pc.ids_word, pc.width, coord);
    vec3 color = vec3(0.0);
    if (pc.mode == 15u) {
        if (ids.x != 0u) color = light_color(ids, coord);
    } else if (pc.mode == 14u) {
        // By the kept cut record it is from, white for none.
        if (ids.x != 0u) {
            uint record = velk_ids_record(velk_draw_data.data[velk_ids_draw(ids)], ids);
            color = record == VELK_NO_RECORD ? vec3(1.0) : record_color(record);
        }
    } else if (pc.mode == 13u) {
        // A quad in its colour, darker the more it faces away from overhead, so its shape shows.
        if (ids.x != 0u) {
            VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
            if ((d.flags & VELK_DRAW_QUADS) != 0u) {
                uint quad = velk_ids_slot(d, ids);
                vec3 u = vec3(uintBitsToFloat(velk_culling.data[quad + 3u]), uintBitsToFloat(velk_culling.data[quad + 4u]),
                              uintBitsToFloat(velk_culling.data[quad + 5u]));
                vec3 v = vec3(uintBitsToFloat(velk_culling.data[quad + 6u]), uintBitsToFloat(velk_culling.data[quad + 7u]),
                              uintBitsToFloat(velk_culling.data[quad + 8u]));
                vec3 n = cross(u, v);
                float facing = dot(n, n) > 0.0 ? abs(normalize(n).y) : 1.0;
                color = unpackUnorm4x8(velk_culling.data[quad + 9u]).rgb * (0.4 + 0.6 * facing);
            } else {
                color = vec3(0.25);
            }
        }
    } else if (pc.mode >= 10u) {
        if (ids.x != 0u) {
            uint draw = velk_ids_draw(ids);
            color = id_color(draw * 16381u + velk_ids_slot(velk_draw_data.data[draw], ids) * 8191u) * 0.4;
        }
    } else if (pc.mode >= 7u) {
        if (ids.x != 0u) color = cluster_color(ids);
    } else if (pc.mode >= 5u) {
        if (ids.x != 0u) color = triangle_size_color(ids);
    } else if (ids.x != 0u) {
        uint draw = velk_ids_draw(ids);
        VelkDrawData d = velk_draw_data.data[draw];
        uint id = draw * 16381u;
        // By the instance's slot, which stays the same from frame to frame; a position in the
        // frame's list of what is visible does not.
        if (pc.mode >= 2u) id += velk_ids_slot(d, ids) * 8191u;
        if (pc.mode >= 3u) id += velk_ids_triangle(d, ids) * 131u;
        color = id_color(id);
    }
    if (pc.dim_outside != 0u && ids.x != 0u && outside_chooser(ids)) color *= 0.3;
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], coord, vec4(color, 1.0));
}
)";

// What the points of light's shaders take of the light code, built on the deferred prelude: the
// culling arena (written by the trace), the light grid, whose word is in the light header at
// CELLS_HEADER (pc.cells_header), word 18, and the area lights' axes.
#define VELK_LIGHT_GRID_ONLY_GLSL R"(
layout(set = 1, binding = 16, std430) buffer VelkLightCellWords { uint data[]; } velk_light_cell_words;
#define CELLS_HEADER pc.cells_header
)"

// Square area lights' shape, for the points of light's shaders.
#define VELK_LIGHT_AREA_GLSL R"(
// Half of each side of a square area light facing `facing`, `half_size` across from its
// middle: the same axes velk_area_irradiance uses.
void velk_area_axes(vec3 facing, float half_size, out vec3 a, out vec3 b)
{
    vec3 ln = normalize(facing);
    vec3 lt = abs(ln.y) < 0.9 ? normalize(cross(vec3(0.0, 1.0, 0.0), ln)) : normalize(cross(vec3(1.0, 0.0, 0.0), ln));
    a = lt * half_size;
    b = cross(ln, lt) * half_size;
}

// The diffuse light reaching a surface at `pos` facing `N` from a square area light with
// nothing in the way, as a factor on the light's colour times intensity: on the same scale as
// a point light's N.L, as velk_area_irradiance's diffuse is used.
float velk_area_diffuse(vec3 middle, vec3 facing, float half_size, vec3 pos, vec3 N)
{
    vec3 a;
    vec3 b;
    velk_area_axes(facing, half_size, a, b);
    vec3 c = middle - pos;
    vec3 p[4] = vec3[4](c - a - b, c + a - b, c + a + b, c - a + b);
    // Only what is above the surface's horizon: clipped there, up to five corners.
    vec3 q[5];
    int m = 0;
    for (int i = 0; i < 4; ++i) {
        vec3 u = p[i];
        vec3 v = p[(i + 1) & 3];
        float du = dot(N, u);
        float dv = dot(N, v);
        if (du >= 0.0 && m < 5) q[m++] = u;
        if ((du >= 0.0) != (dv >= 0.0) && m < 5) q[m++] = mix(u, v, du / (du - dv));
    }
    if (m < 3) return 0.0;
    vec3 f = vec3(0.0);
    for (int i = 0; i < m; ++i) {
        vec3 u = normalize(q[i]);
        vec3 v = normalize(q[i + 1 == m ? 0 : i + 1]);
        vec3 e = cross(u, v);
        float el = length(e);
        if (el > 1e-8) f += e * (acos(clamp(dot(u, v), -1.0, 1.0)) / el);
    }
    return abs(dot(f, N)) * 0.5;
}
)"

// The lights by where they reach, rebuilt each frame (DeferredPath::build_light_grid), in the
// culling arena at the light header's word 18, 0xFFFFFFFF if there is none. Its words: how many
// buckets, a power of two; the size of its cubes, a float; how many lights are listed
// everywhere, and a spare word; those lights; then two words a bucket, where its lights start
// from the grid's first word and how many; then the buckets' lights. A light is listed in the
// cubes its range's box covers, or everywhere. A bucket holds the lights of the cubes that hash
// to it, so it can list lights that do not reach a place in its cube. Follows the declaration
// of velk_light_cell_words and CELLS_HEADER.
#define VELK_LIGHT_GRID_GLSL R"(
uint velk_light_grid_hash(ivec3 cube)
{
    uint h = (uint(cube.x) * 0x8da6b343u) ^ (uint(cube.y) * 0xd8163841u) ^ (uint(cube.z) * 0xcb1ab31fu);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// How many lights could reach the place `world_at`, from the world's origin, and in `first`
// where its bucket's lights start, for velk_light_grid_at. Every light if there is no grid.
uint velk_light_grid_count(vec3 world_at, out uint first)
{
    first = 0u;
    uint grid = velk_light_cell_words.data[CELLS_HEADER + 18u];
    if (grid == 0xFFFFFFFFu) return pc.light_count;
    uint buckets = velk_light_cell_words.data[grid];
    float size = uintBitsToFloat(velk_light_cell_words.data[grid + 1u]);
    uint everywhere = velk_light_cell_words.data[grid + 2u];
    uint b = grid + 4u + everywhere + (velk_light_grid_hash(ivec3(floor(world_at / size))) & (buckets - 1u)) * 2u;
    uint count = velk_light_cell_words.data[b + 1u];
    if (count != 0u) first = grid + velk_light_cell_words.data[b];
    return everywhere + count;
}

// The `k`th light that could reach the place velk_light_grid_count gave `first` for.
uint velk_light_grid_at(uint first, uint k)
{
    uint grid = velk_light_cell_words.data[CELLS_HEADER + 18u];
    if (grid == 0xFFFFFFFFu) return k;
    uint everywhere = velk_light_cell_words.data[grid + 2u];
    return k < everywhere ? velk_light_cell_words.data[grid + 4u + k] : velk_light_cell_words.data[first + k - everywhere];
}
)"

/// The light code the points of light's passes are built with (the lighting pass and
/// light_points_trace_main_src): see VELK_LIGHT_GRID_ONLY_GLSL.
[[maybe_unused]] constexpr string_view light_grid_src = VELK_LIGHT_GRID_ONLY_GLSL VELK_LIGHT_AREA_GLSL VELK_LIGHT_GRID_GLSL;

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
#include "velk-ui.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 1, rgba8)   uniform writeonly image2D gStorageImages[];
layout(set = 0, binding = 2, rgba32f) uniform writeonly image2D gStorageImagesF32[];
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

// velk_globals (set = 1 slot 2) is declared in velk.glsl. pc.globals_base
// selects this view's record.
#define VELK_GLOBALS velk_globals.data[pc.globals_base]

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

// Per-primitive ray geometry (set = 1 slot 11), plus the trees it points at (slots 12 / 13).
// A primitive's runs are allocated once at load and never move.
layout(set = 1, binding = 11, std430) readonly buffer VelkMeshStatic { MeshStaticData data[]; } velk_mesh_static_records;
layout(set = 1, binding = 12, std430) readonly buffer VelkMeshNodes { VelkMeshNode data[]; } velk_mesh_nodes;
layout(set = 1, binding = 13, std430) readonly buffer VelkClusterWords { uint data[]; } velk_cluster_words;

// RtShape comes from velk.glsl. View-level globals (inverse_view_projection, the
// view's top tree for rays, present_counter) are read through VELK_GLOBALS.
)" R"(

// scalar layout: tight packing so the per-dispatch CPU struct (whose
// `vec4 cam_pos` sits at offset 0 of the struct) lines up with the
// GPU's view of cam_pos at offset 8 of the push-constant block. Default
// std430 would round vec4 up to a 16-byte alignment and shift every
// subsequent field by 8 bytes vs. what the CPU writes.
layout(push_constant, scalar) uniform PC {
    uint globals_base;         // offset 0  (4 bytes; view's FrameGlobals index)
    uint spare0;               // 4
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
    uint cells_header;         // 64  word of the light header. See CELLS_HEADER
    uint ltc_matrix_id;        // 68  LTC transform table; 0 = area specular off
    uint lights_base;          // 72  index into velk_lights (set = 1 slot 5)
    uint ltc_magnitude_id;     // 76  LTC energy / Fresnel table
    vec2 env_params;           // 80  x = intensity, y = rotation_rad (inline)
    uint spare6;               // 88
    uint spare1;               // 92
    uint mode;                 // 96  what a pass is to do, for passes that do more than one thing
    uint spare3;               // 100
    uint spare4;               // 104
    uint spare2;               // 108
    uint spare5;               // 112
} pc;

// A scene light, with its position relative to the camera.
Light velk_light_at(uint index)
{
    Light light = velk_lights.data[index];
    light.position.xyz -= VELK_GLOBALS.cam_pos.xyz;
    return light;
}

#include "velk_intersect.glsl"

// The walk's stack per thread, in shared memory: 48 for 64 threads is 12 KB. The light trace's
// log gives the deepest a frame's rays got.
#define VELK_TRACE_STACK_SIZE 48
#include "velk_trace.glsl"
)"
                                                                      R"(
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

// Area lights in closed form, for deferred_lighting_main_src. Appended after the prelude,
// whose Light declaration it depends on.
[[maybe_unused]] constexpr string_view deferred_area_light_src = R"(
// Analytic area-light shading. The clamped-cosine integral over the emitter's
// spherical polygon, evaluated in closed form: no samples, no noise, nothing
// for the denoiser to resolve. Validated against Monte Carlo in
// design-notes/spike_analytic/analytic.cpp before being written here.

// Vertex capacity of every polygon handled here: the light's four corners, which
// clipping at the horizon can grow by one.
const int kOccPolyMax = 8;

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

// Vector form factor of a spherical polygon given as directions.
vec3 velk_form_factor(vec3 p[kOccPolyMax], int n)
{
    vec3 f = vec3(0.0);
    for (int i = 0; i < n; ++i) {
        f += velk_ff_edge(normalize(p[i]), normalize(p[i + 1 == n ? 0 : i + 1]));
    }
    return f;
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

// Diffuse irradiance (.x) and the LTC specular integral (.y) from a square area light at
// `world_pos`, with nothing in the way. What blocks it comes from the points of light, as for
// any light.
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

    vec3 ctr = light.position.xyz - world_pos;
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

    return vec2(f_max, s_max) / (2.0 * 3.14159265);
}
)";

// Stands in for the analytic area-light path in views that have no area
// lights. The lighting main still calls velk_area_irradiance, but a constant
// zero folds away completely, leaving none of the polygon machinery's register
// footprint behind. Compiling the real thing everywhere cost bistro, which has
// no area lights at all, roughly 3x on deferred.lighting: the code never ran,
// but the registers its arrays reserve are charged to every branch.
[[maybe_unused]] constexpr string_view deferred_area_light_stub_src = R"(
vec2 velk_area_irradiance(Light light, vec3 world_pos, vec3 N, vec3 V, float roughness)
{
    return vec2(0.0);
}
)";

[[maybe_unused]] constexpr string_view deferred_lighting_main_src = R"(
#define VELK_LIGHT_WORDS velk_light_cell_words
)" VELK_LIGHT_POOL_GLSL VELK_LIGHT_SLOTS_GLSL VELK_LIGHT_READ_GLSL R"(
// The bounce light at a pixel from the screen probes around it (light_probes_main_src, this
// frame's store at pc.spare1): the four nearest, by how near they are on screen, how much their
// normal agrees with `n` and how near `pos` is to their surface's plane. False where none of
// them has a surface like it.
bool velk_probe_indirect(ivec2 coord, vec3 pos, vec3 n, out vec3 light)
{
    light = vec3(0.0);
    if (pc.spare1 == 0xFFFFFFFFu) return false;
    const float kProbe = 16.0;
    uint across = (pc.width + 15u) / 16u;
    uint down = (pc.height + 15u) / 16u;
    vec2 at = (vec2(coord) + 0.5) / kProbe - 0.5;
    ivec2 base = ivec2(floor(at));
    vec2 f = at - vec2(base);
    float total = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 p = clamp(base + ivec2(k & 1, k >> 1), ivec2(0), ivec2(int(across) - 1, int(down) - 1));
        uint w = pc.spare1 + (uint(p.y) * across + uint(p.x)) * 8u;
        if (uintBitsToFloat(velk_light_cell_words.data[w + 7u]) <= 0.0) continue;
        vec2 e = unpackSnorm2x16(velk_light_cell_words.data[w + 6u]);
        vec3 pn = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
        if (pn.z < 0.0) pn.xy = (1.0 - abs(pn.yx)) * vec2(pn.x >= 0.0 ? 1.0 : -1.0, pn.y >= 0.0 ? 1.0 : -1.0);
        pn = normalize(pn);
        vec3 ppos = vec3(uintBitsToFloat(velk_light_cell_words.data[w + 3u]), uintBitsToFloat(velk_light_cell_words.data[w + 4u]),
                         uintBitsToFloat(velk_light_cell_words.data[w + 5u]));
        float bilinear = ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y);
        float facing = pow(max(dot(pn, n), 0.0), 4.0);
        float plane = exp(-abs(dot(pn, pos - ppos)) / (0.02 * length(pos) + 1e-3));
        float weight = bilinear * facing * plane + 1e-6 * bilinear;
        light += weight * vec3(uintBitsToFloat(velk_light_cell_words.data[w]), uintBitsToFloat(velk_light_cell_words.data[w + 1u]),
                               uintBitsToFloat(velk_light_cell_words.data[w + 2u]));
        total += weight;
    }
    if (total <= 1e-5) return false;
    light /= total;
    return true;
}

// Lights each pixel: each light's diffuse light and highlight, shadowed by the points of light
// on the cut (pc.spare3 the pool, pc.spare4 and pc.spare2 the pixel ids' word and width,
// pc.spare5 kLightPixels' bits), unshadowed until they are traced; indirect light from the
// screen probes (velk_probe_indirect), or the environment; reflections from the environment;
// area lights in closed form; emission. Writes the finished image.
void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;

    vec2 uv = (vec2(coord) + 0.5) / vec2(float(pc.width), float(pc.height));
    vec4 albedo = velk_texture(pc.albedo_tex_id, uv);
    vec4 normal_and_error = velk_texture(pc.normal_tex_id, uv);
    vec3 world_n = normal_and_error.xyz;
    vec3 world_pos = velk_texture(pc.worldpos_tex_id, uv).xyz;
    vec4 mat = velk_texture(pc.material_tex_id, uv);

    // No surface: the environment along the view ray.
    if (dot(world_n, world_n) < 1e-6) {
        vec2 ndc = uv * 2.0 - 1.0;
        mat4 inv_vp = VELK_GLOBALS.inverse_view_projection;
        vec4 near_h = inv_vp * vec4(ndc, 1.0, 1.0);
        vec4 far_h = inv_vp * vec4(ndc, 0.0, 1.0);
        vec3 rd = normalize(far_h.xyz / far_h.w - near_h.xyz / near_h.w);
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(env_miss_color(rd), 1.0));
        return;
    }

    // Added to the lit colour, before tonemapping, so that bloom can pick up bright emitters.
    vec3 emissive = velk_texture(pc.emissive_tex_id, uv).rgb;

    // LightingMode encoded in mat.b (0 = Unlit, 1 = Standard, ...).
    uint lighting_mode = uint(mat.b * 255.0 + 0.5);
    float metallic = clamp(mat.r, 0.0, 1.0);
    float roughness = clamp(mat.g, 0.04, 1.0);

    vec3 color = albedo.rgb;
    if (lighting_mode != 0u) {
        vec3 N = normalize(world_n);
        vec3 V = normalize(-world_pos);
        float NdotV = max(dot(N, V), 0.0);
        vec3 F0 = mix(vec3(0.04), albedo.rgb, metallic);
        float a = roughness * roughness;

        vec3 direct = vec3(0.0);
        vec3 highlights = vec3(0.0);
        vec3 area_irr = vec3(0.0);
        vec3 area_spec = vec3(0.0);
        // The lights its points of light keep and what of each gets through; before its points
        // are traced, every light, unshadowed.
        uint point_lights[VELK_LIGHT_SLOTS];
        float point_through[VELK_LIGHT_SLOTS];
        bool points = false;
        if (pc.spare3 != VELK_LIGHT_NONE) {
            points = velk_light_direct(pc.spare3, velk_pixel_ids(pc.spare4, pc.spare2, coord), world_pos,
                                       uintBitsToFloat(pc.spare5), point_lights, point_through);
        }
        // Counted, for the log: a lit pixel whose light is not traced yet.
        if (!points && pc.spare3 != VELK_LIGHT_NONE) atomicAdd(velk_light_cell_words.data[pc.spare3 + VELK_POOL_UNTRACED_PIXELS], 1u);
        uint light_count = points ? VELK_LIGHT_SLOTS : pc.light_count;
        for (uint k = 0u; k < light_count; ++k) {
            if (points && point_lights[k] == VELK_LIGHT_NONE) continue;
            uint i = points ? point_lights[k] : k;
            float through = points ? point_through[k] : 1.0;
            Light light = velk_light_at(pc.lights_base + i);
            // Area lights are integrated in closed form, and blocked by what the points say.
            if (light.flags.x == 3u) {
                vec2 es = velk_area_irradiance(light, world_pos, N, V, roughness);
                vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * through;
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

            vec3 H = normalize(L + V);
            float NdotH = max(dot(N, H), 0.0);
            float VdotH = max(dot(V, H), 0.0);
            float D = ggx_d(NdotH, a);
            float G = smith_g(NdotV, NdotL, roughness);
            vec3 F = fresnel_schlick(VdotH, F0);
            direct += NdotL * radiance * through;
            highlights += (D * G) * F / max(4.0 * NdotV * NdotL, 1e-6) * NdotL * radiance * through;
        }

        // Env lighting: single-sample approximation. Diffuse reads along N with a deep LOD
        // so it reads roughly the irradiance average; specular reads the mirror reflection
        // with LOD scaled by roughness as a cheap GGX-prefilter stand-in.
        float env_max_lod = float(textureQueryLevels(velk_textures[nonuniformEXT(pc.env_texture_id)])) - 1.0;
        vec3 env_diffuse = env_miss_color_lod(N, env_max_lod);
        vec3 env_specular = env_miss_color_lod(reflect(-V, N), roughness * env_max_lod);
        vec3 F_env = fresnel_schlick_roughness(NdotV, F0, roughness);

        // Light from other surfaces and the sky: the screen probes around it, or the environment
        // where none has a surface like it yet.
        vec3 indirect;
        if (!velk_probe_indirect(coord, world_pos, N, indirect)) indirect = env_diffuse;

        color = albedo.rgb * (1.0 - metallic) * (direct + indirect + area_irr) + highlights + F_env * env_specular
              + area_spec;
    }

    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(color + emissive, albedo.a));
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
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];
// Float storage, for targets carrying HDR rather than display values.
layout(set = 0, binding = 2, rgba32f) uniform writeonly image2D gStorageImagesF32[];

// The per-view frame state (inv-VP, cam, the view's top tree for rays, present_counter)
// comes from the bound FrameGlobals record, indexed by pc.globals_base. See VELK_GLOBALS
// below.

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

// Per-primitive ray geometry (set = 1 slot 11), plus the trees it points at (slots 12 / 13),
// the same buffers the deferred prelude reads. A primitive's runs are allocated once at load
// and never move.
layout(set = 1, binding = 11, std430) readonly buffer VelkMeshStatic { MeshStaticData data[]; } velk_mesh_static_records;
layout(set = 1, binding = 12, std430) readonly buffer VelkMeshNodes { VelkMeshNode data[]; } velk_mesh_nodes;
layout(set = 1, binding = 13, std430) readonly buffer VelkClusterWords { uint data[]; } velk_cluster_words;

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
    uint shapes_base;           // the main's own: the reference path's restart, unused otherwise
    uint globals_base;          // FrameGlobals index (set = 1 slot 2)
    uint light_count;
    uint lights_base;           // index into velk_lights (set = 1 slot 5)
    vec2 env_params;            // x = intensity, y = rotation_rad (inline)
    uvec4 extras;               // the main's own: for the RT path x=image_index, y=width, z=height
    uvec4 env;                  // x=env_material_id, y=env_texture_id, zw=_
} pc;
// This view's FrameGlobals, read by index from the bound globals buffer
// (set = 1 slot 2, declared in velk.glsl) instead of a device address.
#define VELK_GLOBALS velk_globals.data[pc.globals_base]

// A scene light, with its position relative to the camera.
Light velk_light_at(uint index)
{
    Light light = velk_lights.data[index];
    light.position.xyz -= VELK_GLOBALS.cam_pos.xyz;
    return light;
}

// ===== Core ray / hit / fill-context types =====
#include "velk_intersect.glsl"

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

// The walk's stack per thread, in shared memory: 32 for 64 threads is 8 KB. The light trace's
// log gives the deepest the probes' rays got.
#define VELK_TRACE_STACK_SIZE 32
#include "velk_trace.glsl"

)" R"(
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
// Set to a light index to shade against that light alone (see the loop bounds
// below); -1 shades against all of them. Per-invocation, like any GLSL global.
int velk_single_light = -1;
// The reciprocal of the probability with which that light was chosen. A caller
// picking uniformly sets the light count; one importance-sampling sets
// total_weight / chosen_weight. Ignored unless velk_single_light is set.
float velk_single_light_weight = 1.0;

// Direction to a light and its distance / spot attenuation, with no shadow
// term. Shared rather than open-coded twice, so a caller weighting lights by
// their unshadowed contribution uses exactly the maths the shading loop will
// then apply.
void velk_light_sample(Light light, vec3 pos, out vec3 L, out float atten)
{
    atten = 1.0;
    if (light.flags.x == 0u) {
        L = -light.direction.xyz;
    } else {
        vec3 to_light = light.position.xyz - pos;
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
}

// What the last shaded surface passes on of the light arriving from around it. Zero for
// materials that do not go through velk_pbr_shade.
vec3 velk_hit_diffuse = vec3(0.0);

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
    // Single-light mode. -1 (the default) shades against every light, which is
    // what primary RT rays want. The reference path sets one index and the
    // reciprocal of the probability it was chosen with: its hits accumulate over
    // many frames, so one light per hit converges to the same answer while
    // keeping the ray count per hit independent of how many lights there are.
    uint li_begin = (velk_single_light >= 0) ? uint(velk_single_light) : 0u;
    uint li_end   = (velk_single_light >= 0) ? uint(velk_single_light) + 1u : pc.light_count;
    float li_weight = (velk_single_light >= 0) ? velk_single_light_weight : 1.0;
    for (uint li = li_begin; li < li_end; ++li) {
        Light light = velk_light_at(pc.lights_base + li);
        vec3 L;
        float atten;
        velk_light_sample(light, ctx.rel_pos, L, atten);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0 || atten <= 0.0) continue;
        float shadow = velk_eval_shadow(light.flags.y, li, ctx.rel_pos, N);
        vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * atten * shadow;
        direct += base * (1.0 - metallic) * NdotL * radiance * li_weight;
    }

    // Diffuse term: crude "irradiance at the normal" via a single env
    // sample, modulated by AO. Upgrade to preconvolved irradiance or
    // stochastic cosine sampling when visibly needed.
    vec3 env_at_normal = env_miss_color(N);
    vec3 diffuse = base * (1.0 - metallic) * env_at_normal * ao;
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    velk_hit_diffuse = kD * base * (1.0 - metallic) * ao;

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
        uint material;
        EvalContext ctx = velk_hit_context(hit, ray.dir, ray.origin + hit.t * ray.dir, hit.normal, material);
        BrdfSample bs = velk_resolve_fill(material, ctx);

        float a = clamp(bs.emission.a, 0.0, 1.0);
        acc += throughput * bs.emission.rgb * a;
        if (bs.terminate) {
            if (a >= 0.999) return acc;
            // Transparent: continue straight through, reduced throughput.
            throughput *= (1.0 - a);
            ray.origin = ctx.rel_pos +ray.dir * 1e-3;
            continue;
        }
        throughput *= bs.throughput;
        ray.origin = ctx.rel_pos +hit.normal * 1e-3;
        ray.dir = bs.next_dir;
    }
    // Bounce budget exhausted. The remaining throughput is illuminated by
    // the environment along whatever direction we were about to trace;
    // without this the mirror-of-mirror chain would blackhole.
    acc += throughput * env_miss_color(ray.dir);
    return acc;
}

// What a primary ray sees: the nearest surface lit, and through a translucent one what is
// behind it, blended by its alpha.
vec3 trace_primary(Ray ray)
{
    const int kMaxLayers = 8;
    vec3 accum = vec3(0.0);
    vec3 throughput = vec3(1.0);
    for (int layer = 0; layer < kMaxLayers; ++layer) {
        RayHit hit;
        if (!trace_closest_hit(ray, hit)) {
            return accum + throughput * env_miss_color(ray.dir);
        }
        uint material;
        EvalContext ctx = velk_hit_context(hit, ray.dir, ray.origin + hit.t * ray.dir, hit.normal, material);
        BrdfSample bs = velk_resolve_fill(material, ctx);

        vec3 rgb = bs.emission.rgb;
        if (!bs.terminate) {
            // Per-hit sample count: material's preferred count (scales with roughness / lobe
            // width), clamped by the tracer's global cap. Mirror surfaces collapse to 1; rough
            // surfaces spend the budget to flatten GGX noise.
            const uint kSppCap = 12u;
            uint spp = clamp(bs.sample_count_hint, 1u, kSppCap);
            vec3 bounce = vec3(0.0);
            Ray refl;
            refl.origin = ctx.rel_pos + hit.normal * 1e-3;
            refl.dir = bs.next_dir;
            bounce += trace_bounce(refl, bs.throughput);
            for (uint sp = 1u; sp < spp; ++sp) {
                BrdfSample bs2 = velk_resolve_fill(material, ctx);
                refl.dir = bs2.next_dir;
                bounce += trace_bounce(refl, bs2.throughput);
            }
            rgb += bounce * (1.0 / float(spp));
        }
        float a = clamp(bs.emission.a, 0.0, 1.0);
        accum += throughput * rgb * a;
        if (a >= 0.999) return accum;
        throughput *= 1.0 - a;
        ray.origin = ctx.rel_pos + ray.dir * 1e-3;
    }
    return accum;
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    uint w = pc.extras.y;
    uint h = pc.extras.z;
    if (coord.x >= int(w) || coord.y >= int(h)) return;

    rng_init(uvec2(coord), VELK_GLOBALS.present_counter);

    // Per-pixel primary sample count. Each sample fires one jittered
    // primary ray; the results are averaged. Reduces edge-aliasing on shape boundaries and damps
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
        vec4 near_h = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 1.0, 1.0);
        vec4 far_h  = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 0.0, 1.0);
        vec3 near_w = near_h.xyz / near_h.w;
        vec3 far_w  = far_h.xyz  / far_h.w;

        Ray primary;
        primary.origin = near_w;
        primary.dir = normalize(far_w - near_w);
        final += trace_primary(primary);
    }

    imageStore(gStorageImages[nonuniformEXT(pc.extras.x)], coord,
               vec4(final * (1.0 / float(kPrimarySamples)), 1.0));
}
)";

/// Path tracing for reference images. Goes after compose_rt_compute's shared body. Each frame
/// traces one path for a share of the pixels and adds it to a running sum, so the cost of a
/// frame stays bounded. `extras` is the sum image (RGBA32F, alpha counts paths), the image
/// shown (RGBA16F), width and height. `shapes_base` is nonzero to start over. `env.y` is the
/// environment texture, and `env.z` and `env.w` its intensity and rotation as bits. `env.x`
/// must be 0, so that a surface is not lit by the environment along its normal as well.
#define VELK_REFERENCE_PATH_GLSL R"(
// The brightest the sky may count. Readouts lower it so that a tiny sun cannot dominate them.
float reference_sky_cap = 1e30;

vec3 reference_sky(vec3 rd)
{
    if (pc.env.y == 0u) return vec3(0.0);
    float rotation = uintBitsToFloat(pc.env.w);
    float c = cos(rotation);
    float s = sin(rotation);
    vec3 dir = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
    vec2 uv = vec2(atan(dir.z, dir.x) / 6.28318531 + 0.5, asin(clamp(dir.y, -1.0, 1.0)) / 3.14159265 + 0.5);
    return min(textureLod(velk_textures[nonuniformEXT(pc.env.y)], uv, 0.0).rgb * uintBitsToFloat(pc.env.z), vec3(reference_sky_cap));
}

vec3 reference_cosine(vec3 n)
{
    vec3 up = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(up, n));
    vec3 b = cross(n, t);
    float v = rng_next_float();
    float angle = rng_next_float() * 6.28318531;
    float radius = sqrt(v);
    return t * (radius * cos(angle)) + b * (radius * sin(angle)) + n * sqrt(1.0 - v);
}

// One light per surface, in proportion to what it adds there.
void reference_pick_light(vec3 pos, vec3 n)
{
    velk_single_light = -1;
    velk_single_light_weight = 1.0;
    float w_sum = 0.0;
    float w_chosen = 0.0;
    for (uint li = 0u; li < pc.light_count; ++li) {
        Light light = velk_light_at(pc.lights_base + li);
        vec3 L;
        float atten;
        velk_light_sample(light, pos, L, atten);
        vec3 rad = light.color_intensity.rgb * light.color_intensity.a;
        float lw = (rad.r + rad.g + rad.b) * atten * max(dot(n, L), 0.05);
        if (lw <= 0.0) continue;
        w_sum += lw;
        if (rng_next_float() * w_sum <= lw) {
            velk_single_light = int(li);
            w_chosen = lw;
        }
    }
    if (w_chosen > 0.0) velk_single_light_weight = w_sum / w_chosen;
}

float reference_luma(vec3 c)
{
    return c.r + c.g + c.b;
}

// The light a path brings back, and in `first` what its first hit or miss alone brought.
vec3 reference_path(Ray ray, out vec3 first)
{
    const uint kBounces = 12u;
    first = vec3(0.0);
    vec3 total = vec3(0.0);
    vec3 carried = vec3(1.0);
    for (uint bounce = 0u; bounce < kBounces; ++bounce) {
        RayHit hit;
        if (!trace_closest_hit(ray, 1e30, hit)) {
            total += carried * reference_sky(ray.dir);
            if (bounce == 0u) first = total;
            break;
        }
        uint material;
        EvalContext ctx = velk_hit_context(hit, ray.dir, ray.origin + hit.t * ray.dir, hit.normal, material);

        reference_pick_light(ctx.rel_pos, hit.normal);
        velk_hit_diffuse = vec3(0.0);
        BrdfSample bs = velk_resolve_fill(material, ctx);

        // Partly transparent: either the ray goes straight through, or the surface counts in full.
        if (rng_next_float() >= clamp(bs.emission.a, 0.0, 1.0)) {
            ray.origin = ctx.rel_pos + ray.dir * 0.01;
            continue;
        }
        total += carried * bs.emission.rgb;
        if (bounce == 0u) first = total;
        if (bs.terminate) break;

        vec3 n = dot(hit.normal, ray.dir) > 0.0 ? -hit.normal : hit.normal;
        float diffuse_part = reference_luma(velk_hit_diffuse);
        float specular_part = reference_luma(bs.throughput);
        if (diffuse_part + specular_part < 1e-4) break;
        float specular_share = specular_part / (diffuse_part + specular_part);
        if (rng_next_float() < specular_share) {
            carried *= bs.throughput / specular_share;
            ray.dir = bs.next_dir;
            if (dot(ray.dir, n) <= 0.0) break;
        } else {
            carried *= velk_hit_diffuse / (1.0 - specular_share);
            ray.dir = reference_cosine(n);
        }
        ray.origin = ctx.rel_pos + n * 0.01;
        if (max(carried.r, max(carried.g, carried.b)) < 1e-3) break;
    }
    return total;
}
)"

inline constexpr string_view rt_reference_main_src = VELK_REFERENCE_PATH_GLSL R"(
void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    uint w = pc.extras.z;
    uint h = pc.extras.w;
    if (coord.x >= int(w) || coord.y >= int(h)) return;

    uint frame = VELK_GLOBALS.present_counter;
    vec4 held = pc.shapes_base != 0u ? vec4(0.0) : texelFetch(velk_textures[nonuniformEXT(pc.extras.x)], coord, 0);

    const uint kShare = 8u;
    if ((uint(coord.x) + uint(coord.y) * 3u + frame) % kShare == 0u) {
        rng_init(uvec2(coord), frame);
        vec2 jitter = vec2(rng_next_float(), rng_next_float());
        vec2 ndc = (vec2(coord) + jitter) / vec2(float(w), float(h)) * 2.0 - 1.0;
        vec4 near_h = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 1.0, 1.0);
        vec4 far_h = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 0.0, 1.0);
        vec3 near_w = near_h.xyz / near_h.w;
        vec3 far_w = far_h.xyz / far_h.w;

        Ray primary;
        primary.origin = near_w;
        primary.dir = normalize(far_w - near_w);
        vec3 first_hit;
        vec3 brought = reference_path(primary, first_hit);
        if (!any(isnan(brought)) && !any(isinf(brought))) {
            held += vec4(brought, 1.0);
        }
    }

    imageStore(gStorageImagesF32[nonuniformEXT(pc.extras.x)], coord, held);
    imageStore(gStorageImagesF16[nonuniformEXT(pc.extras.y)], coord, vec4(held.rgb / max(held.a, 1.0), 1.0));
}
)";

/// Measures a frame for render tests. Mode 0 clears this frame's words. Mode 1 follows each
/// surface back to the previous frame and adds up how much its lighting changed, so camera
/// motion does not count and surfaces that moved are skipped. `words` holds: valid, summed
/// change times 256, over 5%, over 20%, the same four for surfaces facing up or down, two
/// unused, then per screen tile valid and summed.
inline constexpr string_view test_diagnostics_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 2, rgba32f) uniform writeonly image2D gStorageImagesF32[];
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];
layout(set = 1, binding = 16, std430) buffer TestWords { uint data[]; } test_words;

layout(push_constant, scalar) uniform PC {
    uint word;
    uint words;
    uint mode;
    uint globals_base;
    uint normal_id;
    uint pos_id;
    uint albedo_id;
    uint color_id;
    uint history_id;
    uint output_id;
    uint width;
    uint height;
    uint tiles_up_only;
    uint view_id;
} pc;

const uint kTilesX = 16u;
const uint kTilesY = 9u;
const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

void add(uint at, uint amount) { atomicAdd(test_words.data[pc.word + at], amount); }

void main()
{
    if (pc.mode == 0u) {
        uint i = gl_LocalInvocationIndex + gl_WorkGroupID.x * 64u;
        if (i < pc.words) test_words.data[pc.word + i] = 0u;
        return;
    }
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.width) || p.y >= int(pc.height)) return;
    if (pc.view_id != 0u) imageStore(gStorageImagesF16[nonuniformEXT(pc.view_id)], p, vec4(0.0, 0.0, 0.0, 1.0));

    vec3 normal = texelFetch(velk_textures[nonuniformEXT(pc.normal_id)], p, 0).xyz;
    if (dot(normal, normal) < 1e-6) {
        imageStore(gStorageImagesF32[nonuniformEXT(pc.output_id)], p, vec4(0.0));
        return;
    }
    vec3 N = normalize(normal);
    vec3 rel = texelFetch(velk_textures[nonuniformEXT(pc.pos_id)], p, 0).xyz;
    vec3 albedo = texelFetch(velk_textures[nonuniformEXT(pc.albedo_id)], p, 0).rgb;
    vec3 color = texelFetch(velk_textures[nonuniformEXT(pc.color_id)], p, 0).rgb;
    float light = max(dot(color, kLuma) / max(dot(albedo, kLuma), 0.04), 1e-4);
    GlobalData g = velk_globals.data[pc.globals_base];
    if (pc.mode == 5u) {
        light = max(dot(albedo, kLuma), 1e-4);
    }
    imageStore(gStorageImagesF32[nonuniformEXT(pc.output_id)], p, vec4(rel, light));

    vec3 prev_pos = rel + g.cam_moved;
    vec4 prev_clip = g.prev_view_projection * vec4(prev_pos, 1.0);
    if (prev_clip.w <= 1e-6) return;
    vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
    if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThanEqual(prev_uv, vec2(1.0)))) return;
    ivec2 q = ivec2(prev_uv * vec2(float(pc.width), float(pc.height)));
    ivec2 last = ivec2(int(pc.width) - 1, int(pc.height) - 1);

    // The range of the light on this surface around where it was. Reprojection lands between
    // pixels, so only light outside what was nearby counts as having changed.
    mat4 vp = g.view_projection;
    float focal = length(vec3(vp[0][1], vp[1][1], vp[2][1]));
    float pixel = length(rel) * 2.0 / (focal * float(pc.height));
    float lowest = 1e30;
    float highest = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec4 history = texelFetch(velk_textures[nonuniformEXT(pc.history_id)], clamp(q + ivec2(x, y), ivec2(0), last), 0);
            if (history.a > 0.0 && length(history.xyz - prev_pos) <= pixel * 6.0) {
                lowest = min(lowest, history.a);
                highest = max(highest, history.a);
            }
        }
    }
    if (highest <= 0.0) return;

    float nearest = clamp(light, lowest, highest);
    float change = abs(light - nearest) / max(max(light, nearest), 1e-3);
    // White where the surface faces up or down, blue elsewhere.
    if (pc.view_id != 0u) {
        float shown = min(change * 8.0, 1.0);
        float up = abs(N.y) > 0.9 ? shown : 0.0;
        imageStore(gStorageImagesF16[nonuniformEXT(pc.view_id)], p, vec4(up, up, shown, 1.0));
    }
    uint scaled = uint(change * 256.0);
    add(0u, 1u);
    add(1u, scaled);
    if (change > 0.05) add(2u, 1u);
    if (change > 0.2) add(3u, 1u);
    if (abs(N.y) > 0.9) {
        add(4u, 1u);
        add(5u, scaled);
        if (change > 0.05) add(6u, 1u);
        if (change > 0.2) add(7u, 1u);
    }
    if (pc.tiles_up_only != 0u && abs(N.y) <= 0.9) return;
    uint tile = min(uint(p.y) * kTilesY / pc.height, kTilesY - 1u) * kTilesX + min(uint(p.x) * kTilesX / pc.width, kTilesX - 1u);
    add(10u + tile, 1u);
    add(10u + kTilesX * kTilesY + tile, scaled);
}
)";

/// Appends the #include lines of the intersect snippets registered for this frame.
inline void append_rt_intersect_includes(string& src, const IFrameSnippetRegistry& snippets)
{
    const auto& intersect_info = snippets.intersect_info_by_id();
    for (auto id : snippets.frame_intersects()) {
        if (id < 3 || id - 3 >= intersect_info.size()) continue;
        const auto& ii = intersect_info[id - 3];
        src += string_view("#include \"", 10);
        src += ii.include_name;
        src += string_view("\"\n", 2);
    }
}

/// Appends intersect_shape: built-in rect/cube/sphere kinds and registered visual kinds (3+).
/// Triangle meshes are walked by velk_walk and never get here.
inline void append_rt_intersect_shape(string& src, const IFrameSnippetRegistry& snippets)
{
    const auto append_literal = [&src](const char* s) { src += string_view(s, std::strlen(s)); };
    const auto& intersect_info = snippets.intersect_info_by_id();
    char buf[128];
    append_literal("bool intersect_shape(Ray ray, RtShape shape, out RayHit hit) {\n");
    append_literal("    switch (shape.shape_kind) {\n");
    append_literal("        case 1u: return intersect_cube(ray, shape, hit);\n");
    append_literal("        case 2u: return intersect_sphere(ray, shape, hit);\n");
    for (auto id : snippets.frame_intersects()) {
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
}

/**
 * A point on a drawn triangle, the same for a pixel and a ray: from the draw (VelkDrawData),
 * the instance, the cluster and the triangle, then the weights of its corners, what its
 * material is given. Part of compose_rt_compute's shared body, after velk_material_varyings.
 */
inline constexpr string_view velk_surface_src = R"(
// A corner of a drawn triangle, as its material's vertex function gives it.
struct VelkSurfaceVertex {
    vec3 local;     // in the instance's own space, before its size
    vec3 rel;       // in world space, from the camera
    vec3 normal;
    vec2 uv;
    vec2 uv1;
    vec4 tangent;
    vec4 color;
    uint shape_param;
};

VelkSurfaceVertex velk_surface_vertex(VelkDrawData d, uint cluster, ElementInstance inst, GlobalData g, uint index)
{
    VelkVertex3D v = velk_unpack_vertex3d(velk_cluster_vertex_word(d, cluster) + index * VELK_VERTEX3D_WORDS);
    VelkVaryings s = velk_material_varyings(d.material_id, v, inst, g);
    VelkSurfaceVertex r;
    r.local = inst.offset.xyz + v.position * inst.size.xyz;
    r.rel = s.rel_pos;
    r.normal = s.world_normal;
    r.uv = s.local_uv;
    r.uv1 = velk_read_uv1(velk_cluster_uv1_word(d, cluster, index));
    r.tangent = s.world_tangent;
    r.color = s.color * velk_read_color(velk_cluster_color_word(d, cluster, index));
    r.shape_param = s.shape_param;
    return r;
}

// The vertex indices of a triangle of a draw's cluster. A triangle strip's triangles start
// one vertex apart.
uvec3 velk_surface_corners(VelkDrawData d, uint cluster, uint triangle)
{
    if (d.index_base == VELK_NO_VISIBLE_LIST) return uvec3(triangle) + uvec3(0u, 1u, 2u);
    uint word = d.index_base + velk_cluster_first_index(cluster) + triangle * 3u;
    return uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
}

// What the material is given at the point with corner weights @p weights, at @p rel from the
// camera of @p g.
EvalContext velk_surface_context(VelkDrawData d, GlobalData g, VelkSurfaceVertex v0, VelkSurfaceVertex v1,
                                 VelkSurfaceVertex v2, vec3 weights, vec3 rel)
{
    EvalContext ctx;
    ctx.material_base = d.material_words;
    ctx.texture_id = d.texture_id;
    ctx.shape_param = v0.shape_param;
    ctx.uv = v0.uv * weights.x + v1.uv * weights.y + v2.uv * weights.z;
    ctx.uv1 = v0.uv1 * weights.x + v1.uv1 * weights.y + v2.uv1 * weights.z;
    ctx.base = v0.color * weights.x + v1.color * weights.y + v2.color * weights.z;
    ctx.ray_dir = normalize(rel);
    ctx.normal = v0.normal * weights.x + v1.normal * weights.y + v2.normal * weights.z;
    ctx.hit_pos = rel + g.cam_pos.xyz;
    ctx.rel_pos = rel;
    ctx.tangent = v0.tangent * weights.x + v1.tangent * weights.y + v2.tangent * weights.z;
    return ctx;
}

// What the material of what a ray hit is given, as a pixel's is: from the hit's draw,
// instance, cluster and corners (RayHit, velk_hit_from_words), with the ray's direction,
// where it hit from the camera, and the surface's normal there for an analytic shape.
// `material` is the id velk_eval_material takes, 0 when the hit names no draw.
EvalContext velk_hit_context(RayHit hit, vec3 ray_dir, vec3 rel, vec3 normal, out uint material)
{
    EvalContext ctx;
    ctx.material_base = 0u;
    ctx.texture_id = 0u;
    ctx.shape_param = 0u;
    ctx.uv = hit.uv;
    ctx.uv1 = hit.uv;
    ctx.base = vec4(1.0);
    ctx.normal = normal;
    ctx.tangent = vec4(0.0);
    material = 0u;
    if (hit.draw != 0xFFFFFFFFu && hit.instance != 0xFFFFFFFFu) {
        VelkDrawData d = velk_draw_data.data[hit.draw];
        // A quad's hit names the quad's word, as its pixel does.
        ElementInstance inst = (d.flags & VELK_DRAW_QUADS) != 0u ? velk_quad_instance(hit.instance) : velk_instances.data[hit.instance];
        material = d.material_id;
        if (hit.corners.x == VELK_NO_CORNERS) {
            ctx.material_base = d.material_words;
            ctx.texture_id = d.texture_id;
            ctx.shape_param = inst.params.x;
            ctx.base = inst.color;
        } else {
            GlobalData g = VELK_GLOBALS;
            VelkSurfaceVertex v0 = velk_surface_vertex(d, hit.cluster, inst, g, hit.corners.x);
            VelkSurfaceVertex v1 = velk_surface_vertex(d, hit.cluster, inst, g, hit.corners.y);
            VelkSurfaceVertex v2 = velk_surface_vertex(d, hit.cluster, inst, g, hit.corners.z);
            ctx = velk_surface_context(d, g, v0, v1, v2, vec3(1.0 - hit.weights.x - hit.weights.y, hit.weights), rel);
        }
    }
    ctx.ray_dir = ray_dir;
    ctx.rel_pos = rel;
    ctx.hit_pos = rel + VELK_GLOBALS.cam_pos.xyz;
    return ctx;
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
inline string compose_rt_compute(const IFrameSnippetRegistry& snippets,
                                 string_view main_src = {})
{
    const auto& material_ids        = snippets.frame_materials();
    const auto& shadow_tech_ids     = snippets.frame_shadow_techs();
    const auto& material_info       = snippets.material_info_by_id();
    const auto& shadow_tech_info    = snippets.shadow_tech_info_by_id();

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
    append_rt_intersect_includes(src, snippets);

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

    // velk_material_varyings: dispatch by material id to its vertex function.
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size() || material_info[id - 1].vertex_fn.empty()) continue;
        append_literal("#include \"");
        src += material_info[id - 1].vertex_include;
        append_literal("\"\n");
    }
    append_literal("VelkVaryings velk_material_varyings(uint mid, VelkVertex3D v, ElementInstance inst, GlobalData g) {\n");
    append_literal("    switch (mid) {\n");
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size() || material_info[id - 1].vertex_fn.empty()) continue;
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += material_info[id - 1].vertex_fn;
        append_literal("(v, inst, g);\n");
    }
    append_literal("        default: return velk_default_varyings(v, inst, g);\n");
    append_literal("    }\n");
    append_literal("}\n");
    src += velk_surface_src;

    // velk_eval_material: dispatch by material id, without shading.
    append_literal("MaterialEval velk_eval_material(uint mid, EvalContext ctx) {\n");
    append_literal("    switch (mid) {\n");
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size()) continue;
        const auto& mi = material_info[id - 1];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += mi.fn_name;
        append_literal("(ctx);\n");
    }
    append_literal("        default: {"
                   " MaterialEval e = velk_default_material_eval();"
                   " e.color = ctx.base;"
                   " e.normal = ctx.normal;"
                   " e.roughness = 0.5;"
                   " e.lighting_mode = VELK_LIGHTING_STANDARD;"
                   " return e; }\n");
    append_literal("    }\n");
    append_literal("}\n");

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

    append_rt_intersect_shape(src, snippets);

    // Everything above is the shared RT body. Callers wanting that machinery
    // under a different entry point (the light probes pass) supply their own main.
    src += main_src.empty() ? rt_compute_main_src : main_src;
    return src;
}

} // namespace velk

#endif // VELK_RENDER_DEFAULT_SHADERS_H
