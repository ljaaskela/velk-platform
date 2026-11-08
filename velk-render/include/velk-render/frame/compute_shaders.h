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
    uint shape_index; // only set by trace_closest_hit
};

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

// Ray tracing over the scene's records, shared by every compute shader that
// traces. Registered as the "velk_trace.glsl" include. The including shader
// includes velk_intersect.glsl, defines VELK_TRACE_STACK_SIZE, declares velk_bvh_nodes,
// velk_bvh_shapes, velk_records, velk_mesh_static_records, velk_blas_nodes,
// velk_cluster_words and VELK_GLOBALS.
//
// A record places shapes in the world: rays are moved into its local space, and
// shapes and meshes are intersected there. Origin and direction move by the same
// transform with no renormalising, so a hit's t is the same in both spaces.
//
// A partition's nodes and records are relative to its origin. Entering one subtracts
// the origin from the ray, and returning to the stack depth it was entered at
// restores it. Partitions do not nest.
[[maybe_unused]] constexpr string_view velk_trace_src = R"(
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

// Where a ray that enters the box leaves it.
float ray_aabb_exit(Ray ray, vec3 bmin, vec3 bmax)
{
    vec3 inv_d = 1.0 / ray.dir;
    vec3 t0 = (bmin - ray.origin) * inv_d;
    vec3 t1 = (bmax - ray.origin) * inv_d;
    vec3 tmx = max(t0, t1);
    return min(min(tmx.x, tmx.y), tmx.z);
}

// How wide the area a visibility ray stands for is, in world units, for the shader to
// set before tracing one. 0 means a single line, which tests every instance.
float velk_ray_footprint = 0.0;
// How much wider that gets per unit along the ray, for a light that is not a point.
float velk_ray_spread = 0.0;
// How far the surface a ray starts from may be from the full mesh rays are tested against,
// in world units, when it is drawn at a coarser level of detail. For the shader to set.
float velk_ray_lift = 0.0;
// What a closest-hit ray passed through in nodes too fine for it, for the shader to clear
// before tracing and use after: optical depth, and colour times depth.
float velk_ray_medium_depth = 0.0;
vec3 velk_ray_medium_color = vec3(0.0);
#ifdef VELK_TRACE_MEDIUM
// For a closest-hit ray entering a record whose meshes count as a medium where the ray is
// wider than them: whether this one does, the part of the ray in view, and the colour.
bool velk_ray_mesh_medium = false;
float velk_ray_medium_near = 0.0;
float velk_ray_medium_far = 0.0;
vec3 velk_ray_mesh_color = vec3(0.0);
#endif

// How many footprints wide a node can be and still count as a medium. It varies per node
// so that same-sized nodes do not all change over at one distance, which shows as a ring,
// and it stays the same while the camera moves, so nothing crawls.
float velk_medium_reach(uint node_index)
{
    return 4.0 * (0.5 + float((node_index * 2654435761u) >> 16u) / 65536.0);
}

// The composer generates the body after intersect snippets have been included.
// Built-in kinds forward to the functions the shader defines; visual-registered
// kinds call their snippets.
bool intersect_shape(Ray ray, RtShape shape, out RayHit hit);

// The origin is made relative to the record before it is rotated, so its error
// follows the size of the object and not its distance from the world origin.
Ray velk_local_ray(Ray ray, RtRecord rec)
{
    vec3 rel = ray.origin - vec3(rec.inv_row0.w, rec.inv_row1.w, rec.inv_row2.w);
    Ray local;
    local.origin = vec3(dot(rec.inv_row0.xyz, rel),
                        dot(rec.inv_row1.xyz, rel),
                        dot(rec.inv_row2.xyz, rel));
    local.dir = vec3(dot(rec.inv_row0.xyz, ray.dir),
                     dot(rec.inv_row1.xyz, ray.dir),
                     dot(rec.inv_row2.xyz, ray.dir));
    return local;
}

// Normals transform by the transpose of the inverse.
vec3 velk_world_normal(RtRecord rec, vec3 n)
{
    return normalize(rec.inv_row0.xyz * n.x + rec.inv_row1.xyz * n.y + rec.inv_row2.xyz * n.z);
}

// Triangle-mesh intersector (shape_kind == 255), in the mesh's local space.
// Walks the primitive's tree down to its clusters. See BlasBuild for their layout.
bool intersect_mesh(Ray ray, RtShape shape, out RayHit hit)
{
    {
        float t_aabb;
        if (!ray_aabb(ray, shape.origin.xyz, shape.u_axis.xyz, 1e30, t_aabb)) return false;
    }
    if (shape.mesh_static_base == VELK_INVALID_MESH_STATIC) return false;
    MeshStaticData st = velk_mesh_static(shape);
    if (st.triangle_count == 0u || st.vertex_stride == 0u) return false;
    if (st.blas_node_count == 0u) return false;

    // Normalize the local direction so MT's numerical thresholds
    // (`abs(det) < 1e-7`, `tt < 1e-4`) stay calibrated against mesh-edge
    // magnitudes regardless of world scale. Without this, instances scaled up by
    // a large world matrix produce a tiny local direction, which makes every
    // triangle's `det` collapse below the rejection threshold and silently drops
    // the entire instance. The length converts the hit back to the ray's t.
    vec3 lo = ray.origin;
    float ld_scale = length(ray.dir);
    if (ld_scale < 1e-30) return false;
    vec3 ld = ray.dir / ld_scale;

    uint floats_per_vert = st.vertex_stride >> 2u;  // stride is bytes; floats = bytes/4

    // This primitive's tree and clusters run inside the shared arenas; node and word
    // indices below are relative to these bases.
    uint blas_node_base = st.blas_node_base;
    uint word_base      = st.cluster_word_base;

    Ray local_ray;
    local_ray.origin = lo;
    local_ray.dir    = ld;

    bool  found = false;
    float best_t = 1e30;
    float best_u = 0.0;
    float best_v = 0.0;
    uint  best_triangle = 0u;
    uint  best_vertex_ids = 0u;

    // BLAS walk. Stack depth 32 fits any reasonable tree (a perfectly
    // balanced binary BVH at depth 32 holds 2^32 leaves).
    uint stack[32];
    int sp = 0;
    stack[sp++] = st.blas_root;
    while (sp > 0) {
        uint ni = uint(stack[--sp]);
        BvhNode node = velk_blas_nodes.data[blas_node_base + ni];
        float t_aabb;
        if (!ray_aabb(local_ray, node.aabb_min.xyz, node.aabb_max.xyz, best_t, t_aabb)) continue;

#ifdef VELK_TRACE_MEDIUM
        if (velk_ray_mesh_medium && (st.flags & VELK_MESH_SCATTERED) != 0u) {
            vec3 extent = node.aabb_max.xyz - node.aabb_min.xyz;
            float widest = max(max(extent.x, extent.y), extent.z);
            if (widest < velk_ray_footprint * ld_scale * velk_medium_reach(blas_node_base + ni)) {
                vec3 sides = max(extent, vec3(widest * 1e-3));
                float enter = max(t_aabb, velk_ray_medium_near * ld_scale);
                float leave = min(min(ray_aabb_exit(local_ray, node.aabb_min.xyz, node.aabb_max.xyz), velk_ray_medium_far * ld_scale), best_t);
                float depth = node.aabb_max.w / (sides.x * sides.y * sides.z) * max(leave - enter, 0.0);
                velk_ray_medium_depth += depth;
                velk_ray_medium_color += depth * velk_ray_mesh_color;
                continue;
            }
        }
#endif

        if (node.shape_count > 0u) {
            // Leaf: test triangles, reading the cluster's own positions.
            uint triangle_count = node.shape_count & 0xFFFFu;
            uint positions = word_base + node.first_child;
            for (uint k = 0u; k < triangle_count; ++k) {
                uint tri = velk_cluster_words.data[word_base + node.first_shape + k];
                uint p0 = positions + (tri & 0xFFu) * 3u;
                uint p1 = positions + ((tri >> 8u) & 0xFFu) * 3u;
                uint p2 = positions + ((tri >> 16u) & 0xFFu) * 3u;
                vec3 v0 = vec3(uintBitsToFloat(velk_cluster_words.data[p0]),
                               uintBitsToFloat(velk_cluster_words.data[p0 + 1u]),
                               uintBitsToFloat(velk_cluster_words.data[p0 + 2u]));
                vec3 v1 = vec3(uintBitsToFloat(velk_cluster_words.data[p1]),
                               uintBitsToFloat(velk_cluster_words.data[p1 + 1u]),
                               uintBitsToFloat(velk_cluster_words.data[p1 + 2u]));
                vec3 v2 = vec3(uintBitsToFloat(velk_cluster_words.data[p2]),
                               uintBitsToFloat(velk_cluster_words.data[p2 + 1u]),
                               uintBitsToFloat(velk_cluster_words.data[p2 + 2u]));

                // Moller-Trumbore. The det rejection threshold scales with the
                // actual edge magnitudes so meshes whose local-space coordinates
                // are tiny don't have every triangle silently filtered out as
                // "near-parallel". The same scaling applies to the per-hit `tt`
                // floor so it's a meaningful "ignore self-intersection" distance
                // regardless of mesh scale.
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
                best_triangle = tri;
                best_vertex_ids = positions + (node.shape_count >> 16u) * 3u;
                found = true;
            }
        } else if (node.child_count == 2u) {
            // Front-to-back: push the far child first so a near hit can cull
            // the far subtree through best_t.
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
    if (!found) return false;

    // Interpolate per-vertex normal and UV from the closest hit's barycentrics.
    // Vertex layout: pos[0..2], normal[3..5], uv[6..7]. Moller-Trumbore's (u, v)
    // make (1-u-v, u, v) the weights for (V0, V1, V2). Any-hit callers never read
    // these, so the compiler drops them from those traces.
    float w = 1.0 - best_u - best_v;
    uint best_o0 = velk_cluster_words.data[best_vertex_ids + (best_triangle & 0xFFu)] * floats_per_vert;
    uint best_o1 = velk_cluster_words.data[best_vertex_ids + ((best_triangle >> 8u) & 0xFFu)] * floats_per_vert;
    uint best_o2 = velk_cluster_words.data[best_vertex_ids + ((best_triangle >> 16u) & 0xFFu)] * floats_per_vert;
    vec3 n0 = vec3(velk_mesh_vertex(st, best_o0 + 3u), velk_mesh_vertex(st, best_o0 + 4u), velk_mesh_vertex(st, best_o0 + 5u));
    vec3 n1 = vec3(velk_mesh_vertex(st, best_o1 + 3u), velk_mesh_vertex(st, best_o1 + 4u), velk_mesh_vertex(st, best_o1 + 5u));
    vec3 n2 = vec3(velk_mesh_vertex(st, best_o2 + 3u), velk_mesh_vertex(st, best_o2 + 4u), velk_mesh_vertex(st, best_o2 + 5u));
    vec2 uv0 = vec2(velk_mesh_vertex(st, best_o0 + 6u), velk_mesh_vertex(st, best_o0 + 7u));
    vec2 uv1 = vec2(velk_mesh_vertex(st, best_o1 + 6u), velk_mesh_vertex(st, best_o1 + 7u));
    vec2 uv2 = vec2(velk_mesh_vertex(st, best_o2 + 6u), velk_mesh_vertex(st, best_o2 + 7u));

    hit.t      = best_t / ld_scale;
    hit.uv     = uv0 * w + uv1 * best_u + uv2 * best_v;
    hit.normal = normalize(n0 * w + n1 * best_u + n2 * best_v);
    return true;
}

// Tests one shape in local space. On a nearer hit, updates @p hit, recording the
// shape's index in velk_bvh_shapes.
bool velk_test_shape(Ray local, uint shape_index, float t_min, inout RayHit hit)
{
    RayHit h;
    if (intersect_shape(local, velk_bvh_shapes.data[shape_index], h) && h.t > t_min && h.t < hit.t) {
        hit = h;
        hit.shape_index = shape_index;
        return true;
    }
    return false;
}

)"
                                                                      R"(
// Whether a mesh blocks a ray within t_max, in the ray's t. Apart from intersect_mesh so that
// it stops at the first blocker, within the ray's length, with nothing else to keep.
bool velk_mesh_blocks(Ray ray, RtShape shape, float t_max)
{
    if (shape.mesh_static_base == VELK_INVALID_MESH_STATIC) return false;
    float ld_scale = length(ray.dir);
    if (ld_scale < 1e-30) return false;
    vec3 ld = ray.dir / ld_scale;
    float limit = min(t_max * ld_scale, 1e30);
    Ray local_ray;
    local_ray.origin = ray.origin;
    local_ray.dir = ld;
    float t_aabb;
    if (!ray_aabb(local_ray, shape.origin.xyz, shape.u_axis.xyz, limit, t_aabb)) return false;

    MeshStaticData st = velk_mesh_static(shape);
    if (st.triangle_count == 0u || st.vertex_stride == 0u || st.blas_node_count == 0u) return false;
    uint blas_node_base = st.blas_node_base;
    uint word_base = st.cluster_word_base;

    uint stack[32];
    int sp = 0;
    stack[sp++] = st.blas_root;
    while (sp > 0) {
        BvhNode node = velk_blas_nodes.data[blas_node_base + stack[--sp]];
        if (!ray_aabb(local_ray, node.aabb_min.xyz, node.aabb_max.xyz, limit, t_aabb)) continue;
        if (node.shape_count > 0u) {
            uint triangle_count = node.shape_count & 0xFFFFu;
            uint positions = word_base + node.first_child;
            for (uint k = 0u; k < triangle_count; ++k) {
                uint tri = velk_cluster_words.data[word_base + node.first_shape + k];
                uint p0 = positions + (tri & 0xFFu) * 3u;
                uint p1 = positions + ((tri >> 8u) & 0xFFu) * 3u;
                uint p2 = positions + ((tri >> 16u) & 0xFFu) * 3u;
                vec3 v0 = vec3(uintBitsToFloat(velk_cluster_words.data[p0]), uintBitsToFloat(velk_cluster_words.data[p0 + 1u]),
                               uintBitsToFloat(velk_cluster_words.data[p0 + 2u]));
                vec3 e1 = vec3(uintBitsToFloat(velk_cluster_words.data[p1]), uintBitsToFloat(velk_cluster_words.data[p1 + 1u]),
                               uintBitsToFloat(velk_cluster_words.data[p1 + 2u])) - v0;
                vec3 e2 = vec3(uintBitsToFloat(velk_cluster_words.data[p2]), uintBitsToFloat(velk_cluster_words.data[p2 + 1u]),
                               uintBitsToFloat(velk_cluster_words.data[p2 + 2u])) - v0;
                vec3 p = cross(ld, e2);
                float det = dot(e1, p);
                if (abs(det) < 1e-7 * max(length(e1) * length(p), 1e-30)) continue;
                float inv_det = 1.0 / det;
                vec3 to_v0 = ray.origin - v0;
                float u = dot(to_v0, p) * inv_det;
                if (u < 0.0 || u > 1.0) continue;
                vec3 q = cross(to_v0, e1);
                float v = dot(ld, q) * inv_det;
                if (v < 0.0 || u + v > 1.0) continue;
                float tt = dot(e2, q) * inv_det;
                if (tt >= 1e-4 * max(length(e1), length(e2)) && tt < limit) return true;
            }
        } else if (node.child_count == 2u) {
            // The nearer child first, as the likelier blocker.
            BvhNode l = velk_blas_nodes.data[blas_node_base + node.first_child];
            BvhNode r = velk_blas_nodes.data[blas_node_base + node.first_child + 1u];
            float t_l, t_r;
            bool h_l = ray_aabb(local_ray, l.aabb_min.xyz, l.aabb_max.xyz, limit, t_l);
            bool h_r = ray_aabb(local_ray, r.aabb_min.xyz, r.aabb_max.xyz, limit, t_r);
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
    return false;
}

#ifdef VELK_RAY_MOTION
// The motion at the point of the ray nearest the shape's middle, taken off the ray, moves
// the shape for this ray. Rays wider than the motion would not show it.
Ray velk_moved_ray(Ray local, RtRecord rec, uint shape_index)
{
    if ((rec.flags & 1u) == 0u || velk_ray_footprint >= VELK_RAY_MOTION_REACH) return local;
    RtShape moved = velk_bvh_shapes.data[shape_index];
    vec3 middle = (moved.origin.xyz + moved.u_axis.xyz) * 0.5;
    vec3 nearest = local.origin + local.dir * (dot(middle - local.origin, local.dir) / dot(local.dir, local.dir));
    Ray out_ray = local;
    out_ray.origin -= VELK_RAY_MOTION(nearest, vec3(rec.inv_row0.w, rec.inv_row1.w, rec.inv_row2.w),
                                      float(VELK_GLOBALS.present_counter) / 60.0);
    return out_ray;
}
#endif

// Intersects a record's shapes, returning the nearest hit in (t_min, t_max) with
// its normal in world space. With @p any_hit, returns at the first hit found.
bool velk_intersect_record(Ray ray, uint record, float t_min, float t_max, bool any_hit, out RayHit hit)
{
    RtRecord rec = velk_records.data[record];
    Ray local = velk_local_ray(ray, rec);
    hit.t = t_max;
    bool found = false;
#ifdef VELK_TRACE_MEDIUM
    velk_ray_mesh_medium = !any_hit && velk_ray_footprint > 0.0 && (rec.flags & 2u) != 0u;
#endif
    if (rec.tree_root == VELK_NO_TREE) {
        for (uint i = 0u; i < rec.shape_count; ++i) {
            Ray shape_ray = local;
#ifdef VELK_RAY_MOTION
            shape_ray = velk_moved_ray(local, rec, rec.first_shape + i);
#endif
            if (any_hit) {
                RtShape blocker = velk_bvh_shapes.data[rec.first_shape + i];
                if (blocker.shape_kind == 255u) {
                    if (velk_mesh_blocks(shape_ray, blocker, hit.t)) return true;
                    continue;
                }
            }
            if (velk_test_shape(shape_ray, rec.first_shape + i, t_min, hit)) {
                found = true;
                if (any_hit) return true;
            }
        }
    } else {
        // Many shapes, such as a text visual's glyphs, under a small local tree.
        uint stack[32];
        int sp = 0;
        stack[sp++] = rec.tree_root;
        while (sp > 0) {
            BvhNode node = velk_blas_nodes.data[stack[--sp]];
            float t_enter;
            if (!ray_aabb(local, node.aabb_min.xyz, node.aabb_max.xyz, hit.t, t_enter)) continue;
            for (uint i = 0u; i < node.shape_count; ++i) {
                if (velk_test_shape(local, node.first_shape + i, t_min, hit)) {
                    found = true;
                    if (any_hit) return true;
                }
            }
            for (uint c = 0u; c < node.child_count; ++c) {
                if (sp < 32) stack[sp++] = node.first_child + c;
            }
        }
    }
    if (found) hit.normal = velk_world_normal(rec, hit.normal);
    return found;
}

)"
                                                                      R"(
// The traversals for scenes with instances that have a far form, with VELK_TRACE_MEDIUM
// defined: see velk_with_trace_medium. Apart and compiled out otherwise, since their
// registers are paid for by the whole pass: in the ordinary loops this doubled what every
// ray cost, and beside them it still cost 25%, in scenes where it never runs.
//
// A node of instances not much wider than the ray's footprint counts as a medium, since
// one line through it would hit or miss one instance at random: light falls off with
// distance through it, by its density of instances' area per volume. A node's area is
// in aabb_max.w, 0 for one that is not made of instances, and its average colour is in
// the bits of aabb_min.w, as unorm RGB.
#ifdef VELK_TRACE_MEDIUM
// How much of a wide ray gets through a mesh, in its local space. In a mesh of separate pieces,
// a node much narrower than the ray counts as a medium, by the area under it, so the mesh costs
// a wide ray what the ray can resolve rather than all of its triangles. A solid surface would
// let light through that way, so other meshes are tested exactly.
float velk_mesh_visibility(Ray ray, RtShape shape, float t_max)
{
    if (shape.mesh_static_base == VELK_INVALID_MESH_STATIC) return 1.0;
    float ld_scale = length(ray.dir);
    if (ld_scale < 1e-30) return 1.0;
    vec3 ld = ray.dir / ld_scale;
    float limit = min(t_max * ld_scale, 1e30);
    Ray local_ray;
    local_ray.origin = ray.origin;
    local_ray.dir = ld;
    float enter;
    if (!ray_aabb(local_ray, shape.origin.xyz, shape.u_axis.xyz, limit, enter)) return 1.0;

    MeshStaticData st = velk_mesh_static(shape);
    if (st.triangle_count == 0u || st.vertex_stride == 0u || st.blas_node_count == 0u) return 1.0;
    uint blas_node_base = st.blas_node_base;
    uint word_base = st.cluster_word_base;

    float visibility = 1.0;
    uint stack[32];
    int sp = 0;
    stack[sp++] = st.blas_root;
    while (sp > 0) {
        uint ni = stack[--sp];
        BvhNode node = velk_blas_nodes.data[blas_node_base + ni];
        if (!ray_aabb(local_ray, node.aabb_min.xyz, node.aabb_max.xyz, limit, enter)) continue;
        vec3 extent = node.aabb_max.xyz - node.aabb_min.xyz;
        float widest = max(max(extent.x, extent.y), extent.z);
        // In the mesh's units, where its t is the world's times ld_scale.
        float footprint = velk_ray_footprint * ld_scale + velk_ray_spread * max(enter, 0.0);
        if ((st.flags & VELK_MESH_SCATTERED) != 0u && widest < footprint * velk_medium_reach(blas_node_base + ni)) {
            vec3 sides = max(extent, vec3(widest * 1e-3));
            float len = max(min(ray_aabb_exit(local_ray, node.aabb_min.xyz, node.aabb_max.xyz), limit) - enter, 0.0);
            visibility *= exp(-node.aabb_max.w / (sides.x * sides.y * sides.z) * len);
            continue;
        }
        if (node.shape_count > 0u) {
            uint triangle_count = node.shape_count & 0xFFFFu;
            uint positions = word_base + node.first_child;
            for (uint k = 0u; k < triangle_count; ++k) {
                uint tri = velk_cluster_words.data[word_base + node.first_shape + k];
                uint p0 = positions + (tri & 0xFFu) * 3u;
                uint p1 = positions + ((tri >> 8u) & 0xFFu) * 3u;
                uint p2 = positions + ((tri >> 16u) & 0xFFu) * 3u;
                vec3 v0 = vec3(uintBitsToFloat(velk_cluster_words.data[p0]), uintBitsToFloat(velk_cluster_words.data[p0 + 1u]),
                               uintBitsToFloat(velk_cluster_words.data[p0 + 2u]));
                vec3 e1 = vec3(uintBitsToFloat(velk_cluster_words.data[p1]), uintBitsToFloat(velk_cluster_words.data[p1 + 1u]),
                               uintBitsToFloat(velk_cluster_words.data[p1 + 2u])) - v0;
                vec3 e2 = vec3(uintBitsToFloat(velk_cluster_words.data[p2]), uintBitsToFloat(velk_cluster_words.data[p2 + 1u]),
                               uintBitsToFloat(velk_cluster_words.data[p2 + 2u])) - v0;
                vec3 p = cross(ld, e2);
                float det = dot(e1, p);
                if (abs(det) < 1e-7 * max(length(e1) * length(p), 1e-30)) continue;
                float inv_det = 1.0 / det;
                vec3 to_v0 = ray.origin - v0;
                float u = dot(to_v0, p) * inv_det;
                if (u < 0.0 || u > 1.0) continue;
                vec3 q = cross(to_v0, e1);
                float v = dot(ld, q) * inv_det;
                if (v < 0.0 || u + v > 1.0) continue;
                float tt = dot(e2, q) * inv_det;
                if (tt >= 1e-4 * max(length(e1), length(e2)) && tt < limit) return 0.0;
            }
        } else {
            for (uint c = 0u; c < node.child_count; ++c) {
                if (sp < 32) stack[sp++] = node.first_child + c;
            }
        }
    }
    return visibility;
}

// How much of a wide ray gets through a record's shapes.
float velk_record_visibility(Ray ray, uint record, float t_max)
{
    RtRecord rec = velk_records.data[record];
    if ((rec.flags & 2u) == 0u || rec.tree_root != VELK_NO_TREE) {
        RayHit h;
        return velk_intersect_record(ray, record, 0.0, t_max, true, h) ? 0.0 : 1.0;
    }
    Ray local = velk_local_ray(ray, rec);
    float visibility = 1.0;
    for (uint i = 0u; i < rec.shape_count; ++i) {
        Ray shape_ray = local;
#ifdef VELK_RAY_MOTION
        shape_ray = velk_moved_ray(local, rec, rec.first_shape + i);
#endif
        visibility *= velk_mesh_visibility(shape_ray, velk_bvh_shapes.data[rec.first_shape + i], t_max);
    }
    return visibility;
}

)"
                                                                      R"(
float trace_visibility_medium(Ray ray, float t_max)
{
    float visibility = 1.0;
    uint stack[VELK_TRACE_STACK_SIZE];
    int sp = 0;
    stack[sp++] = VELK_GLOBALS.bvh_root;
    vec3 world_origin = ray.origin;
    int partition_sp = -1;
    while (sp > 0) {
        if (sp == partition_sp) {
            ray.origin = world_origin;
            partition_sp = -1;
        }
        uint ni = stack[--sp];
        BvhNode node = velk_bvh_nodes.data[ni];
        float t_hit;
        if (!ray_aabb(ray, node.aabb_min.xyz, node.aabb_max.xyz, t_max, t_hit)) continue;
        if (node.child_count == VELK_PARTITION_REF) {
            partition_sp = sp;
            ray.origin -= velk_partition_origin(node);
            ni = node.first_child;
            node = velk_bvh_nodes.data[ni];
        }
        if (node.aabb_max.w > 0.0) {
            vec3 extent = node.aabb_max.xyz - node.aabb_min.xyz;
            float widest = max(max(extent.x, extent.y), extent.z);
            // The leaf of a partition without its instances has nothing else to offer.
            float width = velk_ray_footprint + velk_ray_spread * max(t_hit, 0.0);
            if (widest < width * velk_medium_reach(ni) || (node.child_count == 0u && node.shape_count == 0u)) {
                float enter;
                if (ray_aabb(ray, node.aabb_min.xyz, node.aabb_max.xyz, t_max, enter)) {
                    vec3 sides = max(extent, vec3(widest * 1e-3));
                    float len = max(min(ray_aabb_exit(ray, node.aabb_min.xyz, node.aabb_max.xyz), t_max) - enter, 0.0);
                    visibility *= exp(-node.aabb_max.w / (sides.x * sides.y * sides.z) * len);
                }
                continue;
            }
        }
        for (uint i = 0u; i < node.shape_count; ++i) {
            visibility *= velk_record_visibility(ray, node.first_shape + i, t_max);
            if (visibility <= 0.0) {
                return 0.0;
            }
        }
        for (uint i = 0u; i < node.child_count; ++i) {
            if (sp < VELK_TRACE_STACK_SIZE) stack[sp++] = node.first_child + i;
        }
    }
    return visibility;
}

bool trace_closest_hit_medium(Ray ray, float t_min, float t_max, inout RayHit hit)
{
    uint stack[VELK_TRACE_STACK_SIZE];
    int sp = 0;
    stack[sp++] = VELK_GLOBALS.bvh_root;
    vec3 world_origin = ray.origin;
    int partition_sp = -1;
    while (sp > 0) {
        if (sp == partition_sp) {
            ray.origin = world_origin;
            partition_sp = -1;
        }
        uint ni = stack[--sp];
        BvhNode node = velk_bvh_nodes.data[ni];
        float t_enter;
        if (!ray_aabb(ray, node.aabb_min.xyz, node.aabb_max.xyz, hit.t, t_enter)) continue;
        if (node.child_count == VELK_PARTITION_REF) {
            partition_sp = sp;
            ray.origin -= velk_partition_origin(node);
            ni = node.first_child;
            node = velk_bvh_nodes.data[ni];
        }
        if (node.aabb_max.w > 0.0) {
            vec3 extent = node.aabb_max.xyz - node.aabb_min.xyz;
            float widest = max(max(extent.x, extent.y), extent.z);
            if (widest < velk_ray_footprint * velk_medium_reach(ni) || (node.child_count == 0u && node.shape_count == 0u)) {
                float enter;
                if (ray_aabb(ray, node.aabb_min.xyz, node.aabb_max.xyz, hit.t, enter)) {
                    vec3 sides = max(extent, vec3(widest * 1e-3));
                    float leave = min(ray_aabb_exit(ray, node.aabb_min.xyz, node.aabb_max.xyz), hit.t);
                    float depth = node.aabb_max.w / (sides.x * sides.y * sides.z) * max(leave - max(enter, t_min), 0.0);
                    velk_ray_medium_depth += depth;
                    velk_ray_medium_color += depth * unpackUnorm4x8(floatBitsToUint(node.aabb_min.w)).rgb;
                }
                continue;
            }
        }
        velk_ray_mesh_color = unpackUnorm4x8(floatBitsToUint(node.aabb_min.w)).rgb;
        velk_ray_medium_near = t_min;
        for (uint i = 0u; i < node.shape_count; ++i) {
            RayHit h;
            velk_ray_medium_far = hit.t;
            if (velk_intersect_record(ray, node.first_shape + i, t_min, hit.t, false, h)) {
                hit = h;
            }
        }
        for (uint i = 0u; i < node.child_count; ++i) {
            if (sp < VELK_TRACE_STACK_SIZE) stack[sp++] = node.first_child + i;
        }
    }
    return hit.shape_index != 0xffffffffu;
}
#endif
)"
                                                                      R"(
// Visibility traversal for shadow rays: 0 on the first confirmed blocker within t_max,
// otherwise how much light gets through what is too fine for the ray to resolve. With
// VELK_TRACE_ORDERED_ANY_HIT it goes front-to-back, since the near child is the likelier
// blocker, at the cost of testing both children before descending.
float trace_visibility(Ray ray, float t_max)
{
    if (VELK_GLOBALS.bvh_node_count == 0u) return 1.0;
#ifdef VELK_TRACE_MEDIUM
    if (velk_ray_footprint > 0.0) return trace_visibility_medium(ray, t_max);
#endif
    uint stack[VELK_TRACE_STACK_SIZE];
    int sp = 0;
    stack[sp++] = VELK_GLOBALS.bvh_root;
    vec3 world_origin = ray.origin;
    int partition_sp = -1;
    while (sp > 0) {
        if (sp == partition_sp) {
            ray.origin = world_origin;
            partition_sp = -1;
        }
        uint ni = stack[--sp];
        BvhNode node = velk_bvh_nodes.data[ni];
        float t_hit;
        if (!ray_aabb(ray, node.aabb_min.xyz, node.aabb_max.xyz, t_max, t_hit)) continue;

        // The reference's box already covers the partition's root, so the root needs
        // no test of its own.
        if (node.child_count == VELK_PARTITION_REF) {
            partition_sp = sp;
            ray.origin -= velk_partition_origin(node);
            node = velk_bvh_nodes.data[node.first_child];
        }


        for (uint i = 0u; i < node.shape_count; ++i) {
            RayHit h;
            if (velk_intersect_record(ray, node.first_shape + i, 0.0, t_max, true, h)) {
                return 0.0;
            }
        }

#ifdef VELK_TRACE_ORDERED_ANY_HIT
        if (node.child_count == 2u) {
            BvhNode l = velk_bvh_nodes.data[node.first_child];
            BvhNode r = velk_bvh_nodes.data[node.first_child + 1u];
            float t_l, t_r;
            bool h_l = ray_aabb(ray, l.aabb_min.xyz, l.aabb_max.xyz, t_max, t_l);
            bool h_r = ray_aabb(ray, r.aabb_min.xyz, r.aabb_max.xyz, t_max, t_r);
            if (h_l && h_r) {
                if (t_l <= t_r) {
                    if (sp < VELK_TRACE_STACK_SIZE) stack[sp++] = node.first_child + 1u;
                    if (sp < VELK_TRACE_STACK_SIZE) stack[sp++] = node.first_child;
                } else {
                    if (sp < VELK_TRACE_STACK_SIZE) stack[sp++] = node.first_child;
                    if (sp < VELK_TRACE_STACK_SIZE) stack[sp++] = node.first_child + 1u;
                }
            } else if (h_l) {
                if (sp < VELK_TRACE_STACK_SIZE) stack[sp++] = node.first_child;
            } else if (h_r) {
                if (sp < VELK_TRACE_STACK_SIZE) stack[sp++] = node.first_child + 1u;
            }
            continue;
        }
#endif
        for (uint i = 0u; i < node.child_count; ++i) {
            if (sp < VELK_TRACE_STACK_SIZE) stack[sp++] = node.first_child + i;
        }
    }
    return 1.0;
}

// Whether anything blocks the ray within t_max.
bool trace_any_hit(Ray ray, float t_max)
{
    return trace_visibility(ray, t_max) <= 0.0;
}

// Closest-hit traversal bounded to the interval [t_min, t_max). A probe cascade
// traces one distance shell per level, and that bound is what keeps a dense near
// level affordable: the cost of a ray is dominated by how much of the tree it has
// to walk. `hit.shape_index` indexes velk_bvh_shapes.
//
// Nodes are culled against the FAR bound only. `ray_aabb` reports the entry
// distance and not the exit, so a node lying entirely nearer than `t_min` cannot
// be rejected outright; the near bound is applied per shape instead. That costs
// nothing for a level starting at 0 and leaves some traversal on the table for
// levels that do not.
bool trace_closest_hit(Ray ray, float t_min, float t_max, out RayHit hit)
{
    hit.t = t_max;
    hit.shape_index = 0xffffffffu;
    if (VELK_GLOBALS.bvh_node_count == 0u) return false;
#ifdef VELK_TRACE_MEDIUM
    if (velk_ray_footprint > 0.0) return trace_closest_hit_medium(ray, t_min, t_max, hit);
#endif
    uint stack[VELK_TRACE_STACK_SIZE];
    int sp = 0;
    stack[sp++] = VELK_GLOBALS.bvh_root;
    vec3 world_origin = ray.origin;
    int partition_sp = -1;
    while (sp > 0) {
        if (sp == partition_sp) {
            ray.origin = world_origin;
            partition_sp = -1;
        }
        uint ni = stack[--sp];
        BvhNode node = velk_bvh_nodes.data[ni];
        float t_enter;
        if (!ray_aabb(ray, node.aabb_min.xyz, node.aabb_max.xyz, hit.t, t_enter)) continue;
        if (node.child_count == VELK_PARTITION_REF) {
            partition_sp = sp;
            ray.origin -= velk_partition_origin(node);
            node = velk_bvh_nodes.data[node.first_child];
        }
        for (uint i = 0u; i < node.shape_count; ++i) {
            RayHit h;
            if (velk_intersect_record(ray, node.first_shape + i, t_min, hit.t, false, h)) {
                hit = h;
            }
        }
        for (uint i = 0u; i < node.child_count; ++i) {
            if (sp < VELK_TRACE_STACK_SIZE) stack[sp++] = node.first_child + i;
        }
    }
    return hit.shape_index != 0xffffffffu;
}

// Unbounded closest hit. On a miss `hit.t` stays at 1e30, which callers that
// shade the miss rely on.
bool trace_closest_hit(Ray ray, out RayHit hit)
{
    return trace_closest_hit(ray, 0.0, 1e30, hit);
}
)";

/// @p source with the medium traversals of velk_trace_src compiled in, for a scene with
/// instances that have a far form.
inline string velk_with_trace_medium(string_view source)
{
    const size_t version = source.find(string_view("#version"));
    const size_t after = source.find('\n', version) + 1;
    string out;
    out += source.substr(0, after);
    out += string_view("#define VELK_TRACE_MEDIUM\n");
    out += source.substr(after);
    return out;
}

/// @p source with a scene's ray motion compiled in. See IInstancedMeshVisual::set_ray_motion.
inline string velk_with_ray_motion(string_view source, string_view motion, float reach)
{
    const size_t version = source.find(string_view("#version"));
    const size_t after = source.find('\n', version) + 1;
    string out;
    out += source.substr(0, after);
    out += string_view("#define VELK_RAY_MOTION(p, at, t) (");
    out += motion;
    char buffer[64];
    const int length = std::snprintf(buffer, sizeof(buffer), ")\n#define VELK_RAY_MOTION_REACH %f\n", double(reach));
    out += string_view(buffer, static_cast<size_t>(length));
    out += source.substr(after);
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
[[maybe_unused]] constexpr string_view deferred_compute_prelude_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 1, rgba8)   uniform writeonly image2D gStorageImages[];
layout(set = 0, binding = 2, rgba32f) uniform writeonly image2D gStorageImagesF32[];
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

// Scene TLAS, bound by index (set = 1) instead of chased through a
// buffer_device_address in globals. The renderer stamps the same
// scene-wide BVH buffers into set_global_buffer each frame. Node and record
// indices are absolute in these buffers.
layout(set = 1, binding = 0, std430) readonly buffer VelkBvhNodes { BvhNode data[]; } velk_bvh_nodes;
layout(set = 1, binding = 1, std430) readonly buffer VelkBvhShapes { RtShape data[]; } velk_bvh_shapes;
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

// Records placing shapes in the world (set = 1 slot 10). Same buffer the RT
// compute reads.
layout(set = 1, binding = 10, std430) readonly buffer VelkRecords { RtRecord data[]; } velk_records;

// Per-primitive RT geometry metadata (set = 1 slot 11), read via
// velk_mesh_static(shape), plus the BLAS runs it points at (slots 12 / 13).
// A primitive's runs are allocated once at load and never move.
layout(set = 1, binding = 11, std430) readonly buffer VelkMeshStatic { MeshStaticData data[]; } velk_mesh_static_records;
layout(set = 1, binding = 12, std430) readonly buffer VelkBlasNodes { BvhNode data[]; } velk_blas_nodes;
layout(set = 1, binding = 13, std430) readonly buffer VelkClusterWords { uint data[]; } velk_cluster_words;

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
    uint irr_image_id;         // 88  visibility RATIO out (denoised downstream)
    uint unshadowed_image_id;  // 92  unshadowed diffuse out (sharp; composite multiplies)
    uint ray_pass;             // 96  1: traces at half size, writing visibility only. 0: shades every pixel
    uint gi_probe_id;          // 100 GI probe atlas (SH L1); 0 = no GI
    uint gi_probe_dims;        // 104 probe grid, 10 bits per axis
    uint light_loop_probe;     // 108 diagnostic: extra per-light ALU repeats
} pc;

#include "velk_intersect.glsl"

#define VELK_TRACE_STACK_SIZE 96
#define VELK_TRACE_ORDERED_ANY_HIT
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
// Indirect diffuse from the GI probe field (G1, cascade 0). Reconstructs
// irradiance from the surrounding probes' SH L1 against this surface's normal.
//
// The eight probes around the shading point are weighted trilinearly, then by
// how far each one lies in the shading normal's hemisphere. That second term is
// what keeps light from crossing thin geometry: a probe on the far side of a
// wall sits BEHIND the surface being shaded, so its weight collapses and the
// probes on the near side carry the result. It cannot catch an occluder
// standing between a probe and the shading point while the probe is still in
// front (under a table, around a corner); that needs per-probe directional
// depth.
vec3 velk_gi_indirect(vec3 world_pos, vec3 N)
{
    if (pc.gi_probe_id == 0u || VELK_GLOBALS.bvh_node_count == 0u) return vec3(0.0);
    uvec3 dims = uvec3(pc.gi_probe_dims & 1023u,
                       (pc.gi_probe_dims >> 10) & 1023u,
                       (pc.gi_probe_dims >> 20) & 1023u);
    if (dims.x == 0u || dims.y == 0u || dims.z == 0u) return vec3(0.0);

    // The same lattice the probe pass builds. Probe i sits at the CENTRE of cell
    // i, so lattice coordinates run half a cell behind the normalised position.
    vec3 gmin = VELK_GLOBALS.probe_min;
    vec3 gext = max(VELK_GLOBALS.probe_max - gmin, vec3(1e-6));
    vec3 cell = gext / vec3(dims);

    // Lifting the sample point off the surface keeps a probe sitting almost in
    // the surface's plane from landing on either side of it by chance. Scaled
    // by the lattice, so it follows whatever size the scene is.
    vec3 shade_pos = world_pos + N * (0.05 * min(cell.x, min(cell.y, cell.z)));

    vec3 g = (shade_pos - gmin) / gext * vec3(dims) - 0.5;
    vec3 gbase = floor(g);
    vec3 gfrac = clamp(g - gbase, vec3(0.0), vec3(1.0));
    ivec3 base = ivec3(gbase);
    ivec3 last = ivec3(dims) - ivec3(1);

    uint atlas_w = dims.x * dims.z;
    uint atlas_h = dims.y * 4u;
    vec2 atlas_rcp = 1.0 / vec2(float(atlas_w), float(atlas_h));

    vec3 sh[4];
    for (int c = 0; c < 4; ++c) sh[c] = vec3(0.0);
    float total = 0.0;

    for (int o = 0; o < 8; ++o) {
        ivec3 off = ivec3(o & 1, (o >> 1) & 1, (o >> 2) & 1);
        ivec3 pi = clamp(base + off, ivec3(0), last);

        vec3 tri = mix(vec3(1.0) - gfrac, gfrac, vec3(off));
        float w = tri.x * tri.y * tri.z;

        // Plain trilinear, with NO hemisphere weighting. Weighting each probe
        // by how far it lies in the shading normal's hemisphere is the obvious
        // way to stop light crossing thin geometry, and it does not work on its
        // own: it reweights the probe layer in front of a surface against the
        // one behind it by an amount that depends on where the shading point
        // sits INSIDE its cell, and those layers hold very different radiance,
        // so the mix varies across every cell and draws the lattice. Measured
        // at floors of both 0.001 and 0.2 - the floor rescales the mix, it does
        // not remove it. A smoothly varying visibility term (per-probe depth,
        // tested against probe-to-point distance) is what rejects the wrong
        // side without keying on intra-cell geometry.
        if (w <= 0.0) continue;

        for (int c = 0; c < 4; ++c) {
            vec2 uv = (vec2(float(pi.x + pi.z * int(dims.x)),
                            float(pi.y + c * int(dims.y))) + 0.5) * atlas_rcp;
            sh[c] += velk_texture(pc.gi_probe_id, uv).rgb * w;
        }
        total += w;
    }

    if (total <= 0.0) return vec3(0.0);
    float inv_total = 1.0 / total;
    for (int c = 0; c < 4; ++c) sh[c] *= inv_total;

    // Lambertian convolution of an L1 radiance SH: A0*Y00*L00 + A1*Y1m*L1m,
    // with A0 = pi and A1 = 2pi/3 - then divided by pi.
    //
    // The 1/pi is a convention match, not physics. This renderer omits it
    // everywhere: direct light is albedo * N.L * radiance rather than
    // albedo * E / pi, and the env stand-in fed a raw radiance sample in as
    // irradiance. True irradiance from a uniform sky of radiance R is pi*R, so
    // returning E unscaled makes GI 3.14x brighter than everything it sits
    // beside, which reads as a flat over-lit wash.
    vec3 e = 0.282095 * sh[0]
           + 0.325735 * (sh[1] * N.y + sh[2] * N.z + sh[3] * N.x);
    return max(e, vec3(0.0));
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
        vec3 partition_origin = vec3(0.0);
        int partition_sp = -1;
        // Stops early once the light is fully blocked: nothing further can
        // change the result, and this is the common case deep in a shadow.
        // BOTH terms have to be saturated, since the specular lobe can still
        // see a sliver of emitter after the diffuse hemisphere is covered.
        while (sp > 0 && (f_occ < f_max || s_occ < s_max)) {
            if (sp == partition_sp) {
                partition_origin = vec3(0.0);
                partition_sp = -1;
            }
            uint ni = stack[--sp];
            BvhNode node = velk_bvh_nodes.data[ni];

            vec3 bc = 0.5 * (node.aabb_min.xyz + node.aabb_max.xyz) + partition_origin;
            vec3 be = 0.5 * (node.aabb_max.xyz - node.aabb_min.xyz);
            bool outside = false;
            for (int p = 0; p < 6; ++p) {
                if (dot(cull_n[p], bc - cull_p[p]) + dot(abs(cull_n[p]), be) < 0.0) {
                    outside = true;
                    break;
                }
            }
            if (outside) continue;

            if (node.child_count == VELK_PARTITION_REF) {
                partition_sp = sp;
                partition_origin = velk_partition_origin(node);
                node = velk_bvh_nodes.data[node.first_child];
            }

            for (uint ri = 0u; ri < node.shape_count; ++ri) {
              RtRecord rec = velk_records.data[node.first_shape + ri];
              // Records hold the inverse; silhouettes need world-space corners.
              mat3 to_world = inverse(transpose(mat3(rec.inv_row0.xyz, rec.inv_row1.xyz, rec.inv_row2.xyz)));
              vec3 translation = vec3(rec.inv_row0.w, rec.inv_row1.w, rec.inv_row2.w) + partition_origin;
              for (uint i = 0u; i < rec.shape_count; ++i) {
                RtShape s = velk_bvh_shapes.data[rec.first_shape + i];
                if (s.shape_kind == 255u) continue;  // meshes: no silhouette yet

                // Shape geometry, relative to the shading point. Origin is a
                // corner and the axes span the shape, matching what the
                // intersectors above assume.
                vec3 o = to_world * s.origin.xyz + translation - shade_pos;
                vec3 u = to_world * s.u_axis.xyz;
                vec3 v = to_world * s.v_axis.xyz;
                vec3 w = to_world * s.w_axis.xyz;
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
void main()
{
    // Rays run as a dispatch of their own over one pixel in each 2x2 block, since a thread
    // group lasts as long as its longest ray: sparse rays among all pixels would hold up
    // every group.
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy) * (pc.ray_pass != 0u ? 2 : 1);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;

    vec2 uv = (vec2(coord) + 0.5) / vec2(float(pc.width), float(pc.height));
    vec4 albedo    = velk_texture(pc.albedo_tex_id, uv);
    vec4 normal_and_error = velk_texture(pc.normal_tex_id, uv);
    vec3 world_n   = normal_and_error.xyz;
    float lod_error = normal_and_error.w;
    vec3 world_pos = velk_texture(pc.worldpos_tex_id, uv).xyz;
    vec4 mat       = velk_texture(pc.material_tex_id, uv);

    // Sky path: pixels with no G-buffer coverage (cleared to zero,
    // including normal) reconstruct a world-space view ray and sample
    // the environment. Falls back to black when the view has no env.
    if (dot(world_n, world_n) < 1e-6) {
        vec2 ndc = uv * 2.0 - 1.0;
        mat4 inv_vp = VELK_GLOBALS.inverse_view_projection;
        vec4 near_h = inv_vp * vec4(ndc, 1.0, 1.0);
        vec4 far_h  = inv_vp * vec4(ndc, 0.0, 1.0);
        vec3 near_w = near_h.xyz / near_h.w;
        vec3 far_w  = far_h.xyz  / far_h.w;
        vec3 rd = normalize(far_w - near_w);
        vec3 sky = env_miss_color(rd);
        // Sky is sharp (no diffuse irradiance): write it to the "rest" image
        // and zero irradiance. The denoise/composite pass passes it through
        // (albedo == 0 for sky pixels, so it adds nothing).
        if (pc.ray_pass != 0u) {
            imageStore(gStorageImagesF16[nonuniformEXT(pc.irr_image_id)], coord, vec4(0.0));
        } else {
            imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(sky, 1.0));
        }
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
    // Unshadowed diffuse, paired with `irr` which now carries the visibility
    // ratio rather than radiance. Zero for unlit pixels, which zeroes the
    // product at composite regardless of what the ratio filtered to.
    vec3 unshadowed_out = vec3(0.0);
    float irr_valid = 1.0;
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
        // Per-pixel RIS stream. A tile-uniform stream was tried (Stage 0): it
        // makes neighbouring pixels pick the same light, which buys ray
        // coherence for free and did cut night from 31 to 25 ms - but it makes
        // the noise spatially correlated, which is precisely what a spatial
        // filter cannot remove. It showed as 8x8 blockiness while converging
        // and blur afterwards. Coherence has to come from reordering
        // execution, never from correlating the sampling.
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
        vec3 unshadowed_direct = vec3(0.0);
        // Diagnostic accumulator; see the probe loop inside the light loop.
        float loop_probe = 0.0;

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

            // Unshadowed diffuse, summed over ALL lights and free of noise.
            // The ratio estimator divides the stochastic estimate by this and
            // denoises the quotient instead of the radiance.
            unshadowed_direct += NdotL * radiance;

            // DIAGNOSTIC: repeat this light's ALU to price what clustered light
            // culling would remove. It ADDS iterations rather than removing
            // them, so the reservoir seed, the chosen light, the shadow ray and
            // the final image are all untouched and ray coherence cannot drift.
            // Capping the loop instead would change which light RIS picks, and
            // that confound has already wrecked two measurements in this scene.
            for (uint r = 0u; r < pc.light_loop_probe; ++r) {
                vec3 L2;
                float atten2 = 1.0;
                if (light.flags.x == 0u) {
                    L2 = -light.direction.xyz;
                } else {
                    vec3 tl2 = light.position.xyz - world_pos;
                    float d2 = length(tl2);
                    L2 = tl2 / max(d2, 1e-6);
                    float rg2 = max(light.params.x, 1e-6);
                    float tt2 = clamp(1.0 - d2 / rg2, 0.0, 1.0);
                    atten2 = tt2 * tt2;
                    if (light.flags.x == 2u) {
                        float ca2 = dot(-L2, light.direction.xyz);
                        atten2 *= smoothstep(light.params.z, light.params.y, ca2);
                    }
                }
                float ndl2 = max(dot(N, L2), 0.0);
                vec3 rad2 = light.color_intensity.rgb * light.color_intensity.a * atten2;
                vec3 H2 = normalize(L2 + V);
                float nh2 = max(dot(N, H2), 0.0);
                float vh2 = max(dot(V, H2), 0.0);
                vec3 sp2 = (ggx_d(nh2, a) * smith_g(NdotV, ndl2, roughness))
                         * fresnel_schlick(vh2, F0)
                         / max(4.0 * NdotV * ndl2, 1e-6) * ndl2 * rad2;
                loop_probe += sp2.r + sp2.g + sp2.b + ndl2;
            }

            // Reservoir step: weight = luminance of this light's unshadowed
            // diffuse contribution.
            vec3 dcontrib = albedo.rgb * NdotL * radiance;
            float w = dot(dcontrib, vec3(0.2126, 0.7152, 0.0722));
            total_w += w;
            // A full mix, not a linear step: the seed starts linear in the pixel
            // coordinates, so a linear step gives a value linear across the screen,
            // and comparing that to a probability draws diagonal stripes.
            seed ^= seed >> 16; seed *= 0x7feb352du;
            seed ^= seed >> 15; seed *= 0x846ca68bu;
            seed ^= seed >> 16;
            if (w > 0.0 && float(seed) * (1.0 / 4294967296.0) < w / total_w) {
                res_light = i;
                res_w = w;
                res_NdotL = NdotL;
                res_radiance = radiance;
            }
        }

        // Cannot fire: every term the probe loop accumulates is non-negative.
        // It exists so the loop has an observable result and is not optimised
        // away, without perturbing anything when the probe is off.
        if (loop_probe < -1.0) unshadowed_direct += vec3(1.0);

        // Shadowed diffuse IRRADIANCE estimate from the single chosen light
        // (one ray), demodulated (no albedo - reapplied at composite).
        // One pixel in each 2x2 block traces, always the same one, and the spatial pass fills
        // in the rest from its neighbours by geometry. The unshadowed term is still computed
        // for every pixel, so light falloff and N.L stay sharp; only visibility is sparse.
        bool trace_now = pc.ray_pass != 0u;

)"
                                                                      R"(
        // A pixel's width on the surface, wider when seen at a shallow angle.
        mat4 footprint_vp = VELK_GLOBALS.view_projection;
        float footprint_f = length(vec3(footprint_vp[0][1], footprint_vp[1][1], footprint_vp[2][1]));
        float pixel_width = length(world_pos - VELK_GLOBALS.cam_pos.xyz) * 2.0 / (footprint_f * float(pc.height))
            / sqrt(max(NdotV, 0.05));

        vec3 direct_diffuse = vec3(0.0);
        if (trace_now && res_light != 0xffffffffu && res_w > 0.0) {
            Light chosen = velk_lights.data[pc.lights_base + res_light];
            // It stands for its whole 2x2 block.
            velk_ray_footprint = pixel_width * 2.0;
            velk_ray_spread = chosen.flags.x == 0u ? 0.01 : 0.0;
            velk_ray_lift = lod_error;
            float shadow = velk_eval_shadow(chosen.flags.y, res_light, world_pos, N);
            velk_ray_footprint = 0.0;
            velk_ray_spread = 0.0;
            velk_ray_lift = 0.0;
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
        // Occlusion over less than a pixel's width cannot be resolved, so it is not traced.
        // The probe field replaces this term, so with probes bound no ray is traced.
        bool has_gi = pc.gi_probe_id != 0u;
        float ao = 1.0;
        const float ao_range = 0.3;
        if (trace_now && !has_gi && VELK_GLOBALS.bvh_node_count != 0u && ao_range > pixel_width) {
            Ray ao_r;
            ao_r.origin = world_pos + N * (0.01 + lod_error);
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
        // Ratio estimator (Heitz et al. 2018). What gets denoised is not the
        // radiance but the quotient of the shadowed estimate by the unshadowed
        // one. The quotient is visibility, which is low frequency and bounded,
        // so it survives filtering far better than radiance does: a filter
        // that would smear a bright light's falloff barely touches a shadow
        // ratio. The composite multiplies the sharp unshadowed term back in.
        //
        // The env term is folded into the same ratio rather than kept beside
        // it, so the binary AO ray's aliasing is denoised too.
        // Indirect bounce from the probe field. It joins the UNSHADOWED term
        // rather than the ratio: it is already a visibility-resolved gather
        // (the probe rays did the occlusion), so multiplying it by the
        // stochastic shadow ratio would darken it a second time.
        vec3 gi = velk_gi_indirect(world_pos, N);
        // The probe field REPLACES the env-diffuse stand-in rather than adding
        // to it. `env_diffuse` is one raw radiance sample along N used as
        // irradiance; the probes compute the same quantity properly, as a
        // cosine-weighted gather that already includes sky visibility. Adding
        // both counts the sky twice, which in a daylit scene washes everything
        // flat - and the probe field supersedes the AO term too, since its
        // occlusion is real rather than a single 0.3-unit ray.
        vec3 ambient_unshadowed = has_gi ? gi : env_diffuse;
        vec3 ambient_shadowed   = has_gi ? gi : env_diffuse * ao;
        unshadowed_out = unshadowed_direct + ambient_unshadowed;
        vec3 shadowed_total = direct_diffuse + ambient_shadowed;
        irr = shadowed_total / max(unshadowed_out, vec3(1e-4));
        // Alpha marks "this pixel traced a ray this frame". The temporal pass
        // keeps history untouched where it is 0 rather than folding in a zero.
        irr_valid = trace_now ? 1.0 : 0.0;
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
    if (pc.ray_pass != 0u) {
        imageStore(gStorageImagesF16[nonuniformEXT(pc.irr_image_id)], coord, vec4(irr, irr_valid));
        return;
    }
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(rest, albedo.a));
    // The unshadowed term the composite multiplies the denoised ratio by. Kept
    // out of the denoiser deliberately: it is exact and sharp, and it carries
    // the high-frequency detail (light falloff, N.L) that must not be blurred.
    imageStore(gStorageImagesF16[nonuniformEXT(pc.unshadowed_image_id)], coord,
               vec4(unshadowed_out, 1.0));
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
    vec4 cur_s     = velk_texture(pc.irr_id, uv);
    vec3 cur_irr   = cur_s.rgb;
    // Alpha 0 means the lighting pass skipped this pixel's ray this frame
    // (half-rate shadows). There is nothing to fold in, and folding in the
    // zero would darken it, so history passes through untouched and the
    // sample count does not advance.
    bool has_sample = cur_s.a > 0.5;
    vec3 world_pos = velk_texture(pc.worldpos_id, uv).xyz;
    float cur_lum  = dot(cur_irr, LUMA);

    // Temporal reprojection: where was this world point last frame?
    vec3 irr_acc = has_sample ? cur_irr : vec3(0.0);
    float count = has_sample ? 1.0 : 0.0;
    float m2_acc = has_sample ? cur_lum * cur_lum : 0.0;
    const bool kAccumulate = false;
    if (kAccumulate && pc.reset == 0u) {
        vec4 prev_clip = VELK_GLOBALS.prev_view_projection * vec4(world_pos, 1.0);
        if (prev_clip.w > 1e-6) {
            vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
            if (all(greaterThanEqual(prev_uv, vec2(0.0))) &&
                all(lessThanEqual(prev_uv, vec2(1.0)))) {
                vec4 hp = velk_texture(pc.hist_pos_prev_id, prev_uv);
                vec4 hi = velk_texture(pc.hist_irr_prev_id, prev_uv);
                // Same surface? World-space distance, tolerance scaled by depth.
                if (hp.w > 0.5 && length(hp.xyz - world_pos) < 0.03 * max(prev_clip.w, 1.0)) {
                    float m2_prev = velk_texture(pc.hist_mom_prev_id, prev_uv).r;
                    if (has_sample) {
                        // Fixed accumulation window. Shorter = shorter dynamic-
                        // occluder trail, slightly noisier static; the spatial pass'
                        // variance-guided filter cleans the residual. Tunable; the
                        // spatial pass treats this count as converged.
                        count = min(hi.a + 1.0, 8.0);
                        float w = 1.0 / count;
                        irr_acc = mix(hi.rgb, cur_irr, w);
                        m2_acc = mix(m2_prev, cur_lum * cur_lum, w);
                    } else {
                        irr_acc = hi.rgb;
                        count = hi.a;
                        m2_acc = m2_prev;
                    }
                }
            }
        }
    }

    imageStore(gStorageImagesF16[nonuniformEXT(pc.hist_irr_cur_id)], coord, vec4(irr_acc, count));
    imageStore(gStorageImagesF16[nonuniformEXT(pc.hist_pos_cur_id)], coord, vec4(world_pos, 1.0));
    imageStore(gStorageImagesF16[nonuniformEXT(pc.hist_mom_cur_id)], coord, vec4(m2_acc, 0.0, 0.0, 0.0));
}
)";

// Spatial filter + composite. Fills every pixel's visibility from the pixels that traced a
// ray, one in each 2x2 block, over a normal + plane-distance weighted neighbourhood. Then
// composites the final image (albedo*(1-metallic)*filtered + sharp rest, read back in place
// from the output) for the surface blit.
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
    uint hist_irr_id;          // accumulated visibility RATIO + count (sampled, neighborhood)
    uint hist_mom_id;          // accumulated luminance 2nd moment (.r)
    uint output_id;            // deferred_output: "rest" on read, final on write
    uint width;
    uint height;
    uint unshadowed_id;        // sharp unshadowed diffuse; multiplies the filtered ratio
    uint probe_grid;           // diagnostic: reconstruct the ratio from an
                               // N-pixel screen grid; 0 or 1 = per pixel
} pc;

// One filter tap set, centred on `coord` and using THAT pixel's own geometry
// as the edge-stop reference. Factored out of main so the screen-grid
// diagnostic below can run it at grid points instead of only at the shading
// pixel: a probe sitting there would see its own surface, not the caller's.
vec3 velk_filter_at(ivec2 coord, vec2 dims)
{
    vec2 uv = (vec2(coord) + 0.5) / dims;
    vec3 n0 = velk_texture(pc.normal_id, uv).xyz;
    if (dot(n0, n0) < 1e-6) return vec3(0.0);

    vec3 N         = normalize(n0);
    vec3 world_pos = velk_texture(pc.worldpos_id, uv).xyz;

    float depth = max(length(VELK_GLOBALS.cam_pos.xyz - world_pos), 1e-3);

    // Only pixels that traced a ray count, which are one in each 2x2 block, by geometry.
    // A surface unlike all of them nearby, such as a leaf, takes their plain mean.
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    vec3 any_sum = vec3(0.0);
    float any_wsum = 0.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            ivec2 c2 = coord + ivec2(i, j);
            if (c2.x < 0 || c2.y < 0 || c2.x >= int(pc.width) || c2.y >= int(pc.height)) continue;
            if (((c2.x | c2.y) & 1) != 0) continue;
            vec2 uv2 = (vec2(c2) + 0.5) / dims;
            vec3 n2 = velk_texture(pc.normal_id, uv2).xyz;
            if (dot(n2, n2) < 1e-6) continue; // sky neighbour
            vec3 p2  = velk_texture(pc.worldpos_id, uv2).xyz;
            vec3 ir2 = velk_texture(pc.hist_irr_id, uv2).rgb;
            float wn = pow(max(dot(N, normalize(n2)), 0.0), 32.0);
            float wp = exp(-abs(dot(p2 - world_pos, N)) / (0.05 * depth));
            float wk = exp(-float(i * i + j * j) * 0.25);
            float w = wn * wp * wk;
            sum += ir2 * w;
            wsum += w;
            any_sum += ir2 * wk;
            any_wsum += wk;
        }
    }
    if (wsum > 1e-5) return sum / wsum;
    return any_wsum > 0.0 ? any_sum / any_wsum : vec3(1.0);
}
)"
                                                                      R"(
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

    vec3 N         = normalize(world_n);
    vec3 world_pos = velk_texture(pc.worldpos_id, uv).xyz;
    vec4 albedo    = velk_texture(pc.albedo_id, uv);
    float metallic = clamp(velk_texture(pc.material_id, uv).r, 0.0, 1.0);

    vec3 irr_out;
    if (pc.probe_grid > 1u) {
        // DIAGNOSTIC, not a shipping path. Asks what a screen-space probe
        // cascade could deliver, without building one: rebuild the visibility
        // RATIO from probes on an N-pixel grid while falloff and N.L stay per
        // pixel, which is the same split a cascade would make and needs no new
        // rays. If the image holds, probe density at this spacing is enough.
        //
        // Each grid point runs the FULL filter, which is what an earlier run of
        // this test got wrong: it interpolated the filter's OUTPUT, so it also
        // deleted spatial filtering and then read the resulting slow
        // convergence as a density result.
        //
        // Read distant SHARP shadows (building edges, awnings, railings), not
        // contact shadows. Near occluders are the case this handles well;
        // bistro's 2-degree sun and 0.3-unit bulbs stay sharp at any distance
        // and are the case that would kill it. Cost here means nothing: five
        // filter evaluations per pixel is the diagnostic's price, not the
        // technique's.
        int g = int(pc.probe_grid);
        ivec2 g0 = (coord / g) * g;
        vec2 f = vec2(coord - g0) / float(g);
        float depth = max(length(VELK_GLOBALS.cam_pos.xyz - world_pos), 1e-3);
        vec3 acc = vec3(0.0);
        float aw = 0.0;
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                ivec2 gc = min(g0 + ivec2(i, j) * g,
                               ivec2(int(pc.width) - 1, int(pc.height) - 1));
                vec2 guv = (vec2(gc) + 0.5) / dims;
                vec3 gn = velk_texture(pc.normal_id, guv).xyz;
                if (dot(gn, gn) < 1e-6) continue;
                vec3 gp = velk_texture(pc.worldpos_id, guv).xyz;
                // Bilinear, then gated on geometry. Without the gate a probe on
                // the far side of a silhouette bleeds across it and the test
                // would report edge halos that a real implementation rejects,
                // rather than the resolution question being asked.
                float bw = ((i == 0) ? (1.0 - f.x) : f.x)
                         * ((j == 0) ? (1.0 - f.y) : f.y);
                bw *= pow(max(dot(N, normalize(gn)), 0.0), 32.0);
                bw *= exp(-abs(dot(gp - world_pos, N)) / (0.05 * depth));
                if (bw <= 1e-6) continue;
                acc += velk_filter_at(gc, dims) * bw;
                aw += bw;
            }
        }
        // All four rejected: fall back to this pixel, as a real implementation
        // would have to.
        irr_out = (aw > 1e-5) ? (acc / aw) : velk_filter_at(coord, dims);
    } else {
        irr_out = velk_filter_at(coord, dims);
    }

    // irr_out is the filtered VISIBILITY RATIO; the radiance it scales is read
    // sharp and unfiltered here, so light falloff and N.L keep their detail
    // and only the visibility carries the filter's blur.
    vec3 unshadowed = velk_texture(pc.unshadowed_id, uv).rgb;
    vec3 final = albedo.rgb * (1.0 - metallic) * (unshadowed * irr_out) + rest.rgb;
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
// Float storage, for targets carrying HDR rather than display values.
layout(set = 0, binding = 2, rgba32f) uniform writeonly image2D gStorageImagesF32[];

// Scene TLAS bound by index (set = 1); same buffers as the deferred
// pass. Replaces the RtRoot BDA fields bvh_nodes / bvh_shapes for the
// array reads (bvh_root / bvh_node_count scalars still come from RtRoot).
layout(set = 1, binding = 0, std430) readonly buffer VelkBvhNodes { BvhNode data[]; } velk_bvh_nodes;
layout(set = 1, binding = 1, std430) readonly buffer VelkBvhShapes { RtShape data[]; } velk_bvh_shapes;
// The rest of the per-view frame state (inv-VP, cam, bvh root and count,
// present_counter) comes from the bound FrameGlobals record, indexed by
// pc.globals_base. See VELK_GLOBALS below.

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

// Records placing shapes in the world (set = 1 slot 10). Same buffer the
// deferred pass reads.
layout(set = 1, binding = 10, std430) readonly buffer VelkRecords { RtRecord data[]; } velk_records;

// Per-primitive RT geometry metadata (set = 1 slot 11), read via
// velk_mesh_static(shape). A primitive's record is allocated once at load and
// never moves.
layout(set = 1, binding = 11, std430) readonly buffer VelkMeshStatic { MeshStaticData data[]; } velk_mesh_static_records;
// Per-mesh BLAS, same buffers the deferred prelude reads.
layout(set = 1, binding = 12, std430) readonly buffer VelkBlasNodes { BvhNode data[]; } velk_blas_nodes;
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

// Wide scene roots (a grid of 40+ tiles, a dashboard of cards) need more than the
// deferred pass's 64, since closest-hit pushes both children unordered.
#define VELK_TRACE_STACK_SIZE 128
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
// then apply. The two copies of intersect_mesh in this file are what that rule
// exists to prevent.
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
    // what primary RT rays want. A GI probe sets one index and the reciprocal
    // of the probability it was chosen with: probe hits accumulate over many
    // frames, so one light per hit converges to the same answer while keeping
    // the RAY count per hit independent of how many lights the scene has.
    // Shading every probe hit against all of them would be ~21 shadow rays per
    // probe ray in bistro, which is what makes the obvious implementation
    // unaffordable.
    uint li_begin = (velk_single_light >= 0) ? uint(velk_single_light) : 0u;
    uint li_end   = (velk_single_light >= 0) ? uint(velk_single_light) + 1u : pc.light_count;
    float li_weight = (velk_single_light >= 0) ? velk_single_light_weight : 1.0;
    for (uint li = li_begin; li < li_end; ++li) {
        Light light = velk_lights.data[pc.lights_base + li];
        vec3 L;
        float atten;
        velk_light_sample(light, ctx.hit_pos, L, atten);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0 || atten <= 0.0) continue;
        float shadow = velk_eval_shadow(light.flags.y, li, ctx.hit_pos, N);
        vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * atten * shadow;
        direct += base * (1.0 - metallic) * NdotL * radiance * li_weight;
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
        RtShape s = velk_bvh_shapes.data[hit.shape_index];
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
        vec4 near_h = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 1.0, 1.0);
        vec4 far_h  = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 0.0, 1.0);
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
            RtRecord rec = velk_records.data[s.primary_record];
            RayHit hit;
            if (!intersect_shape(velk_local_ray(primary, rec), s, hit)) continue;
            hit.normal = velk_world_normal(rec, hit.normal);

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

// World-space GI probes. Composed with the RT body, so probe hits get the same
// material evaluation primary rays do - real albedo and emission rather than a
// vertex-tint approximation.
//
// One entry point serves every cascade level; the push constants say which.
// `env.z` gives this level's grid, `extras.z`/`extras.w` its distance shell in
// units of its own probe spacing, and `extras.x`/`extras.y` the level above
// (atlas and grid), or zero for the coarsest level. A level resolves only what
// falls inside its shell and defers a miss upward, so the levels partition
// distance instead of each integrating the whole scene.
//
// One workgroup per probe, one thread per direction: the RT prelude fixes the
// workgroup at 8x8 = 64, which is the direction count wanted here anyway. The
// 64 results reduce in shared memory to 4 spherical-harmonic coefficients, so
// a probe costs four texel writes per frame however many rays fed it.
//
// Cost is independent of light count by construction: each hit shades against
// ONE uniformly chosen light (velk_single_light), scaled by the count, and the
// temporal blend converges.
[[maybe_unused]] constexpr string_view probe_gi_main_src = R"(
shared vec3 s_sh[4][64];
// The level above, sampled once per probe rather than once per ray: the eight
// rays of a probe all start from the same point.
shared vec3 s_coarse[8][4];

// Probe grid dimensions, packed 10 bits per axis.
uvec3 velk_probe_dims_of(uint p)
{
    return uvec3(p & 1023u, (p >> 10) & 1023u, (p >> 20) & 1023u);
}

uvec3 velk_probe_dims()
{
    return velk_probe_dims_of(pc.env.z);
}

// Radiance along a direction from an L1 projection of radiance. NOT the
// constants the surface lookup uses: that one convolves with a cosine lobe to
// get irradiance, while a merge substitutes what the ray itself would have
// seen.
vec3 velk_sh_radiance(vec3 sh[4], vec3 d)
{
    return 0.282095 * sh[0]
         + 0.488603 * (sh[1] * d.y + sh[2] * d.z + sh[3] * d.x);
}

// Trilinear sample of the level above at a point in open space. No hemisphere
// weighting here: that term exists to keep a probe from lighting a surface
// through its own back, and this is a free-space radiance query with no
// surface involved.
void velk_coarse_sh(uint atlas, uvec3 cd, vec3 pos, out vec3 sh[4])
{
    for (int c = 0; c < 4; ++c) sh[c] = vec3(0.0);
    if (atlas == 0u || cd.x == 0u || cd.y == 0u || cd.z == 0u) return;

    vec3 gmin = VELK_GLOBALS.probe_min;
    vec3 gext = max(VELK_GLOBALS.probe_max - gmin, vec3(1e-6));

    vec3 g = (pos - gmin) / gext * vec3(cd) - 0.5;
    vec3 gbase = floor(g);
    vec3 gfrac = clamp(g - gbase, vec3(0.0), vec3(1.0));
    ivec3 base = ivec3(gbase);
    ivec3 last = ivec3(cd) - ivec3(1);

    vec2 rcp = 1.0 / vec2(float(cd.x * cd.z), float(cd.y * 4u));
    float total = 0.0;
    for (int o = 0; o < 8; ++o) {
        ivec3 off = ivec3(o & 1, (o >> 1) & 1, (o >> 2) & 1);
        ivec3 pi = clamp(base + off, ivec3(0), last);
        vec3 tri = mix(vec3(1.0) - gfrac, gfrac, vec3(off));
        float w = tri.x * tri.y * tri.z;
        if (w <= 0.0) continue;
        for (int c = 0; c < 4; ++c) {
            vec2 uv = (vec2(float(pi.x + pi.z * int(cd.x)),
                            float(pi.y + c * int(cd.y))) + 0.5) * rcp;
            sh[c] += velk_texture(atlas, uv).rgb * w;
        }
        total += w;
    }
    if (total > 0.0) {
        float inv_total = 1.0 / total;
        for (int c = 0; c < 4; ++c) sh[c] *= inv_total;
    }
}

// Fibonacci sphere, rotated per frame so successive frames fill the gaps
// between directions instead of resampling the same 64.
vec3 velk_probe_dir(uint i, uint n, float phase)
{
    float k = float(i) + 0.5;
    float phi = acos(1.0 - 2.0 * k / float(n));
    float theta = 3.14159265 * (1.0 + sqrt(5.0)) * k + phase;
    return vec3(cos(theta) * sin(phi), sin(theta) * sin(phi), cos(phi));
}

float velk_probe_hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return float(x) * (1.0 / 4294967296.0);
}
)"
                                                                      R"(
void main()
{
    // Cascade 0 shape: MANY probes with FEW directions each. The hierarchy
    // exists so that near probes trace short rays at high spatial resolution
    // and each level above trades 4x fewer probes for 4x more directions.
    // 64 directions per probe would be DDGI, not a cascade, and costs 8x the
    // rays for angular resolution the levels above are meant to supply.
    //
    // The RT prelude fixes the workgroup at 64 threads, so one group covers
    // 8 probes x 8 directions, with eight independent reductions inside it.
    const uint kDirs = 8u;
    const uint kProbesPerGroup = 8u;

    // EVERY level runs in this one dispatch, workgroups laid out finest first.
    // Three separate dispatches cost three launches and, worse, three graph
    // barriers, and that fixed cost dominated: the outermost level measured
    // 2.6 ms to trace 6912 rays, which is roughly 380 ns per ray against a
    // tracer floor of 1.8. It was not tracing, it was launching.
    //
    // A level reads the one above from the PREVIOUS frame rather than this
    // one, which is what makes a single dispatch legal: workgroups have no
    // ordering between them and no device-scope barrier, so a live dependency
    // could only be expressed by splitting the dispatch again. Atomics would
    // not help - they order accesses to a location, not workgroups. The coarse
    // levels ping-pong so nothing is read and written in the same dispatch,
    // and the staleness costs one frame in a field whose temporal window is
    // twenty.
    const uint kLevels = 3u;
    uvec3 dims0 = velk_probe_dims_of(pc.env.z);

    // The fine level updates half its probes per frame, alternating halves. It
    // holds roughly eight ninths of the probes and its cost is dominated by
    // SHADING each hit (a material evaluation plus a shadow ray), not by
    // tracing, so halving how many probes shade halves the pass.
    //
    // The split is by WORKGROUP, not by probe. A group's 8 probes run
    // concurrently across its 64 lanes, so idling half of them inside a group
    // leaves the group running exactly as long as before and saves nothing.
    // Whole groups drop out instead and the dispatch shrinks to match, so the
    // skipped half never launches.
    //
    // Rounded up, so an odd group count leaves the last group covered on one
    // phase and out of range on the other rather than never updating.
    //
    // Bit 1, not bit 0: bit 0 is what the coarse levels ping-pong on, so a
    // phase taken from it would tie each half permanently to one of the two
    // coarse buffers. They hold the same field but carry different noise, and
    // level 0 takes its miss radiance from there, so the halves would inherit
    // systematically different indirect light and print as slabs. Running each
    // half on two consecutive frames lets both see both buffers.
    uint rr_phase = (VELK_GLOBALS.present_counter >> 1) & 1u;
    uint fine_groups = ((dims0.x * dims0.y * dims0.z) / kProbesPerGroup + 1u) >> 1;

    // Which level owns this workgroup. Each level halves every axis, so its
    // probe count drops eightfold and the walk is three iterations.
    uint level = 0u;
    uvec3 dims = dims0;
    uint group_base = 0u;
    for (uint i = 0u; i < kLevels; ++i) {
        uvec3 di = max(dims0 >> i, uvec3(1u));
        uint groups_i = (i == 0u) ? fine_groups
                                  : (di.x * di.y * di.z) / kProbesPerGroup;
        if (gl_WorkGroupID.x < group_base + groups_i) {
            level = i;
            dims = di;
            break;
        }
        group_base += groups_i;
    }

    uint probe_count = dims.x * dims.y * dims.z;
    uint lane = gl_LocalInvocationIndex;
    uint probe_local = lane / kDirs;
    uint dir = lane - probe_local * kDirs;
    // Only the fine level round-robins. The coarse levels ping-pong, so
    // skipping one would leave the buffer it was due to write holding a value
    // two frames old while the level below reads it as the previous frame's.
    //
    // Which half a probe falls in is fixed rather than shuffled, and that is
    // enough: every probe still updates at the same rate, so the halves differ
    // only by one frame of phase against a temporal window of twenty.
    uint group_in_level = gl_WorkGroupID.x - group_base;
    if (level == 0u) group_in_level = group_in_level * 2u + rr_phase;
    uint probe = group_in_level * kProbesPerGroup + probe_local;
    // No early return before the barriers: every thread must reach them.
    // `probe_live` rather than `active`, which GLSL reserves.
    bool probe_live = probe < probe_count && probe_count > 0u;

    // Atlas handles. The fine level is single buffered because nothing reads it
    // inside this dispatch, which also keeps the handle the lighting pass bakes
    // into its push constants stable across frames.
    uint atlas_write;
    uint atlas_self;    // this level's previous value, for the temporal blend
    uint atlas_coarse;  // the level above, previous frame; 0 at the outermost
    if (level == 0u) {
        atlas_write = pc.env.w;   atlas_self = pc.env.w;   atlas_coarse = pc.extras.x;
    } else if (level == 1u) {
        atlas_write = pc.extras.y; atlas_self = pc.extras.x; atlas_coarse = pc.extras.z;
    } else {
        atlas_write = pc.extras.w; atlas_self = pc.extras.z; atlas_coarse = 0u;
    }
    uvec3 coarse_dims = max(dims0 >> (level + 1u), uvec3(1u));

    // Probe positions come from the volume the CPU places, which stays put while
    // geometry moves inside it, so probes keep their history.
    vec3 bmin = VELK_GLOBALS.probe_min;
    vec3 extent = VELK_GLOBALS.probe_max - bmin;
    uvec3 pc3 = uvec3(0u);
    vec3 origin = bmin;
    if (probe_live) {
        pc3 = uvec3(probe % dims.x, (probe / dims.x) % dims.y,
                    probe / (dims.x * dims.y));
        origin = bmin + extent * ((vec3(pc3) + 0.5) / vec3(dims));
    }

    // Rotating the direction set per frame matters far more with 8 directions
    // than with 64: successive frames fill the gaps, and the temporal blend
    // integrates them.
    //
    // The rotation is per PROBE as well as per frame. Sharing one phase across
    // the whole field correlates every probe's sampling error, so a frame whose
    // phase happens to favour bright surfaces lifts the entire field at once
    // and the temporal blend then drags that excursion out over its window -
    // visible as the whole scene slowly pulsing brighter and darker. Making the
    // phase independent per probe turns one field-wide swing into unrelated
    // per-probe noise, which the lookup already averages over eight probes.
    float phase = velk_probe_hash(probe * 2654435761u
                                  + VELK_GLOBALS.present_counter * 747796405u)
                * 6.28318531;
    vec3 d = velk_probe_dir(dir, kDirs, phase);

    // This level's distance shell. Spacing doubles with each level, so with the
    // fine spacing S the shells are [0, S), [S, 2S) and [2S, inf): each starts
    // half its own spacing in, which is exactly where the level below ended.
    // They PARTITION distance rather than each integrating the whole scene, and
    // only the outermost - the one with fewest probes - traces unbounded.
    vec3 cell = extent / vec3(dims);
    float spacing = max(cell.x, max(cell.y, cell.z));
    float t_min = (level == 0u) ? 0.0 : 0.5 * spacing;
    float t_max = (level + 1u >= kLevels) ? 1e30 : spacing;

    // Sample the level above once per probe rather than once per ray: all eight
    // rays leave from the same point. Lane 0 of each probe's block does it, and
    // the barrier sits outside the branch so every thread reaches it.
    bool has_coarse = atlas_coarse != 0u;
    if (dir == 0u) {
        vec3 csh[4];
        if (probe_live && has_coarse) {
            velk_coarse_sh(atlas_coarse, coarse_dims, origin, csh);
        } else {
            for (int c = 0; c < 4; ++c) csh[c] = vec3(0.0);
        }
        for (int c = 0; c < 4; ++c) s_coarse[probe_local][c] = csh[c];
    }
    barrier();

    vec3 radiance = vec3(0.0);
    if (probe_live) {
        Ray ray;
        ray.origin = origin;
        ray.dir = d;
        RayHit hit;
        // A probe's rays stand for everything around it, out to the next probes, so
        // instances smaller than that are not hit or missed one by one.
        velk_ray_footprint = spacing * 0.25;
        velk_ray_medium_depth = 0.0;
        velk_ray_medium_color = vec3(0.0);
        if (trace_closest_hit(ray, t_min, t_max, hit)) {
            RtShape s = velk_bvh_shapes.data[hit.shape_index];
            EvalContext ectx;
            ectx.material_base = s.material_base;
            ectx.texture_id = s.texture_id;
            ectx.shape_param = s.shape_param;
            ectx.uv = hit.uv;
            ectx.uv1 = hit.uv;
            ectx.base = s.color;
            ectx.ray_dir = ray.dir;
            ectx.normal = hit.normal;
            ectx.hit_pos = ray.origin + hit.t * ray.dir;
            ectx.tangent = vec4(0.0);

            // Full material evaluation. Measured FASTER than hand-rolled
            // single-light shading (2.8 ms vs 3.5 ms at 256 probes), because
            // materials that terminate early never trace a shadow ray at all,
            // where the hand-rolled version traced one per hit unconditionally.
            // Reservoir-pick ONE light, weighted by its unshadowed contribution
            // here, rather than uniformly. Uniform picking is unbiased but its
            // variance is brutal: a light contributing nothing to this hit -
            // aimed away, out of range, or muted to zero intensity - still wins
            // its share of the draws and returns black scaled by the light
            // count. That noise does not merely look bad. The lookup clamps the
            // reconstructed irradiance at zero, so it truncates the negative
            // excursions and keeps the positive ones, and the field converges
            // measurably too BRIGHT.
            //
            // One pass of cheap ALU over the lights, no extra rays: the ray
            // count per hit stays independent of the light count, which is the
            // part that costs.
            uint seed = probe * 9781u + dir * 6271u
                      + VELK_GLOBALS.present_counter * 26699u;
            float w_sum = 0.0;
            float w_chosen = 0.0;
            for (uint li = 0u; li < pc.light_count; ++li) {
                Light light = velk_lights.data[pc.lights_base + li];
                vec3 L;
                float atten;
                velk_light_sample(light, ectx.hit_pos, L, atten);
                // Floored, not clamped to zero: this weights against the
                // GEOMETRIC normal while shading uses the normal-mapped one, so
                // a light this test rejects can still light the surface. A
                // small floor keeps such a light reachable and the estimator
                // unbiased. Zero intensity and out-of-range lights are excluded
                // by the terms below instead, where the contribution really is
                // zero for shading too.
                float ndl = max(dot(hit.normal, L), 0.05);
                vec3 rad = light.color_intensity.rgb * light.color_intensity.a;
                float lw = (rad.r + rad.g + rad.b) * (1.0 / 3.0) * atten * ndl;
                if (lw <= 0.0) continue;
                w_sum += lw;
                seed = seed * 1664525u + 1013904223u;
                if (velk_probe_hash(seed) * w_sum <= lw) {
                    velk_single_light = int(li);
                    w_chosen = lw;
                }
            }
            // 1 / p(chosen). Every light with any contribution stays reachable,
            // so the estimator is unbiased; only the variance changes.
            velk_single_light_weight = (w_chosen > 0.0) ? (w_sum / w_chosen) : 0.0;
            BrdfSample bs = velk_resolve_fill(s.material_id, ectx);
            velk_single_light = -1;
            velk_single_light_weight = 1.0;
            radiance = bs.emission.rgb * clamp(bs.emission.a, 0.0, 1.0);
        } else {
            // Nothing inside this shell, so the interval is transparent and the
            // ray's radiance belongs to the level above. Only the coarsest
            // level, which has none, falls back to the environment. Taking the
            // environment at EVERY level would double-count the sky, and worse,
            // it turns each ray into a bright/dark decision that flips over one
            // probe spacing, which interpolation then draws as a grid.
            if (has_coarse) {
                vec3 csh[4];
                for (int c = 0; c < 4; ++c) csh[c] = s_coarse[probe_local][c];
                radiance = max(velk_sh_radiance(csh, d), vec3(0.0));
            } else {
                radiance = env_miss_color(d);
            }
        }
        velk_ray_footprint = 0.0;
)"
                                                                      R"(
        // The ray lost light in what it passed through, and gained what that reflects,
        // lit as rounded objects seen along the ray. What reaches them from elsewhere
        // is what the level above sees that way, or the environment.
        if (velk_ray_medium_depth > 0.0) {
            vec3 direct = vec3(0.0);
            for (uint li = 0u; li < pc.light_count; ++li) {
                Light light = velk_lights.data[pc.lights_base + li];
                vec3 L;
                float atten;
                velk_light_sample(light, origin, L, atten);
                float cos_g = clamp(dot(L, -d), -1.0, 1.0);
                float g = acos(cos_g);
                direct += light.color_intensity.rgb * light.color_intensity.a * atten * (2.0 / 3.0)
                          * (sin(g) + (3.14159265 - g) * cos_g) / 3.14159265;
            }
            vec3 elsewhere;
            if (has_coarse) {
                vec3 csh[4];
                for (int c = 0; c < 4; ++c) csh[c] = s_coarse[probe_local][c];
                elsewhere = max(velk_sh_radiance(csh, d), vec3(0.0));
            } else {
                elsewhere = env_miss_color(d);
            }
            float through = exp(-velk_ray_medium_depth);
            radiance = radiance * through
                + (velk_ray_medium_color / velk_ray_medium_depth) * (direct + elsewhere) * (1.0 - through);
        }
    }

    // Project into SH L1. 4*pi/N is the Monte Carlo weight for directions
    // distributed uniformly over the sphere.
    const float kY0 = 0.282095;
    const float kY1 = 0.488603;
    float w = 4.0 * 3.14159265 / float(kDirs);
    s_sh[0][lane] = radiance * (kY0 * w);
    s_sh[1][lane] = radiance * (kY1 * d.y * w);
    s_sh[2][lane] = radiance * (kY1 * d.z * w);
    s_sh[3][lane] = radiance * (kY1 * d.x * w);
    barrier();

    // Reduce within each probe's 8-lane block. The barrier stays outside the
    // conditional so every thread reaches it.
    for (uint stride = kDirs >> 1; stride > 0u; stride >>= 1) {
        if (dir < stride) {
            for (int c = 0; c < 4; ++c) s_sh[c][lane] += s_sh[c][lane + stride];
        }
        barrier();
    }

    if (dir != 0u || !probe_live) return;

    // Atlas: x = px + pz * dims.x, y = py + coefficient * dims.y.
    uint atlas_w = dims.x * dims.z;
    uint atlas_h = dims.y * 4u;
    for (int c = 0; c < 4; ++c) {
        ivec2 texel = ivec2(int(pc3.x + pc3.z * dims.x), int(pc3.y) + c * int(dims.y));
        vec2 uv = (vec2(texel) + 0.5) / vec2(float(atlas_w), float(atlas_h));
        // The coarse levels read their previous texture and write the other, so
        // these are the same handle only for the fine level.
        vec3 prev = velk_texture(atlas_self, uv).rgb;
        // Fixed blend rather than a running count: probes re-converge cheaply
        // and a fixed rate stops moving lights smearing indefinitely.
        //
        // The first four frames after the volume changes overwrite instead of
        // blending, because that is what seeds the atlas and the fine level
        // reaches half its probes at a time, alternating every second frame.
        // Seeding a narrower window leaves a half blending 5% at a time into
        // whatever the texture happened to hold, which shows as slabs eight
        // probes wide and never clears at all if any of it was NaN.
        float blend = (VELK_GLOBALS.present_counter - VELK_GLOBALS.probe_reset_frame <= 3u)
                    ? 1.0 : 0.05;
        // RGBA32F, not F16: the RT prelude declares the f32 storage array, and
        // the atlas is small enough that the extra precision is free.
        // s_sh[c][lane] is this probe's block total, lane being its dir-0 slot.
        imageStore(gStorageImagesF32[nonuniformEXT(atlas_write)], texel,
                   vec4(mix(prev, s_sh[c][lane], blend), 1.0));
    }
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

    // Everything above is the shared RT body. Callers wanting that machinery
    // under a different entry point (the GI probe pass) supply their own main.
    src += main_src.empty() ? rt_compute_main_src : main_src;
    return src;
}

} // namespace velk

#endif // VELK_RENDER_DEFAULT_SHADERS_H
