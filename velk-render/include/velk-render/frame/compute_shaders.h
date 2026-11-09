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
// For a closest-hit ray that stands for a cone: the tangent of its half angle, for the
// shader to set. A part of a solid mesh narrower than the cone where the ray reaches it
// counts as the surface there. 0 for an exact ray.
float velk_ray_cone = 0.0;
// How far the surface a ray starts from may be from the full mesh rays are tested against,
// in world units, when it is drawn at a coarser level of detail. For the shader to set.
float velk_ray_lift = 0.0;
// What a closest-hit ray passed through in nodes too fine for it, for the shader to clear
// before tracing and use after: optical depth, and colour times depth.
float velk_ray_medium_depth = 0.0;
vec3 velk_ray_medium_color = vec3(0.0);
// What the coordinates of the tree being walked are relative to, from where rays are: the
// camera, or inside a partition, the camera taken from the partition's origin.
vec3 velk_ray_frame = vec3(0.0);

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
    vec3 rel = ray.origin - (vec3(rec.inv_row0.w, rec.inv_row1.w, rec.inv_row2.w) - velk_ray_frame);
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

)"
                                                                      R"(
// A node of the top tree, moved to where rays are.
BvhNode velk_top_node(uint index)
{
    BvhNode node = velk_bvh_nodes.data[index];
    node.aabb_min.xyz -= velk_ray_frame;
    node.aabb_max.xyz -= velk_ray_frame;
    return node;
}

// The walk behind every ray: one loop that does one piece of work per iteration, a triangle, a
// node of a mesh or of a record's small tree, a shape, a record, or a node of the scene. Each
// lane of a wave works from its own state, so none waits for another's inner loop. With
// @p closest it finds the nearest hit, and otherwise how much light gets through within t_max.
// With @p seeded it tests only @p seed, a shape placed by record @p seed_record.
//
// Mesh hits keep only what identifies them, and the surface is worked out once, for the winner.
//
// With VELK_TRACE_MEDIUM, for scenes with instances that have a far form: a node of instances
// not much wider than the ray's footprint counts as a medium, since one line through it would
// hit or miss one instance at random. Light falls off with distance through it, by its density
// of instances' area per volume. A node's area is in aabb_max.w, 0 for one not made of
// instances, and its average colour is in the bits of aabb_min.w, as unorm RGB. Compiled out
// otherwise, since registers are paid for by the whole pass.
#ifdef VELK_TRACE_MEDIUM
// A mesh counted as a medium keeps going past what other shapes hit, as far as this record's
// limit, since what it holds in front of that still dims the ray.
#define VELK_WALK_LIMIT (mesh_medium ? min(mesh_best, medium_far * mesh_scale) : min(mesh_best, best_t * mesh_scale))
#else
#define VELK_WALK_LIMIT min(mesh_best, best_t * mesh_scale)
#endif

float velk_walk(Ray ray, float t_max, bool closest, bool seeded, uint seed_record, RtShape seed, out RayHit hit)
{
    hit.t = t_max;
    hit.shape_index = 0xFFFFFFFFu;
    hit.uv = vec2(0.0);
    hit.normal = vec3(0.0);
    if (VELK_GLOBALS.bvh_node_count == 0u && !seeded) return closest ? 0.0 : 1.0;

    const int kStack = VELK_TRACE_STACK_SIZE + 64;
    const uint kScene = 0u;
    const uint kMesh = 1u;
    const uint kTree = 2u;
    const uint kNone = 0xFFFFFFFFu;

    // The three words past kStack hold the frame of an analytic hit, which few rays need
    // and registers cannot spare.
    uint stack[kStack + 3];
    int sp = 0;
    if (!seeded) stack[sp++] = VELK_GLOBALS.bvh_root;
    velk_ray_frame = VELK_GLOBALS.cam_pos.xyz;
    int partition_sp = -1;

    uint level = kScene;
    uint level_after_mesh = kScene;
    int mesh_scope = 0;
    int tree_scope = 0;

    uint record_at = 0u;
    uint records_left = 0u;
    uint shape_at = 0u;
    uint shapes_left = seeded ? 1u : 0u;
    uint triangle_at = 0u;
    // A leaf's shape_count: triangles left in the low half, its vertices in the high half.
    uint leaf_counts = 0u;

    uint cur_record = seeded ? seed_record : kNone;
    uint cur_shape = kNone;
    Ray walk_ray = ray;
    float mesh_scale = 1.0;
    uint mesh_base = 0u;
    uint word_base = 0u;
    uint positions = 0u;
    float mesh_cone = 0.0;
    float mesh_best = 1e30;

    float best_t = t_max;
    uint best_shape = kNone;
    uint best_record = kNone;
    uint best_triangle = 0u;
    uint best_vertex_ids = 0u;
    float best_u = 0.0;
    float best_v = 0.0;
    bool best_stopped = false;
    uint best_stop_normal = 0u;

#ifdef VELK_TRACE_MEDIUM
    float footprint = velk_ray_footprint;
    float spread = closest ? 0.0 : velk_ray_spread;
    bool record_medium = false;
    bool mesh_medium = false;
    float medium_far = t_max;
    uint mesh_color_word = 0u;
    float medium_depth = 0.0;
    vec3 medium_color = vec3(0.0);
    float visibility = 1.0;
#endif

    while (true) {
        if ((leaf_counts & 0xFFFFu) != 0u) {
            uint tri = velk_cluster_words.data[triangle_at];
            ++triangle_at;
            --leaf_counts;
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
            // Moller-Trumbore, with thresholds that scale with the triangle, so that meshes
            // with tiny local coordinates are not all filtered out as near-parallel.
            vec3 e1 = v1 - v0;
            vec3 e2 = v2 - v0;
            vec3 pv = cross(walk_ray.dir, e2);
            float det = dot(e1, pv);
            if (abs(det) < 1e-7 * max(length(e1) * length(pv), 1e-30)) continue;
            float inv_det = 1.0 / det;
            vec3 to_v0 = walk_ray.origin - v0;
            float u = dot(to_v0, pv) * inv_det;
            if (u < 0.0 || u > 1.0) continue;
            vec3 q = cross(to_v0, e1);
            float v = dot(walk_ray.dir, q) * inv_det;
            if (v < 0.0 || u + v > 1.0) continue;
            float tt = dot(e2, q) * inv_det;
            if (tt < 1e-4 * max(length(e1), length(e2)) || tt >= mesh_best) continue;
            mesh_best = tt;
            float world_t = tt / mesh_scale;
            if (world_t < best_t) {
                if (!closest) return 0.0;
                best_t = world_t;
                best_shape = cur_shape;
                best_record = cur_record;
                best_u = u;
                best_v = v;
                best_triangle = tri;
                best_vertex_ids = positions + (leaf_counts >> 16u) * 3u;
                best_stopped = false;
            }
        } else if (level == kMesh) {
            if (sp == mesh_scope) {
                level = level_after_mesh;
                if (level == kTree) walk_ray = velk_local_ray(ray, velk_records.data[cur_record]);
                continue;
            }
            uint ref = stack[--sp];
            if ((ref & VELK_BLAS_LEAF) != 0u) {
                uint leaf = word_base + (ref & ~VELK_BLAS_LEAF);
                triangle_at = word_base + velk_cluster_words.data[leaf];
                leaf_counts = velk_cluster_words.data[leaf + 1u];
                positions = word_base + velk_cluster_words.data[leaf + 2u];
                continue;
            }
            VelkMeshNode node = velk_mesh_nodes.data[mesh_base + ref];
            float t_l, t_r;
            float limit = VELK_WALK_LIMIT;
            bool h_l = ray_aabb(walk_ray, node.a.xyz, vec3(node.a.w, node.b.xy), limit, t_l);
            bool h_r = ray_aabb(walk_ray, vec3(node.b.zw, node.c.x), node.c.yzw, limit, t_r);
            bool left_first = h_l && (!h_r || t_l <= t_r);
            uint near_ref = kNone;
            uint far_ref = kNone;
            // The nearer child first, so a near stop can cull the far one, and it goes on top
            // of the stack so that a near hit or a blocker is found sooner.
            for (uint k = 0u; k < 2u; ++k) {
                bool left = (k == 0u) == left_first;
                if (!(left ? h_l : h_r)) continue;
                float t_aabb = left ? t_l : t_r;
                if (t_aabb > VELK_WALK_LIMIT) continue;
                uint child = left ? node.refs.x : node.refs.y;
                uint surface = left ? node.refs.z : node.refs.w;
                vec3 box_min = left ? node.a.xyz : vec3(node.b.zw, node.c.x);
                vec3 box_max = left ? vec3(node.a.w, node.b.xy) : node.c.yzw;
)"
                                                                      R"(
#ifdef VELK_TRACE_MEDIUM
                if (mesh_medium) {
                    vec3 extent = box_max - box_min;
                    float widest = max(max(extent.x, extent.y), extent.z);
                    // In the mesh's units, where its t is the world's times mesh_scale.
                    if (widest < (footprint * mesh_scale + spread * t_aabb) * velk_medium_reach(mesh_base + child)) {
                        vec3 sides = max(extent, vec3(widest * 1e-3));
                        float leave = min(min(ray_aabb_exit(walk_ray, box_min, box_max), medium_far * mesh_scale), mesh_best);
                        float amount = uintBitsToFloat(surface) / (sides.x * sides.y * sides.z) * max(leave - t_aabb, 0.0);
                        if (closest) {
                            medium_depth += amount;
                            medium_color += amount * unpackUnorm4x8(mesh_color_word).rgb;
                        } else {
                            visibility *= exp(-amount);
                        }
                        continue;
                    }
                }
#endif
                // Narrower than the cone: this child is the surface. A real triangle under it
                // gives the material something to read.
                if (mesh_cone > 0.0) {
                    vec3 extent = box_max - box_min;
                    if (max(max(extent.x, extent.y), extent.z) < mesh_cone * t_aabb) {
                        uint under = child;
                        while ((under & VELK_BLAS_LEAF) == 0u) {
                            under = velk_mesh_nodes.data[mesh_base + under].refs.x;
                        }
                        uint leaf = word_base + (under & ~VELK_BLAS_LEAF);
                        mesh_best = t_aabb;
                        best_t = t_aabb / mesh_scale;
                        best_shape = cur_shape;
                        best_record = cur_record;
                        best_u = 1.0 / 3.0;
                        best_v = best_u;
                        best_triangle = velk_cluster_words.data[word_base + velk_cluster_words.data[leaf]];
                        best_vertex_ids = word_base + velk_cluster_words.data[leaf + 2u] + (velk_cluster_words.data[leaf + 1u] >> 16u) * 3u;
                        best_stopped = true;
                        best_stop_normal = surface;
                        continue;
                    }
                }
                if (k == 0u) {
                    near_ref = child;
                } else {
                    far_ref = child;
                }
            }
            if (far_ref != kNone && sp < kStack) stack[sp++] = far_ref;
            if (near_ref != kNone && sp < kStack) stack[sp++] = near_ref;
        } else if (shapes_left > 0u) {
            uint index = shape_at;
            ++shape_at;
            --shapes_left;
            Ray shape_ray = walk_ray;
            if (level == kScene) {
                RtRecord shape_rec = velk_records.data[cur_record];
                shape_ray = velk_local_ray(ray, shape_rec);
#ifdef VELK_RAY_MOTION
                if (!seeded) shape_ray = velk_moved_ray(shape_ray, shape_rec, index);
#endif
            }
            RtShape shape = seeded ? seed : velk_bvh_shapes.data[index];
            if (shape.shape_kind == 255u) {
                if (shape.mesh_static_base == VELK_INVALID_MESH_STATIC) continue;
#ifdef VELK_TRACE_MEDIUM
                bool is_medium = record_medium;
                float reach = is_medium ? medium_far : best_t;
#else
                float reach = best_t;
#endif
                float t_box;
                if (!ray_aabb(shape_ray, shape.origin.xyz, shape.u_axis.xyz, reach, t_box)) continue;
                MeshStaticData st = velk_mesh_static(shape);
                if (st.triangle_count == 0u || st.vertex_stride == 0u || st.blas_node_count == 0u) continue;
                // Normalized so that the triangle thresholds do not depend on how much an
                // instance is scaled. The length converts hits back to the ray's t.
                float scale = length(shape_ray.dir);
                if (scale < 1e-30) continue;
                walk_ray.origin = shape_ray.origin;
                walk_ray.dir = shape_ray.dir / scale;
                mesh_scale = scale;
                mesh_base = st.blas_node_base;
                word_base = st.cluster_word_base;
                mesh_cone = closest && (st.flags & (VELK_MESH_SCATTERED | VELK_MESH_DEFORMING)) == 0u ? velk_ray_cone : 0.0;
#ifdef VELK_TRACE_MEDIUM
                mesh_medium = is_medium && (st.flags & VELK_MESH_SCATTERED) != 0u;
#endif
                mesh_best = 1e30;
                cur_shape = index;
                level_after_mesh = level;
                level = kMesh;
                mesh_scope = sp;
                if (sp < kStack) stack[sp++] = st.blas_root;
            } else {
                RayHit h;
                if (intersect_shape(shape_ray, shape, h) && h.t > 0.0 && h.t < best_t) {
                    if (!closest) return 0.0;
                    best_t = h.t;
                    best_shape = index;
                    best_record = cur_record;
                    stack[kStack] = floatBitsToUint(velk_ray_frame.x);
                    stack[kStack + 1] = floatBitsToUint(velk_ray_frame.y);
                    stack[kStack + 2] = floatBitsToUint(velk_ray_frame.z);
                }
            }
        } else if (level == kTree) {
            if (sp == tree_scope) {
                level = kScene;
                continue;
            }
            BvhNode node = velk_blas_nodes.data[stack[--sp]];
            float t_enter;
            if (!ray_aabb(walk_ray, node.aabb_min.xyz, node.aabb_max.xyz, best_t, t_enter)) continue;
            shape_at = node.first_shape;
            shapes_left = node.shape_count;
            for (uint c = 0u; c < node.child_count; ++c) {
                if (sp < kStack) stack[sp++] = node.first_child + c;
            }
        } else if (records_left > 0u) {
            cur_record = record_at;
            ++record_at;
            --records_left;
            RtRecord rec = velk_records.data[cur_record];
#ifdef VELK_TRACE_MEDIUM
            record_medium = footprint > 0.0 && (rec.flags & 2u) != 0u && (closest || rec.tree_root == VELK_NO_TREE);
            medium_far = best_t;
#endif
            if (rec.tree_root == VELK_NO_TREE) {
                shape_at = rec.first_shape;
                shapes_left = rec.shape_count;
            } else {
                walk_ray = velk_local_ray(ray, rec);
                level = kTree;
                tree_scope = sp;
                if (sp < kStack) stack[sp++] = rec.tree_root;
            }
        } else {
            if (sp == 0) break;
            if (sp == partition_sp) {
                velk_ray_frame = VELK_GLOBALS.cam_pos.xyz;
                partition_sp = -1;
            }
            uint ni = stack[--sp];
            BvhNode node = velk_top_node(ni);
            float t_enter;
            if (!ray_aabb(ray, node.aabb_min.xyz, node.aabb_max.xyz, best_t, t_enter)) continue;
            // The reference's box already covers the partition's root, so the root needs
            // no test of its own.
            if (node.child_count == VELK_PARTITION_REF) {
                partition_sp = sp;
                velk_ray_frame = VELK_GLOBALS.cam_pos.xyz - velk_partition_origin(node);
                ni = node.first_child;
                node = velk_top_node(ni);
            }
)"
                                                                      R"(
#ifdef VELK_TRACE_MEDIUM
            if (footprint > 0.0 && node.aabb_max.w > 0.0) {
                vec3 extent = node.aabb_max.xyz - node.aabb_min.xyz;
                float widest = max(max(extent.x, extent.y), extent.z);
                // The leaf of a partition without its instances has nothing else to offer.
                if (widest < (footprint + spread * max(t_enter, 0.0)) * velk_medium_reach(ni) || (node.child_count == 0u && node.shape_count == 0u)) {
                    float enter;
                    if (ray_aabb(ray, node.aabb_min.xyz, node.aabb_max.xyz, best_t, enter)) {
                        vec3 sides = max(extent, vec3(widest * 1e-3));
                        float leave = min(ray_aabb_exit(ray, node.aabb_min.xyz, node.aabb_max.xyz), best_t);
                        float amount = node.aabb_max.w / (sides.x * sides.y * sides.z) * max(leave - enter, 0.0);
                        if (closest) {
                            medium_depth += amount;
                            medium_color += amount * unpackUnorm4x8(floatBitsToUint(node.aabb_min.w)).rgb;
                        } else {
                            visibility *= exp(-amount);
                        }
                    }
                    continue;
                }
            }
            mesh_color_word = floatBitsToUint(node.aabb_min.w);
#endif
            record_at = node.first_shape;
            records_left = node.shape_count;
#ifdef VELK_TRACE_ORDERED_ANY_HIT
            // Front to back for a shadow ray, since the near child is the likelier blocker.
            if (!closest && node.child_count == 2u) {
                BvhNode l = velk_top_node(node.first_child);
                BvhNode r = velk_top_node(node.first_child + 1u);
                float t_l, t_r;
                bool h_l = ray_aabb(ray, l.aabb_min.xyz, l.aabb_max.xyz, best_t, t_l);
                bool h_r = ray_aabb(ray, r.aabb_min.xyz, r.aabb_max.xyz, best_t, t_r);
                if (h_l && h_r) {
                    uint near_child = t_l <= t_r ? node.first_child : node.first_child + 1u;
                    uint far_child = t_l <= t_r ? node.first_child + 1u : node.first_child;
                    if (sp < kStack) stack[sp++] = far_child;
                    if (sp < kStack) stack[sp++] = near_child;
                } else if (h_l) {
                    if (sp < kStack) stack[sp++] = node.first_child;
                } else if (h_r) {
                    if (sp < kStack) stack[sp++] = node.first_child + 1u;
                }
                continue;
            }
#endif
            for (uint i = 0u; i < node.child_count; ++i) {
                if (sp < kStack) stack[sp++] = node.first_child + i;
            }
        }
    }

    if (!closest) {
#ifdef VELK_TRACE_MEDIUM
        return visibility;
#else
        return 1.0;
#endif
    }

#ifdef VELK_TRACE_MEDIUM
    velk_ray_medium_depth = medium_depth;
    velk_ray_medium_color = medium_color;
#endif
    if (best_shape == kNone) return 0.0;

    hit.t = best_t;
    hit.shape_index = best_shape;
    RtRecord rec = velk_records.data[best_record];
    RtShape shape = seeded ? seed : velk_bvh_shapes.data[best_shape];
    if (shape.shape_kind == 255u) {
        // Vertex layout: pos[0..2], normal[3..5], uv[6..7]. (u, v) from the triangle test
        // make (1-u-v, u, v) the weights for its three vertices.
        MeshStaticData st = velk_mesh_static(shape);
        uint floats_per_vert = st.vertex_stride >> 2u;
        float w = 1.0 - best_u - best_v;
        uint o0 = velk_cluster_words.data[best_vertex_ids + (best_triangle & 0xFFu)] * floats_per_vert;
        uint o1 = velk_cluster_words.data[best_vertex_ids + ((best_triangle >> 8u) & 0xFFu)] * floats_per_vert;
        uint o2 = velk_cluster_words.data[best_vertex_ids + ((best_triangle >> 16u) & 0xFFu)] * floats_per_vert;
        vec2 uv0 = vec2(velk_mesh_vertex(st, o0 + 6u), velk_mesh_vertex(st, o0 + 7u));
        vec2 uv1 = vec2(velk_mesh_vertex(st, o1 + 6u), velk_mesh_vertex(st, o1 + 7u));
        vec2 uv2 = vec2(velk_mesh_vertex(st, o2 + 6u), velk_mesh_vertex(st, o2 + 7u));
        hit.uv = uv0 * w + uv1 * best_u + uv2 * best_v;
        vec3 n;
        if (best_stopped) {
            // The less the node's surface agrees on a direction, the more it faces the ray.
            vec3 ld = normalize(vec3(dot(rec.inv_row0.xyz, ray.dir), dot(rec.inv_row1.xyz, ray.dir), dot(rec.inv_row2.xyz, ray.dir)));
            vec3 stop_normal = unpackSnorm4x8(best_stop_normal).xyz;
            vec3 bent = stop_normal - ld * (1.0 - min(length(stop_normal), 1.0));
            float bent_length = length(bent);
            n = bent_length > 1e-4 ? bent / bent_length : -ld;
        } else {
            vec3 n0 = vec3(velk_mesh_vertex(st, o0 + 3u), velk_mesh_vertex(st, o0 + 4u), velk_mesh_vertex(st, o0 + 5u));
            vec3 n1 = vec3(velk_mesh_vertex(st, o1 + 3u), velk_mesh_vertex(st, o1 + 4u), velk_mesh_vertex(st, o1 + 5u));
            vec3 n2 = vec3(velk_mesh_vertex(st, o2 + 3u), velk_mesh_vertex(st, o2 + 4u), velk_mesh_vertex(st, o2 + 5u));
            n = normalize(n0 * w + n1 * best_u + n2 * best_v);
        }
        hit.normal = velk_world_normal(rec, n);
    } else {
        velk_ray_frame = vec3(uintBitsToFloat(stack[kStack]), uintBitsToFloat(stack[kStack + 1]), uintBitsToFloat(stack[kStack + 2]));
        Ray shape_ray = velk_local_ray(ray, rec);
#ifdef VELK_RAY_MOTION
        if (!seeded && rec.tree_root == VELK_NO_TREE) shape_ray = velk_moved_ray(shape_ray, rec, best_shape);
#endif
        RayHit h;
        intersect_shape(shape_ray, shape, h);
        hit.uv = h.uv;
        hit.normal = velk_world_normal(rec, h.normal);
    }
    return 1.0;
}
#undef VELK_WALK_LIMIT

// How much light gets through along a shadow ray within t_max: 0 at the first blocker,
// otherwise what is too fine for the ray to resolve lets some through.
float trace_visibility(Ray ray, float t_max)
{
    RayHit h;
    RtShape none;
    return velk_walk(ray, t_max, false, false, 0u, none, h);
}

// Whether anything blocks the ray within t_max.
bool trace_any_hit(Ray ray, float t_max)
{
    return trace_visibility(ray, t_max) <= 0.0;
}

// The nearest hit within t_max, with its normal in world space. `hit.shape_index` indexes
// velk_bvh_shapes.
bool trace_closest_hit(Ray ray, float t_max, out RayHit hit)
{
    RtShape none;
    return velk_walk(ray, t_max, true, false, 0u, none, hit) > 0.5;
}

// Unbounded closest hit. On a miss `hit.t` stays at 1e30, which callers that
// shade the miss rely on.
bool trace_closest_hit(Ray ray, out RayHit hit)
{
    return trace_closest_hit(ray, 1e30, hit);
}

// The nearest hit on one shape placed by @p record, with its normal in world space.
bool trace_shape(Ray ray, uint record, RtShape shape, out RayHit hit)
{
    return velk_walk(ray, 1e30, true, true, record, shape, hit) > 0.5;
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

struct ResolveVertex {
    vec3 rel;
    vec3 normal;
    vec2 uv;
    vec2 uv1;
    vec4 tangent;
    vec4 color;
    uint shape_param;
};

ResolveVertex resolve_vertex(VelkDrawData d, ElementInstance inst, GlobalData g, uint index)
{
    VelkVertex3D v = velk_unpack_vertex3d(d.vbo_base + index * VELK_VERTEX3D_WORDS);
    VelkVaryings s = velk_material_varyings(d.material_id, v, inst, g);
    ResolveVertex r;
    r.rel = s.rel_pos;
    r.normal = s.world_normal;
    r.uv = s.local_uv;
    r.uv1 = vec2(velk_vertex_word(d.uv1_base, d.uv1_enabled * index * 2u),
                 velk_vertex_word(d.uv1_base, d.uv1_enabled * index * 2u + 1u));
    r.tangent = s.world_tangent;
    r.color = s.color;
    r.shape_param = s.shape_param;
    return r;
}

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

EvalContext resolve_context(VelkDrawData d, ElementInstance inst, GlobalData g, ResolveVertex v0, ResolveVertex v1,
                            ResolveVertex v2, vec3 weights, vec3 rel)
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

vec3 resolve_shading_normal(MaterialEval e, EvalContext ctx)
{
    return normalize(length(e.normal) > 0.0 ? e.normal : ctx.normal);
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(RESOLVE_WIDTH) || coord.y >= int(RESOLVE_HEIGHT)) return;

    uvec2 ids = texelFetch(velk_textures_uint[nonuniformEXT(RESOLVE_IDS)], coord, 0).xy;
    if (ids.x == 0u) {
        imageStore(gStorageImages[nonuniformEXT(RESOLVE_ALBEDO)], coord, vec4(0.0));
        imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_NORMAL)], coord, vec4(0.0));
        imageStore(gStorageImagesF32[nonuniformEXT(RESOLVE_POSITION)], coord, vec4(0.0));
        imageStore(gStorageImages[nonuniformEXT(RESOLVE_MATERIAL)], coord, vec4(0.0));
        imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_EMISSIVE)], coord, vec4(0.0));
        return;
    }

    VelkDrawData d = velk_draw_data.data[ids.x - 1u];
    uint triangle = ids.y & ((1u << d.triangle_bits) - 1u);
    uint position = ids.y >> d.triangle_bits;
    uint slot = d.visible_base == VELK_NO_VISIBLE_LIST ? position : velk_culling.data[d.visible_base + position];
    ElementInstance inst = velk_instances.data[d.instances_base + slot];
    GlobalData g = velk_globals.data[d.globals_base];

    // A triangle strip's triangles start one vertex apart.
    uvec3 corners = uvec3(triangle) + uvec3(0u, 1u, 2u);
    if (d.index_base != VELK_NO_VISIBLE_LIST) {
        uint first = d.cluster_first_base == VELK_NO_VISIBLE_LIST ? 0u : velk_culling.data[d.cluster_first_base + position];
        uint word = d.index_base + first + triangle * 3u;
        corners = uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
    }
    ResolveVertex v0 = resolve_vertex(d, inst, g, corners.x);
    ResolveVertex v1 = resolve_vertex(d, inst, g, corners.y);
    ResolveVertex v2 = resolve_vertex(d, inst, g, corners.z);

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

    EvalContext ctx = resolve_context(d, inst, g, v0, v1, v2, weights, rel);
    velk_uv_set = 0u;
    MaterialEval e = velk_eval_material(d.material_id, ctx);
    vec3 N = resolve_shading_normal(e, ctx);

    // Widens roughness where the shading normal changes quickly between pixels, so that a
    // tight highlight does not alias (Tokuyoshi 2019). The next pixels are taken on this
    // triangle, so an edge between surfaces does not count.
    EvalContext ctx_x = resolve_context(d, inst, g, v0, v1, v2, weights_x, rel_x);
    velk_uv_set = 0u;
    vec3 dndx = resolve_shading_normal(velk_eval_material(d.material_id, ctx_x), ctx_x) - N;
    EvalContext ctx_y = resolve_context(d, inst, g, v0, v1, v2, weights_y, rel_y);
    velk_uv_set = 0u;
    vec3 dndy = resolve_shading_normal(velk_eval_material(d.material_id, ctx_y), ctx_y) - N;
    float variance = 0.25 * (dot(dndx, dndx) + dot(dndy, dndy));
    float roughness = sqrt(clamp(e.roughness * e.roughness + min(2.0 * variance, 0.18), 0.0, 1.0));

    float lod_error = d.lod_error_base == VELK_NO_VISIBLE_LIST
        ? 0.0
        : uintBitsToFloat(velk_culling.data[d.lod_error_base + position]);

    imageStore(gStorageImages[nonuniformEXT(RESOLVE_ALBEDO)], coord, e.color);
    imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_NORMAL)], coord, vec4(N, lod_error));
    imageStore(gStorageImagesF32[nonuniformEXT(RESOLVE_POSITION)], coord, vec4(rel, 0.0));
    imageStore(gStorageImages[nonuniformEXT(RESOLVE_MATERIAL)], coord,
               vec4(e.metallic, roughness, float(e.lighting_mode) / 255.0, 0.0));
    imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_EMISSIVE)], coord, vec4(e.emissive, 0.0));
}
)";

/// Shows the visibility buffer as flat colours: mode 1 per draw, 2 per draw instance, 3 per
/// triangle. Mode 4 shows the depth pyramid's fourth level.
inline constexpr string_view visibility_debug_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform usampler2D velk_textures_uint[];
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint ids_id;
    uint output_id;
    uint width;
    uint height;
    uint mode;
    uint pyramid_word;
} pc;

vec3 id_color(uint id)
{
    uint h = (id + 1u) * 2654435761u;
    h ^= h >> 15u;
    h *= 2246822519u;
    h ^= h >> 13u;
    return vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0;
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

    uvec2 ids = texelFetch(velk_textures_uint[nonuniformEXT(pc.ids_id)], coord, 0).xy;
    vec3 color = vec3(0.0);
    if (ids.x != 0u) {
        uint draw = ids.x - 1u;
        uint bits = velk_draw_data.data[draw].triangle_bits;
        uint id = draw * 16381u;
        if (pc.mode >= 2u) id += (ids.y >> bits) * 8191u;
        if (pc.mode >= 3u) id += (ids.y & ((1u << bits) - 1u)) * 131u;
        color = id_color(id);
    }
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], coord, vec4(color, 1.0));
}
)";

/// @p source as the GI probe pass, with the number of rays each probe traces per update
/// compiled in. A power of two, 64 at most.
inline string velk_with_probe_dirs(string_view source, uint32_t dirs)
{
    const size_t version = source.find(string_view("#version"));
    const size_t after = source.find('\n', version) + 1;
    char define[72];
    const int size = std::snprintf(define, sizeof(define), "#define VELK_PROBE_PASS\n#define VELK_PROBE_DIRS %uu\n", dirs);
    string out;
    out += source.substr(0, after);
    out += string_view(define, static_cast<size_t>(size));
    out += source.substr(after);
    return out;
}

/// Samples the distance to what the camera sees at 64 points of the G-buffer into a run of
/// words, or -1 where it sees nothing.
[[maybe_unused]] constexpr string_view gi_measure_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint word;
    uint pyramid_word;
    uint globals_base;
} pc;

// Mirrors kGiDistanceFirstLog2 and kGiDistanceBinLog2 in deferred_path.cpp.
const float kFirstLog2 = -4.0;
const float kBinLog2 = 0.375;

shared uint s_bins[64];

// Counts what the view holds by how far away it is, in 64 bins of log2 distance, from a level
// of the depth pyramid. Each texel holds the farthest depth under it, so a thin thing close
// to the camera only counts where it covers a whole texel.
void main()
{
    uint lane = gl_LocalInvocationIndex;
    s_bins[lane] = 0u;
    barrier();

    const uint level = 3u;
    uint width = max(cull_words.data[pc.pyramid_word + 1u] >> level, 1u);
    uint height = max(cull_words.data[pc.pyramid_word + 2u] >> level, 1u);
    uint first = cull_words.data[pc.pyramid_word + 3u + level];
    mat4 inverse_vp = velk_globals.data[pc.globals_base].inverse_view_projection;
    uint texels = first != 0u ? width * height : 0u;
    for (uint i = lane; i < texels; i += 64u) {
        float depth = uintBitsToFloat(cull_words.data[first + i]);
        if (depth <= 0.0) continue;
        vec2 ndc = (vec2(float(i % width), float(i / width)) + 0.5) / vec2(float(width), float(height)) * 2.0 - 1.0;
        vec4 at = inverse_vp * vec4(ndc, depth, 1.0);
        float away = length(at.xyz / at.w);
        uint bin = uint(clamp((log2(max(away, 1e-6)) - kFirstLog2) / kBinLog2, 0.0, 63.0));
        atomicAdd(s_bins[bin], 1u);
    }
    barrier();
    cull_words.data[pc.word + lane] = s_bins[lane];
}
)";

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
// The GI probe field: eight levels of 32x32x32 cells around the camera, each level's cells
// twice as wide as the one below. Cell widths are powers of two in world units, named by
// their exponent, the level's ladder index. A probe's texel depends only on its world cell,
// so when the camera moves a level wraps around in the atlas and only the cells it newly
// covers change. Each texel block carries a stamp of the cell and ladder index it holds,
// so a stale block is recognised without tracking the movement.
//
// In the atlas, x = cell.x + cell.z * 32, and y = cell.y + (row + slot * 5) * 32, where a
// level's slot is its ladder index modulo 8. Rows 0 to 3 are SH coefficients. Row 0's alpha
// is the stamp, row 1's the number of samples blended in, and row 2's the share of rays that
// hit the inside of something. Row 4 is how far the probe has moved from its cell's centre,
// in cells.
//
// A second atlas holds two 8x8 octahedral maps per probe, side by side: irradiance over pi
// against each direction (rgb, gamma encoded), then the mean distance to surfaces in each
// direction and its square (rg). The maps, weights and blending follow DDGI.
#define VELK_GI_FIELD_GLSL R"(
const int kGiDim = 16;
const int kGiLevels = 8;
const int kGiRows = 5;
const int kGiMapSize = 8;
const float kGiIrradianceGamma = 5.0;

float velk_gi_cell(int ladder)
{
    return ldexp(1.0, ladder);
}

float velk_gi_stamp(ivec3 cell, int ladder)
{
    uint m = 63u;
    return float((uint(cell.x) & m) | ((uint(cell.y) & m) << 6u) | ((uint(cell.z) & m) << 12u) | ((uint(ladder) & m) << 18u));
}

vec2 velk_gi_uv(ivec3 texel, int slot, int row)
{
    return (vec2(float(texel.x + texel.z * kGiDim), float(texel.y + (row + slot * kGiRows) * kGiDim)) + 0.5)
        / vec2(float(kGiDim * kGiDim), float(kGiDim * kGiRows * kGiLevels));
}

// A map entry's texel: `side` 0 is irradiance, 1 distance.
ivec2 velk_gi_map_texel(ivec3 texel, int slot, ivec2 entry, int side)
{
    return ivec2((texel.x + texel.z * kGiDim) * (kGiMapSize * 2) + side * kGiMapSize + entry.x, (texel.y + slot * kGiDim) * kGiMapSize + entry.y);
}

vec2 velk_gi_map_uv(ivec2 at)
{
    return (vec2(at) + 0.5) / vec2(float(kGiDim * kGiDim * kGiMapSize * 2), float(kGiDim * kGiLevels * kGiMapSize));
}

vec2 velk_gi_oct(vec3 d)
{
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    if (d.z >= 0.0) return d.xy;
    return (vec2(1.0) - abs(d.yx)) * vec2(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0);
}

vec3 velk_gi_oct_dir(vec2 p)
{
    vec3 n = vec3(p, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0.0) {
        n.xy = (vec2(1.0) - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

// Where a point relative to the camera is on a level's lattice of probes, 0 to 31 inside.
vec3 velk_gi_lattice(int ladder, vec3 rel, vec3 cam)
{
    float cell = velk_gi_cell(ladder);
    vec3 cam_cells = cam / cell;
    return rel / cell + (cam_cells - floor(cam_cells)) + float(kGiDim / 2) - 0.5;
}

// How many cells a point is inside a level's lattice, negative outside it.
float velk_gi_edge(int ladder, vec3 rel, vec3 cam)
{
    vec3 g = velk_gi_lattice(ladder, rel, cam);
    return min(min(g.x, min(g.y, g.z)), float(kGiDim - 1) - max(g.x, max(g.y, g.z)));
}

// How many cells inside a level a point is for fading into the next level. Measured from the
// camera, so it moves smoothly with it where the grid moves in steps, and at most
// velk_gi_edge, so a fade is complete before the level ends.
float velk_gi_fade_edge(int ladder, vec3 rel)
{
    vec3 cells = abs(rel) / velk_gi_cell(ladder);
    return float(kGiDim / 2) - 1.5 - max(cells.x, max(cells.y, cells.z));
}

// The weight one of the eight probes around a point gets: 0 unless it holds this level's
// cell, fading out the more of its rays reach the inside of something, such as one under
// the ground. With a normal, probes behind the surface get none, since they cannot see
// what lights it.
float velk_gi_corner(int ladder, vec3 rel, vec3 cam, vec3 N, int o, uint atlas, out ivec3 texel, out vec4 first, out vec4 third)
{
    float cell_size = velk_gi_cell(ladder);
    vec3 cam_cells = cam / cell_size;
    vec3 cam_floor = floor(cam_cells);
    vec3 g = clamp(velk_gi_lattice(ladder, rel, cam), vec3(0.0), vec3(float(kGiDim - 1)));
    vec3 gbase = floor(g);
    vec3 gfrac = g - gbase;
    ivec3 off = ivec3(o & 1, (o >> 1) & 1, (o >> 2) & 1);
    ivec3 cell = ivec3(cam_floor) - kGiDim / 2 + min(ivec3(gbase) + off, ivec3(kGiDim - 1));
    vec3 tri = mix(vec3(1.0) - gfrac, gfrac, vec3(off));
    float w = tri.x * tri.y * tri.z;
    int slot = ladder & 7;
    texel = cell & ivec3(kGiDim - 1);
    first = vec4(0.0);
    third = vec4(0.0);
    if (w <= 0.0) return 0.0;
    first = velk_texture(atlas, velk_gi_uv(texel, slot, 0));
    if (first.a != velk_gi_stamp(cell, ladder)) return 0.0;
    third = velk_texture(atlas, velk_gi_uv(texel, slot, 2));
    if (dot(N, N) > 0.0) {
        vec3 probe = (vec3(cell) - cam_floor + 0.5 - (cam_cells - cam_floor)) * cell_size;
        w *= clamp(dot(probe - rel, N) / cell_size * 8.0, 0.0, 1.0);
    }
    return w * clamp(1.0 - third.a * 4.0, 0.0, 1.0);
}

// Trilinear SH of a level at a point, from the probes around it. Returns the weight found,
// 0 if none did, and sh is then undefined.
float velk_gi_sample(uint atlas, int ladder, vec3 rel, vec3 cam, vec3 N, out vec3 sh[4])
{
    for (int c = 0; c < 4; ++c) sh[c] = vec3(0.0);
    int slot = ladder & 7;
    float total = 0.0;
    for (int o = 0; o < 8; ++o) {
        ivec3 texel;
        vec4 first;
        vec4 third;
        float w = velk_gi_corner(ladder, rel, cam, N, o, atlas, texel, first, third);
        if (w <= 0.0) continue;
        sh[0] += first.rgb * w;
        sh[1] += velk_texture(atlas, velk_gi_uv(texel, slot, 1)).rgb * w;
        sh[2] += third.rgb * w;
        sh[3] += velk_texture(atlas, velk_gi_uv(texel, slot, 3)).rgb * w;
        total += w;
    }
    if (total > 0.0) {
        for (int c = 0; c < 4; ++c) sh[c] /= total;
    }
    return total;
}

// A map entry for a direction, bilinear between the entries around it.
vec4 velk_gi_map_read(uint maps, ivec3 texel, int slot, int side, vec3 dir)
{
    vec2 at = (velk_gi_oct(dir) * 0.5 + 0.5) * float(kGiMapSize) - 0.5;
    vec2 at_floor = floor(at);
    vec2 f = at - at_floor;
    vec4 sum = vec4(0.0);
    for (int k = 0; k < 4; ++k) {
        ivec2 corner = ivec2(k & 1, k >> 1);
        ivec2 entry = clamp(ivec2(at_floor) + corner, ivec2(0), ivec2(kGiMapSize - 1));
        vec2 weights = mix(vec2(1.0) - f, f, vec2(corner));
        sum += velk_texture(maps, velk_gi_map_uv(velk_gi_map_texel(texel, slot, entry, side))) * weights.x * weights.y;
    }
    return sum;
}

// The irradiance over pi a level holds against a normal, as DDGI blends it: each probe by
// how well it faces the surface and whether it sees the point. `rel` is the surface point
// and `biased` that point moved off it. Returns the weight found, 0 if no probe has this cell.
float velk_gi_sample_irradiance(uint atlas, uint maps, int ladder, vec3 rel, vec3 biased, vec3 cam, vec3 N, out vec3 irradiance)
{
    irradiance = vec3(0.0);
    int slot = ladder & 7;
    float spacing = velk_gi_cell(ladder);
    vec3 cam_cells = cam / spacing;
    vec3 cam_floor = floor(cam_cells);
    vec3 g = clamp(velk_gi_lattice(ladder, biased, cam), vec3(0.0), vec3(float(kGiDim - 1)));
    vec3 gbase = floor(g);
    vec3 gfrac = g - gbase;
    float total = 0.0;
    for (int o = 0; o < 8; ++o) {
        ivec3 off = ivec3(o & 1, (o >> 1) & 1, (o >> 2) & 1);
        ivec3 cell = ivec3(cam_floor) - kGiDim / 2 + min(ivec3(gbase) + off, ivec3(kGiDim - 1));
        ivec3 texel = cell & ivec3(kGiDim - 1);
        vec4 first = velk_texture(atlas, velk_gi_uv(texel, slot, 0));
        if (first.a != velk_gi_stamp(cell, ladder)) continue;
        if (velk_texture(atlas, velk_gi_uv(texel, slot, 2)).a > 0.25) continue;
#ifdef VELK_GI_DEBUG_SWITCHES
        if (velk_gi_ignore_young && velk_texture(atlas, velk_gi_uv(texel, slot, 1)).a < 8.0) continue;
#endif

        vec3 probe = (vec3(cell) - cam_floor + 0.5 - (cam_cells - cam_floor)) * spacing;
        vec3 to_probe = normalize(probe - rel);
        vec3 biased_to_probe = probe - biased;
        float distance_to_probe = length(biased_to_probe);
        vec3 biased_dir = biased_to_probe / max(distance_to_probe, 1e-5);

        vec3 tri = max(vec3(0.001), mix(vec3(1.0) - gfrac, gfrac, vec3(off)));

        // Wrap shading rather than a hard back-face test, so small details whose normals
        // rule out every probe that sees them are still lit.
        float wrap = (dot(to_probe, N) + 1.0) * 0.5;
        float weight = wrap * wrap + 0.2;

        vec2 moments = velk_gi_map_read(maps, texel, slot, 1, -biased_dir).rg;
        float variance = abs(moments.x * moments.x - moments.y);
        float chebyshev = 1.0;
        if (distance_to_probe > moments.x) {
            float v = distance_to_probe - moments.x;
            chebyshev = variance / (variance + v * v);
            chebyshev = max(chebyshev * chebyshev * chebyshev, 0.0);
        }
#ifdef VELK_GI_DEBUG_SWITCHES
        if (velk_gi_ignore_visibility) chebyshev = 1.0;
#endif
        // Never quite zero, for when no probe sees the point.
        weight *= max(0.05, chebyshev);
        weight = max(0.000001, weight);

        // Tiny weights are visible because perception is logarithmic, so they are crushed.
        const float kCrush = 0.2;
        if (weight < kCrush) weight *= weight * weight * (1.0 / (kCrush * kCrush));
        weight *= tri.x * tri.y * tri.z;

        // Decoded only part of the way, to blend in a perceptual space.
        vec3 stored = max(velk_gi_map_read(maps, texel, slot, 0, N).rgb, vec3(0.0));
        irradiance += weight * pow(stored, vec3(kGiIrradianceGamma * 0.5));
        total += weight;
    }
    if (total > 0.0) {
        irradiance /= total;
        irradiance *= irradiance;
    }
    return total;
}

// The level a surface point relative to the camera uses: the finest holding it. Also gives
// the point moved off the surface, towards the normal and the viewer as DDGI biases it, and
// how many cells inside that level it is. `view_from` is the direction from viewer to point.
int velk_gi_level_for(int finest, vec3 world_pos, vec3 N, vec3 view_from, vec3 cam, out vec3 shade_pos, out float edge)
{
    int level = 0;
    edge = 0.0;
    shade_pos = world_pos;
    for (; level < kGiLevels - 1; ++level) {
        float spacing = velk_gi_cell(finest + level);
        shade_pos = world_pos + N * (0.2 * spacing) - view_from * (0.6 * spacing);
        edge = velk_gi_edge(finest + level, shade_pos, cam);
        if (edge >= 0.0) break;
    }
    return level;
}

// The field's coefficients for a surface point relative to the camera. Uses the finest
// level holding the point, fading into the next one over the last two cells, and a coarser
// one where probes are yet to fill in. False if no probe could light it.
bool velk_gi_lookup(uint atlas, int finest, vec3 world_pos, vec3 N, vec3 cam, out vec3 sh[4])
{
    vec3 shade_pos;
    float edge;
    int level = velk_gi_level_for(finest, world_pos, N, normalize(world_pos), cam, shade_pos, edge);

    int used = level;
    float weight = velk_gi_sample(atlas, finest + used, shade_pos, cam, N, sh);
    while (weight <= 0.0 && used + 1 < kGiLevels) {
        ++used;
        weight = velk_gi_sample(atlas, finest + used, shade_pos, cam, N, sh);
    }
    if (weight <= 0.0) return false;

    float fade = velk_gi_fade_edge(finest + level, shade_pos);
    if (used == level && used + 1 < kGiLevels && fade < 2.0) {
        vec3 outer[4];
        if (velk_gi_sample(atlas, finest + used + 1, shade_pos, cam, N, outer) > 0.0) {
            float t = clamp(fade * 0.5, 0.0, 1.0);
            for (int c = 0; c < 4; ++c) sh[c] = mix(outer[c], sh[c], t);
        }
    }
    return true;
}

// Irradiance over pi from the field against a surface's normal, with the same choice of
// levels. Negative if no probe could light it.
vec3 velk_gi_irradiance(uint atlas, uint maps, int finest, vec3 world_pos, vec3 N, vec3 view_from, vec3 cam)
{
    vec3 shade_pos;
    float edge;
    int level = velk_gi_level_for(finest, world_pos, N, view_from, cam, shade_pos, edge);

    int used = level;
    vec3 irradiance;
    float weight = velk_gi_sample_irradiance(atlas, maps, finest + used, world_pos, shade_pos, cam, N, irradiance);
    while (weight <= 0.0 && used + 1 < kGiLevels) {
        ++used;
        weight = velk_gi_sample_irradiance(atlas, maps, finest + used, world_pos, shade_pos, cam, N, irradiance);
    }
    if (weight <= 0.0) return vec3(-1.0);

    float fade = velk_gi_fade_edge(finest + level, shade_pos);
    if (used == level && used + 1 < kGiLevels && fade < 2.0) {
        vec3 outer;
        if (velk_gi_sample_irradiance(atlas, maps, finest + used + 1, world_pos, shade_pos, cam, N, outer) > 0.0) {
            irradiance = mix(outer, irradiance, clamp(fade * 0.5, 0.0, 1.0));
        }
    }
    return max(irradiance, vec3(0.0));
}

vec3 velk_sh_radiance(vec3 sh[4], vec3 d)
{
    return 0.282095 * sh[0]
         + 0.488603 * (sh[1] * d.y + sh[2] * d.z + sh[3] * d.x);
}
)"

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
layout(set = 1, binding = 12, std430) readonly buffer VelkMeshNodes { VelkMeshNode data[]; } velk_mesh_nodes;
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
    uint direct_samples_id;    // 4         half-size lights picked and their shadow results; 0 = each pixel alone
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
    int gi_finest;             // 104 ladder index of the probe field's finest level
    uint light_loop_probe;     // 108 diagnostic: extra per-light ALU repeats
    uint gi_maps_id;           // 112 GI probe irradiance maps
} pc;

// A scene light, with its position relative to the camera.
Light velk_light_at(uint index)
{
    Light light = velk_lights.data[index];
    light.position.xyz -= VELK_GLOBALS.cam_pos.xyz;
    return light;
}

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
)" VELK_GI_FIELD_GLSL R"(
// Indirect diffuse from the GI probe field, as irradiance against a surface's normal, for
// a point relative to the camera. Negative if no probe could light it.
vec3 velk_gi_indirect(vec3 world_pos, vec3 N)
{
    if (pc.gi_probe_id == 0u || VELK_GLOBALS.bvh_node_count == 0u) return vec3(0.0);
    return velk_gi_irradiance(pc.gi_probe_id, pc.gi_maps_id, pc.gi_finest, world_pos, N, normalize(world_pos), VELK_GLOBALS.cam_pos.xyz);
}

// Light arriving from a direction, as the GI probe field knows it. Broad, since the field
// holds only a low-order approximation. Negative if no probe could light the point.
vec3 velk_gi_radiance(vec3 world_pos, vec3 N, vec3 dir)
{
    if (pc.gi_probe_id == 0u || VELK_GLOBALS.bvh_node_count == 0u) return vec3(-1.0);
    vec3 sh[4];
    if (!velk_gi_lookup(pc.gi_probe_id, pc.gi_finest, world_pos, N, VELK_GLOBALS.cam_pos.xyz, sh)) return vec3(-1.0);
    return max(velk_sh_radiance(sh, dir), vec3(0.0));
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

            vec3 bc = 0.5 * (node.aabb_min.xyz + node.aabb_max.xyz) + partition_origin - VELK_GLOBALS.cam_pos.xyz;
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
              vec3 translation = vec3(rec.inv_row0.w, rec.inv_row1.w, rec.inv_row2.w) + partition_origin - VELK_GLOBALS.cam_pos.xyz;
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
// Direct diffuse light from the lights nearby pixels picked, reusing the shadow rays they
// traced. One light per pixel comes out the colour of that light, so a pixel averages several.
// How far it looks follows how many lights share its light: where one light dominates it
// keeps to the closest samples, so that light's shadows stay sharp. Alpha is 0 if no sample
// could be used.
vec4 velk_shared_direct(ivec2 coord, vec3 world_pos, vec3 N, vec3 albedo_rgb, float total_w, float total_w2, uint seed)
{
    if (total_w <= 0.0) return vec4(0.0, 0.0, 0.0, 1.0);
    const int kSamples = 8;
    const float kNearest = 1.5;
    const float kFarthest = 8.0;
    const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

    float lights_in_effect = total_w * total_w / max(total_w2, 1e-12);
    float reach = mix(kNearest, kFarthest, clamp((lights_in_effect - 1.0) / 3.0, 0.0, 1.0));
    ivec2 own = coord / 2;
    ivec2 half_size = ivec2((int(pc.width) + 1) / 2, (int(pc.height) + 1) / 2);
    float depth = length(world_pos);

    vec3 sum = vec3(0.0);
    float used = 0.0;
    for (int k = 0; k < kSamples; ++k) {
        ivec2 q = own;
        if (k > 0) {
            seed ^= seed >> 16; seed *= 0x7feb352du;
            seed ^= seed >> 15; seed *= 0x846ca68bu;
            seed ^= seed >> 16;
            float angle = float(seed) * (6.28318531 / 4294967296.0);
            seed ^= seed >> 16; seed *= 0x7feb352du;
            seed ^= seed >> 15; seed *= 0x846ca68bu;
            seed ^= seed >> 16;
            float radius = sqrt(float(seed) * (1.0 / 4294967296.0)) * reach;
            q += ivec2(round(vec2(cos(angle), sin(angle)) * radius));
            if (q.x < 0 || q.y < 0 || q.x >= half_size.x || q.y >= half_size.y) continue;
        }
        vec4 picked = texelFetch(velk_textures[nonuniformEXT(pc.direct_samples_id)], q, 0);
        if (picked.a <= 0.0) continue;
        vec3 q_normal = texelFetch(velk_textures[nonuniformEXT(pc.normal_tex_id)], q * 2, 0).xyz;
        vec3 q_pos = texelFetch(velk_textures[nonuniformEXT(pc.worldpos_tex_id)], q * 2, 0).xyz;
        if (dot(q_normal, q_normal) < 1e-6) continue;
        if (dot(N, normalize(q_normal)) < 0.9 || abs(dot(q_pos - world_pos, N)) > 0.02 * depth) continue;
        used += 1.0;

        Light light = velk_light_at(pc.lights_base + uint(picked.r));
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
        float w = dot(albedo_rgb * NdotL * radiance, kLuma);
        if (w <= 0.0) continue;
        // Weighted by how likely either pixel was to pick it, so that a light strong
        // over there and weak here does not count as if this pixel had picked it.
        sum += NdotL * radiance * picked.g / (0.5 * (w / total_w + picked.b));
    }
    return used > 0.0 ? vec4(sum / used, 1.0) : vec4(0.0);
}

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
            if (pc.direct_samples_id != 0u) imageStore(gStorageImagesF32[nonuniformEXT(pc.direct_samples_id)], coord / 2, vec4(0.0));
        } else {
            if (pc.direct_samples_id != 0u) imageStore(gStorageImagesF16[nonuniformEXT(pc.irr_image_id)], coord, vec4(0.0));
            imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(sky, 1.0));
            imageStore(gStorageImagesF16[nonuniformEXT(pc.unshadowed_image_id)], coord, vec4(0.0));
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
    // How much of `rest` is the lights' highlights, which composite shadows by the ratio.
    float highlight_share = 0.0;
    float irr_valid = 1.0;
    if (lighting_mode == 0u) {
        // Unlit: emit albedo as-is (no diffuse irradiance).
        rest = albedo.rgb;
    } else {
        vec3 N = normalize(world_n);
        vec3 V = normalize(-world_pos);
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
        float total_w2 = 0.0;
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
            Light light = velk_light_at(pc.lights_base + i);
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
            total_w2 += w * w;
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
        float pixel_width = length(world_pos) * 2.0 / (footprint_f * float(pc.height))
            / sqrt(max(NdotV, 0.05));

        vec3 direct_diffuse = vec3(0.0);
        float picked_shadow = 0.0;
        if (trace_now && res_light != 0xffffffffu && res_w > 0.0) {
            Light chosen = velk_light_at(pc.lights_base + res_light);
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
            picked_shadow = shadow;
        }

        // Rays only find out what each traced pixel picked and whether it is lit. Every pixel
        // then works out its own light from those.
        float shared_valid = 1.0;
        if (pc.direct_samples_id != 0u) {
            if (trace_now) {
                bool picked = res_light != 0xffffffffu && res_w > 0.0;
                imageStore(gStorageImagesF32[nonuniformEXT(pc.direct_samples_id)], coord / 2,
                           picked ? vec4(float(res_light), picked_shadow, res_w / total_w, 1.0) : vec4(0.0));
                return;
            }
            vec4 shared_direct = velk_shared_direct(coord, world_pos, N, albedo.rgb, total_w, total_w2, seed);
            direct_diffuse = shared_direct.rgb;
            shared_valid = shared_direct.a;
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
        // Where no probe could light the surface, the environment stands in.
        vec3 ambient_unshadowed = has_gi && gi.r >= 0.0 ? gi : env_diffuse;
        vec3 ambient_shadowed   = ambient_unshadowed * ao;
        unshadowed_out = unshadowed_direct + ambient_unshadowed;
        vec3 shadowed_total = direct_diffuse + ambient_shadowed;
        irr = shadowed_total / max(unshadowed_out, vec3(1e-4));
        // Alpha marks "this pixel traced a ray this frame". The temporal pass
        // keeps history untouched where it is 0 rather than folding in a zero.
        irr_valid = pc.direct_samples_id != 0u ? shared_valid : (trace_now ? 1.0 : 0.0);
        // The environment map cannot tell whether the sky is visible along the reflection.
        // The field can: how much of the environment's light it finds that way is how much
        // of the sharp reflection stays, and what it finds in place of the sky fills the rest.
        vec3 reflected = reflect(-V, N);
        vec3 field_reflection = velk_gi_radiance(world_pos, N, reflected);
        if (field_reflection.r >= 0.0) {
            const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
            float env_broad = dot(env_miss_color_lod(reflected, env_max_lod), kLuma);
            float sky_seen = env_broad > 1e-4 ? clamp(dot(field_reflection, kLuma) / env_broad, 0.0, 1.0) : 0.0;
            env_specular = env_specular * sky_seen + field_reflection * (1.0 - sky_seen);
        }
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
        const vec3 kHighlightLuma = vec3(0.2126, 0.7152, 0.0722);
        float rest_luma = dot(rest, kHighlightLuma);
        highlight_share = rest_luma > 1e-5 ? clamp(dot(direct_specular, kHighlightLuma) / rest_luma, 0.0, 1.0) : 0.0;
    }

    // Emissive is additive radiance; part of the sharp output.
    rest += emissive;

    // Two outputs (raw HDR linear): "rest" (sharp) on output_image_id, diffuse
    // irradiance on irr_image_id. The denoise/composite pass reprojects +
    // accumulates the irradiance and folds albedo*(1-metallic)*irr into rest.
    if (pc.ray_pass != 0u) {
        imageStore(gStorageImagesF16[nonuniformEXT(pc.irr_image_id)], coord, vec4(irr, irr_valid));
        if (pc.direct_samples_id != 0u) imageStore(gStorageImagesF32[nonuniformEXT(pc.direct_samples_id)], coord / 2, vec4(0.0));
        return;
    }
    if (pc.direct_samples_id != 0u) imageStore(gStorageImagesF16[nonuniformEXT(pc.irr_image_id)], coord, vec4(irr, irr_valid));
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(rest, albedo.a));
    // The unshadowed term the composite multiplies the denoised ratio by. Kept
    // out of the denoiser deliberately: it is exact and sharp, and it carries
    // the high-frequency detail (light falloff, N.L) that must not be blurred.
    imageStore(gStorageImagesF16[nonuniformEXT(pc.unshadowed_image_id)], coord,
               vec4(unshadowed_out, highlight_share));
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
        // Positions are relative to the camera, which was elsewhere then.
        vec3 prev_pos = world_pos + VELK_GLOBALS.cam_moved;
        vec4 prev_clip = VELK_GLOBALS.prev_view_projection * vec4(prev_pos, 1.0);
        if (prev_clip.w > 1e-6) {
            vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
            if (all(greaterThanEqual(prev_uv, vec2(0.0))) &&
                all(lessThanEqual(prev_uv, vec2(1.0)))) {
                vec4 hp = velk_texture(pc.hist_pos_prev_id, prev_uv);
                vec4 hi = velk_texture(pc.hist_irr_prev_id, prev_uv);
                // Same surface? World-space distance, tolerance scaled by depth.
                if (hp.w > 0.5 && length(hp.xyz - prev_pos) < 0.03 * max(prev_clip.w, 1.0)) {
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

    float depth = max(length(world_pos), 1e-3);

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
        float depth = max(length(world_pos), 1e-3);
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
    vec4 unshadowed_share = velk_texture(pc.unshadowed_id, uv);
    vec3 unshadowed = unshadowed_share.rgb;
    // The lights' highlights, in the alpha as a share of `rest`, are shadowed like their diffuse.
    float seen = clamp(dot(irr_out.rgb, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    vec3 sharp = rest.rgb * (1.0 - unshadowed_share.a * (1.0 - seen));
    vec3 final = albedo.rgb * (1.0 - metallic) * (unshadowed * irr_out) + sharp;
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
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];
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

#ifdef VELK_PROBE_PASS
vec3 velk_hit_irradiance(vec3 pos, vec3 N);
#endif

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
#ifdef VELK_PROBE_PASS
    // A probe's hit is lit by the field itself, which sees what is really around it and
    // carries light over further bounces as frames go by.
    vec3 from_field = velk_hit_irradiance(ctx.rel_pos, N);
    if (from_field.r >= 0.0) env_at_normal = from_field;
#endif
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
        ctx.rel_pos = ray.origin + hit.t * ray.dir;
        ctx.hit_pos = ctx.rel_pos + VELK_GLOBALS.cam_pos.xyz;
        ctx.tangent = vec4(0.0); // RT has no tangent basis (no normal mapping)
        BrdfSample bs = velk_resolve_fill(s.material_id, ctx);

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
        velk_ray_frame = VELK_GLOBALS.cam_pos.xyz;

        for (uint i = 0u; i < shape_count; ++i) {
            RtShape s = velk_shapes.data[pc.shapes_base + i];
            RayHit hit;
            if (!trace_shape(primary, s.primary_record, s, hit)) continue;

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
            ctx.rel_pos = primary.origin + hit.t * primary.dir;
            ctx.hit_pos = ctx.rel_pos + VELK_GLOBALS.cam_pos.xyz;
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
                refl.origin = ctx.rel_pos +hit.normal * 1e-3;
                refl.dir = bs.next_dir;
                bounce += trace_bounce(refl, bs.throughput);
                for (uint sp = 1u; sp < spp; ++sp) {
                    BrdfSample bs2 = velk_resolve_fill(s.material_id, ctx);
                    refl.origin = ctx.rel_pos +hit.normal * 1e-3;
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
        RtShape s = velk_bvh_shapes.data[hit.shape_index];
        EvalContext ctx;
        ctx.material_base = s.material_base;
        ctx.texture_id = s.texture_id;
        ctx.shape_param = s.shape_param;
        ctx.uv = hit.uv;
        ctx.uv1 = hit.uv;
        ctx.base = s.color;
        ctx.ray_dir = ray.dir;
        ctx.normal = hit.normal;
        ctx.rel_pos = ray.origin + hit.t * ray.dir;
        ctx.hit_pos = ctx.rel_pos + VELK_GLOBALS.cam_pos.xyz;
        ctx.tangent = vec4(0.0);

        reference_pick_light(ctx.rel_pos, hit.normal);
        velk_hit_diffuse = vec3(0.0);
        BrdfSample bs = velk_resolve_fill(s.material_id, ctx);

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

/// Diagnostic: for each pixel asked for and each level, takes the probe that counts most at
/// that surface and follows true paths from that probe's own position, against the pixel's
/// normal, to compare with what the probe holds. The words at `extras.x` are 12 of pixels, the
/// normal and position textures, the atlas and the finest level, then 5 per level per pixel:
/// weight, what the probe holds (green), first bounce sum, whole path sum, count. `extras.w`
/// is the pixel count and `env` is as in rt_reference_main_src.
inline constexpr string_view gi_truth_main_src = VELK_GI_FIELD_GLSL VELK_REFERENCE_PATH_GLSL R"(
layout(set = 1, binding = 16, std430) buffer TruthWords { uint data[]; } truth_words;

void main()
{
    reference_sky_cap = 5.0;
    if (gl_LocalInvocationIndex != 0u) return;
    uint index = gl_WorkGroupID.x;
    uint base = pc.extras.x;
    if (index >= pc.extras.w) return;
    ivec2 pixel = ivec2(truth_words.data[base + index * 2u], truth_words.data[base + index * 2u + 1u]);
    vec3 N = texelFetch(velk_textures[nonuniformEXT(truth_words.data[base + 12u])], pixel, 0).xyz;
    if (dot(N, N) < 1e-6) return;
    N = normalize(N);
    vec3 rel = texelFetch(velk_textures[nonuniformEXT(truth_words.data[base + 13u])], pixel, 0).xyz;
    uint atlas = truth_words.data[base + 14u];
    int finest = int(truth_words.data[base + 15u]);
    vec3 cam = VELK_GLOBALS.cam_pos.xyz;
    vec3 up = abs(N.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);

    for (int level = 0; level < kGiLevels; ++level) {
        int ladder = finest + level;
        int slot = ladder & 7;
        float spacing = velk_gi_cell(ladder);
        vec3 point = rel + N * (0.05 * spacing);
        vec3 cam_cells = cam / spacing;
        vec3 cam_floor = floor(cam_cells);
        vec3 g = clamp(velk_gi_lattice(ladder, point, cam), vec3(0.0), vec3(float(kGiDim - 1)));
        float best = 0.0;
        float holds = 0.0;
        vec3 origin = point;
        for (int o = 0; o < 8; ++o) {
            ivec3 texel;
            vec4 first;
            vec4 third;
            float w = velk_gi_corner(ladder, point, cam, N, o, atlas, texel, first, third);
            if (w > best) {
                best = w;
                ivec3 off = ivec3(o & 1, (o >> 1) & 1, (o >> 2) & 1);
                ivec3 cell = ivec3(cam_floor) - kGiDim / 2 + min(ivec3(floor(g)) + off, ivec3(kGiDim - 1));
                origin = (vec3(cell) - cam_floor + 0.5 - (cam_cells - cam_floor)) * spacing;
                vec3 second = velk_texture(atlas, velk_gi_uv(texel, slot, 1)).rgb;
                vec3 fourth = velk_texture(atlas, velk_gi_uv(texel, slot, 3)).rgb;
                holds = 0.282095 * first.g + 0.325735 * (second.g * N.y + third.g * N.z + fourth.g * N.x);
            }
        }

        rng_init(uvec2(pixel) + uvec2(uint(level) * 131u, 0u), VELK_GLOBALS.present_counter);
        // The same directions and reach as the probe readout, split the same way.
        const uint kPaths = 64u;
        float t_max = level + 1 < kGiLevels ? spacing : 1e30;
        float first_sum = 0.0;
        float whole_sum = 0.0;
        float added = 0.0;
        float near_count = 0.0;
        float near_first = 0.0;
        float near_whole = 0.0;
        float far_whole = 0.0;
        for (uint i = 0u; i < kPaths; ++i) {
            float u = (float(i) + 0.5) / float(kPaths);
            float phi = float(i) * 2.39996323;
            float radius = sqrt(u);
            Ray ray;
            ray.origin = origin;
            ray.dir = T * (radius * cos(phi)) + B * (radius * sin(phi)) + N * sqrt(1.0 - u);
            RayHit hit;
            bool near_hit = trace_closest_hit(ray, t_max, hit) && hit.t < t_max;
            vec3 first_hit;
            vec3 whole = reference_path(ray, first_hit);
            if (any(isnan(whole)) || any(isinf(whole))) continue;
            first_sum += first_hit.g;
            whole_sum += whole.g;
            added += 1.0;
            if (near_hit) {
                near_count += 1.0;
                near_first += first_hit.g;
                near_whole += whole.g;
            } else {
                far_whole += whole.g;
            }
        }

        // True paths over the whole sphere: the mean, the mean along the normal for L1, and
        // the mean against the cosine, turned a different way each frame.
        float turn = rng_next_float() * 6.28318531;
        float sphere_sum = 0.0;
        float sphere_n_sum = 0.0;
        float sphere_cos_sum = 0.0;
        float sphere_added = 0.0;
        for (uint i = 0u; i < kPaths; ++i) {
            float along = 1.0 - 2.0 * (float(i) + 0.5) / float(kPaths);
            float around = float(i) * 2.39996323 + turn;
            float flat_len = sqrt(max(1.0 - along * along, 0.0));
            Ray ray;
            ray.origin = origin;
            ray.dir = T * (flat_len * cos(around)) + B * (flat_len * sin(around)) + N * along;
            vec3 first_hit;
            vec3 whole = reference_path(ray, first_hit);
            if (any(isnan(whole)) || any(isinf(whole))) continue;
            sphere_sum += whole.g;
            sphere_n_sum += whole.g * along;
            sphere_cos_sum += whole.g * max(along, 0.0);
            sphere_added += 1.0;
        }

        uint at = base + 16u + index * 104u + uint(level) * 13u;
        truth_words.data[at + 9u] = floatBitsToUint(uintBitsToFloat(truth_words.data[at + 9u]) + sphere_sum);
        truth_words.data[at + 10u] = floatBitsToUint(uintBitsToFloat(truth_words.data[at + 10u]) + sphere_n_sum);
        truth_words.data[at + 11u] = floatBitsToUint(uintBitsToFloat(truth_words.data[at + 11u]) + sphere_cos_sum);
        truth_words.data[at + 12u] = floatBitsToUint(uintBitsToFloat(truth_words.data[at + 12u]) + sphere_added);
        truth_words.data[at] = floatBitsToUint(best);
        truth_words.data[at + 1u] = floatBitsToUint(holds);
        truth_words.data[at + 2u] = floatBitsToUint(uintBitsToFloat(truth_words.data[at + 2u]) + first_sum);
        truth_words.data[at + 3u] = floatBitsToUint(uintBitsToFloat(truth_words.data[at + 3u]) + whole_sum);
        truth_words.data[at + 4u] = floatBitsToUint(uintBitsToFloat(truth_words.data[at + 4u]) + added);
        truth_words.data[at + 5u] = floatBitsToUint(uintBitsToFloat(truth_words.data[at + 5u]) + near_count);
        truth_words.data[at + 6u] = floatBitsToUint(uintBitsToFloat(truth_words.data[at + 6u]) + near_first);
        truth_words.data[at + 7u] = floatBitsToUint(uintBitsToFloat(truth_words.data[at + 7u]) + near_whole);
        truth_words.data[at + 8u] = floatBitsToUint(uintBitsToFloat(truth_words.data[at + 8u]) + far_whole);
    }
}
)";

/// Diagnostic: the true irradiance over pi at each pixel asked for, to compare with the GI
/// field. Adds a few cosine-weighted paths from the pixel's surface every frame to running
/// sums. Goes after compose_rt_compute's shared body. `extras` is the word base (12 words of
/// pixels, then 7 for each: first bounce sum, whole path sum, count), the normal and position
/// textures, and the pixel count. `env` is as in rt_reference_main_src.
inline constexpr string_view gi_reference_main_src = VELK_REFERENCE_PATH_GLSL R"(
layout(set = 1, binding = 16, std430) buffer ReferenceWords { uint data[]; } reference_words;

void main()
{
    reference_sky_cap = 5.0;
    if (gl_LocalInvocationIndex != 0u) return;
    uint index = gl_WorkGroupID.x;
    if (index >= pc.extras.w) return;
    ivec2 pixel = ivec2(reference_words.data[pc.extras.x + index * 2u], reference_words.data[pc.extras.x + index * 2u + 1u]);
    vec3 N = texelFetch(velk_textures[nonuniformEXT(pc.extras.y)], pixel, 0).xyz;
    if (dot(N, N) < 1e-6) return;
    N = normalize(N);
    vec3 rel = texelFetch(velk_textures[nonuniformEXT(pc.extras.z)], pixel, 0).xyz;
    rng_init(uvec2(pixel), VELK_GLOBALS.present_counter);

    const uint kPaths = 32u;
    vec3 first_sum = vec3(0.0);
    vec3 whole_sum = vec3(0.0);
    float added = 0.0;
    for (uint i = 0u; i < kPaths; ++i) {
        Ray ray;
        ray.origin = rel + N * 0.01;
        ray.dir = reference_cosine(N);
        vec3 first;
        vec3 whole = reference_path(ray, first);
        if (any(isnan(whole)) || any(isinf(whole))) continue;
        first_sum += first;
        whole_sum += whole;
        added += 1.0;
    }

    uint at = pc.extras.x + 12u + index * 7u;
    for (uint c = 0u; c < 3u; ++c) {
        reference_words.data[at + c] = floatBitsToUint(uintBitsToFloat(reference_words.data[at + c]) + first_sum[c]);
        reference_words.data[at + 3u + c] = floatBitsToUint(uintBitsToFloat(reference_words.data[at + 3u + c]) + whole_sum[c]);
    }
    reference_words.data[at + 6u] = floatBitsToUint(uintBitsToFloat(reference_words.data[at + 6u]) + added);
}
)";

// World-space GI probes. Composed with the RT body, so probe hits get the same
// material evaluation primary rays do - real albedo and emission rather than a
// vertex-tint approximation.
//
/// Diagnostic: writes what the GI probe field holds at a point, against a normal, as irradiance
/// over pi: first as used for shading, then each level's weight and value.
inline constexpr string_view gi_print_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 1) in;
layout(set = 1, binding = 16, std430) buffer PrintWords { uint data[]; } print_words;

)" VELK_GI_FIELD_GLSL R"(
layout(push_constant, scalar) uniform PC {
    vec4 point;
    vec4 normal;
    uint word;
    uint atlas;
    uint maps;
    uint globals_base;
    int finest;
} pc;

void put(uint at, vec3 v)
{
    print_words.data[pc.word + at] = floatBitsToUint(v.x);
    print_words.data[pc.word + at + 1u] = floatBitsToUint(v.y);
    print_words.data[pc.word + at + 2u] = floatBitsToUint(v.z);
}

void main()
{
    GlobalData g = velk_globals.data[pc.globals_base];
    vec3 cam = g.cam_pos.xyz;
    vec3 rel = pc.point.xyz - cam;
    vec3 N = pc.normal.xyz;
    put(0u, velk_gi_irradiance(pc.atlas, pc.maps, pc.finest, rel, N, normalize(rel), cam));
    for (int level = 0; level < kGiLevels; ++level) {
        vec3 sh[4];
        vec3 at = rel + N * (0.05 * velk_gi_cell(pc.finest + level));
        float weight = velk_gi_sample(pc.atlas, pc.finest + level, at, cam, N, sh);
        vec3 e = weight > 0.0 ? 0.282095 * sh[0] + 0.325735 * (sh[1] * N.y + sh[2] * N.z + sh[3] * N.x) : vec3(0.0);
        print_words.data[pc.word + 3u + uint(level) * 4u] = floatBitsToUint(weight);
        put(4u + uint(level) * 4u, e);
    }
}
)";

/// Shows the GI probe field in place of the image. Mode 1: what the field gives each surface,
/// without albedo. Mode 2: the level in use as a flat colour, with its cells as a checker.
/// Mode 3: of the eight probes around a surface, green if all hold this cell, yellow if some
/// do, red if none. Mode 4: of those holding it, green if all see the surface, yellow if some
/// do, red if none, blue if no probe holds it. Mode 5: the youngest of those holding it, red
/// if at most 3 updates old, yellow if under 16, green if older, blue if no probe holds it.
/// Mode 6: as mode 1, with every probe counting whether or not it sees the surface.
/// Mode 7: as mode 1, without probes under 8 updates old.
inline constexpr string_view gi_debug_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

#define VELK_GI_DEBUG_SWITCHES
bool velk_gi_ignore_visibility = false;
bool velk_gi_ignore_young = false;
)" VELK_GI_FIELD_GLSL R"(
layout(push_constant, scalar) uniform PC {
    uint atlas;
    uint maps;
    uint globals_base;
    int finest;
    uint normal_id;
    uint pos_id;
    uint output_id;
    uint width;
    uint height;
    uint mode;
} pc;

const vec3 kGreen = vec3(0.0, 1.0, 0.0);
const vec3 kYellow = vec3(1.0, 1.0, 0.0);
const vec3 kRed = vec3(1.0, 0.0, 0.0);
const vec3 kBlue = vec3(0.0, 0.2, 1.0);

const vec3 kLevelColors[8] = vec3[8](
    vec3(1.0, 0.0, 0.0), vec3(1.0, 0.5, 0.0), vec3(1.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0),
    vec3(0.0, 1.0, 1.0), vec3(0.0, 0.3, 1.0), vec3(0.6, 0.0, 1.0), vec3(1.0, 0.0, 0.7));

vec3 velk_gi_debug_color(vec3 rel, vec3 N, vec3 cam)
{
    if (pc.mode == 1u || pc.mode == 6u || pc.mode == 7u) {
        velk_gi_ignore_visibility = pc.mode == 6u;
        velk_gi_ignore_young = pc.mode == 7u;
        return max(velk_gi_irradiance(pc.atlas, pc.maps, pc.finest, rel, N, normalize(rel), cam), vec3(0.0));
    }

    vec3 shade_pos;
    float edge;
    int level = velk_gi_level_for(pc.finest, rel, N, normalize(rel), cam, shade_pos, edge);
    int ladder = pc.finest + level;
    int slot = ladder & 7;
    float spacing = velk_gi_cell(ladder);
    vec3 cam_cells = cam / spacing;
    vec3 cam_floor = floor(cam_cells);
    vec3 g = clamp(velk_gi_lattice(ladder, shade_pos, cam), vec3(0.0), vec3(float(kGiDim - 1)));
    vec3 gbase = floor(g);

    if (pc.mode == 2u) {
        ivec3 world_cell = ivec3(cam_floor) + ivec3(gbase);
        return kLevelColors[level] * (((world_cell.x + world_cell.y + world_cell.z) & 1) != 0 ? 0.4 : 1.0);
    }

    int holding = 0;
    int seeing = 0;
    float youngest = 1e9;
    for (int o = 0; o < 8; ++o) {
        ivec3 off = ivec3(o & 1, (o >> 1) & 1, (o >> 2) & 1);
        ivec3 cell = ivec3(cam_floor) - kGiDim / 2 + min(ivec3(gbase) + off, ivec3(kGiDim - 1));
        ivec3 texel = cell & ivec3(kGiDim - 1);
        if (velk_texture(pc.atlas, velk_gi_uv(texel, slot, 0)).a != velk_gi_stamp(cell, ladder)) continue;
        ++holding;
        youngest = min(youngest, velk_texture(pc.atlas, velk_gi_uv(texel, slot, 1)).a);
        if (velk_texture(pc.atlas, velk_gi_uv(texel, slot, 2)).a > 0.25) continue;

        vec3 probe = (vec3(cell) - cam_floor + 0.5 - (cam_cells - cam_floor)) * spacing;
        vec3 to_probe = probe - shade_pos;
        float dist = length(to_probe);
        vec2 moments = velk_gi_map_read(pc.maps, texel, slot, 1, -to_probe / max(dist, 1e-5)).rg;
        float seen = 1.0;
        if (dist > moments.x) {
            float variance = abs(moments.x * moments.x - moments.y);
            float v = dist - moments.x;
            seen = variance / (variance + v * v);
            seen = seen * seen * seen;
        }
        if (seen >= 0.5) ++seeing;
    }

    if (pc.mode == 3u) return holding == 8 ? kGreen : holding > 0 ? kYellow : kRed;
    if (holding == 0) return kBlue;
    if (pc.mode == 5u) return youngest <= 3.0 ? kRed : youngest < 16.0 ? kYellow : kGreen;
    return seeing == holding ? kGreen : seeing > 0 ? kYellow : kRed;
}

void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.width) || p.y >= int(pc.height)) return;
    vec3 normal = texelFetch(velk_textures[nonuniformEXT(pc.normal_id)], p, 0).xyz;
    vec3 color = vec3(0.0);
    if (dot(normal, normal) >= 1e-6) {
        vec3 rel = texelFetch(velk_textures[nonuniformEXT(pc.pos_id)], p, 0).xyz;
        color = velk_gi_debug_color(rel, normalize(normal), velk_globals.data[pc.globals_base].cam_pos.xyz);
    }
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], p, vec4(color, 1.0));
}
)";

/// Measures a frame for render tests. Mode 0 clears this frame's words. Mode 1 follows each
/// surface back to the previous frame and adds up how much its lighting changed, so camera
/// motion does not count and surfaces that moved are skipped, and counts surfaces no probe
/// could light. `words` holds: valid, summed change times 256, over 5%, over 20%, the same four
/// for surfaces facing up or down, tested and missing, then per screen tile valid and summed.
inline constexpr string_view test_diagnostics_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 2, rgba32f) uniform writeonly image2D gStorageImagesF32[];
layout(set = 1, binding = 16, std430) buffer TestWords { uint data[]; } test_words;

)" VELK_GI_FIELD_GLSL R"(
layout(push_constant, scalar) uniform PC {
    uint word;
    uint words;
    uint mode;
    uint globals_base;
    uint atlas;
    uint maps;
    int finest;
    uint normal_id;
    uint pos_id;
    uint albedo_id;
    uint color_id;
    uint history_id;
    uint output_id;
    uint width;
    uint height;
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
    imageStore(gStorageImagesF32[nonuniformEXT(pc.output_id)], p, vec4(rel, light));

    GlobalData g = velk_globals.data[pc.globals_base];
    if (pc.atlas != 0u && ((p.x + p.y) & 3) == 0) {
        add(8u, 1u);
        if (velk_gi_irradiance(pc.atlas, pc.maps, pc.finest, rel, N, normalize(rel), g.cam_pos.xyz).r < 0.0) add(9u, 1u);
    }

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
    uint tile = min(uint(p.y) * kTilesY / pc.height, kTilesY - 1u) * kTilesX + min(uint(p.x) * kTilesX / pc.width, kTilesX - 1u);
    add(10u + tile, 1u);
    add(10u + kTilesX * kTilesY + tile, scaled);
}
)";

/// Diagnostic: for each pixel asked for, writes 70 words: the GI field's irradiance over pi,
/// the sky along the normal at its broadest, then from the frame's textures the albedo, the
/// final HDR colour, the lighting before shadows, and the normal.
inline constexpr string_view gi_pixels_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 1) in;
layout(set = 1, binding = 16, std430) buffer PrintWords { uint data[]; } print_words;

)" VELK_GI_FIELD_GLSL R"(
layout(push_constant, scalar) uniform PC {
    uint word;
    uint atlas;
    uint maps;
    uint globals_base;
    int finest;
    uint albedo_id;
    uint normal_id;
    uint pos_id;
    uint hdr_id;
    uint unshadowed_id;
    uint env_id;
    float intensity;
    float rotation;
    uint count;
    ivec2 pixels[6];
} pc;

void put(uint at, vec3 v)
{
    print_words.data[at] = floatBitsToUint(v.x);
    print_words.data[at + 1u] = floatBitsToUint(v.y);
    print_words.data[at + 2u] = floatBitsToUint(v.z);
}

void main()
{
    uint i = gl_WorkGroupID.x;
    if (i >= pc.count) return;
    ivec2 p = pc.pixels[i];
    uint at = pc.word + i * 70u;
    GlobalData g = velk_globals.data[pc.globals_base];
    vec3 N = normalize(texelFetch(velk_textures[nonuniformEXT(pc.normal_id)], p, 0).xyz);
    vec3 rel = texelFetch(velk_textures[nonuniformEXT(pc.pos_id)], p, 0).xyz;
    put(at, velk_gi_irradiance(pc.atlas, pc.maps, pc.finest, rel, N, normalize(rel), g.cam_pos.xyz));

    vec3 sky = vec3(0.0);
    if (pc.env_id != 0u) {
        float c = cos(pc.rotation);
        float s = sin(pc.rotation);
        vec3 dir = vec3(c * N.x + s * N.z, N.y, -s * N.x + c * N.z);
        vec2 uv = vec2(atan(dir.z, dir.x) / 6.28318531 + 0.5, asin(clamp(dir.y, -1.0, 1.0)) / 3.14159265 + 0.5);
        float lod = float(textureQueryLevels(velk_textures[nonuniformEXT(pc.env_id)])) - 1.0;
        sky = textureLod(velk_textures[nonuniformEXT(pc.env_id)], uv, lod).rgb * pc.intensity;
    }
    put(at + 3u, sky);
    put(at + 6u, texelFetch(velk_textures[nonuniformEXT(pc.albedo_id)], p, 0).rgb);
    put(at + 9u, texelFetch(velk_textures[nonuniformEXT(pc.hdr_id)], p, 0).rgb);
    put(at + 12u, texelFetch(velk_textures[nonuniformEXT(pc.unshadowed_id)], p, 0).rgb);
    put(at + 15u, N);

    // Then the point, the level a surface there uses, what each level holds for it (green),
    // and that level's eight probes: blend weight, whether it holds this cell, the share of
    // its rays that hit the inside of something, its green irradiance against the normal, and
    // its height above the surface in cells.
    vec3 cam = g.cam_pos.xyz;
    put(at + 18u, rel);
    int level = 0;
    vec3 shade = rel;
    for (; level < kGiLevels - 1; ++level) {
        shade = rel + N * (0.05 * velk_gi_cell(pc.finest + level));
        if (velk_gi_edge(pc.finest + level, shade, cam) >= 0.0) break;
    }
    print_words.data[at + 21u] = floatBitsToUint(float(level));
    for (int l = 0; l < kGiLevels; ++l) {
        vec3 sh[4];
        float weight = velk_gi_sample(pc.atlas, pc.finest + l, rel + N * (0.05 * velk_gi_cell(pc.finest + l)), cam, N, sh);
        float e = weight > 0.0 ? 0.282095 * sh[0].g + 0.325735 * (sh[1].g * N.y + sh[2].g * N.z + sh[3].g * N.x) : -1.0;
        print_words.data[at + 22u + uint(l)] = floatBitsToUint(e);
    }

    int ladder = pc.finest + level;
    int slot = ladder & 7;
    float cs = velk_gi_cell(ladder);
    vec3 cam_cells = cam / cs;
    vec3 cam_floor = floor(cam_cells);
    vec3 lattice = clamp(velk_gi_lattice(ladder, shade, cam), vec3(0.0), vec3(float(kGiDim - 1)));
    vec3 gbase = floor(lattice);
    vec3 gfrac = lattice - gbase;
    ivec3 base = ivec3(gbase);
    ivec3 corner = ivec3(cam_floor) - kGiDim / 2;
    for (int o = 0; o < 8; ++o) {
        ivec3 off = ivec3(o & 1, (o >> 1) & 1, (o >> 2) & 1);
        ivec3 cell = corner + min(base + off, ivec3(kGiDim - 1));
        vec3 tri = mix(vec3(1.0) - gfrac, gfrac, vec3(off));
        ivec3 texel = cell & ivec3(kGiDim - 1);
        vec4 first = velk_texture(pc.atlas, velk_gi_uv(texel, slot, 0));
        vec4 third = velk_texture(pc.atlas, velk_gi_uv(texel, slot, 2));
        vec3 second = velk_texture(pc.atlas, velk_gi_uv(texel, slot, 1)).rgb;
        vec3 fourth = velk_texture(pc.atlas, velk_gi_uv(texel, slot, 3)).rgb;
        vec3 probe = (vec3(cell) - cam_floor + 0.5 - (cam_cells - cam_floor)) * cs;
        uint base_word = at + 30u + uint(o) * 5u;
        print_words.data[base_word] = floatBitsToUint(tri.x * tri.y * tri.z);
        print_words.data[base_word + 1u] = floatBitsToUint(first.a == velk_gi_stamp(cell, ladder) ? 1.0 : 0.0);
        print_words.data[base_word + 2u] = floatBitsToUint(third.a);
        print_words.data[base_word + 3u] = floatBitsToUint(0.282095 * first.g + 0.325735 * (second.g * N.y + third.g * N.z + fourth.g * N.x));
        print_words.data[base_word + 4u] = floatBitsToUint(dot(probe - rel, N) / cs);
    }
}
)";

// Updates part of the probe field each frame, see VELK_GI_FIELD_GLSL. `env.z` is the
// finest level's ladder index, `env.w` the atlas, and `extras.x` the workgroups of this
// dispatch. Each level is an independent DDGI volume, and its rays go as far as they need to.
//
// A workgroup is 64 threads, VELK_PROBE_DIRS rays for each of its probes. probe_trace_main_src
// finds what each ray hits and probe_gi_main_src shades it, with the same thread handling the
// same ray in both. Each hit shades against one light chosen by contribution, so the cost does
// not depend on the light count.
//
// The words at `extras.z` hold, in order, 4 per ray for probe_maps_compute_src: radiance and
// distance, the distance negative for the back of a solid; 4 per probe from the trace, saying
// which probe it is; and 11 per ray from the trace: distance, shape, uv, normal, and medium
// depth and colour.
#define VELK_PROBE_DIR_GLSL R"(
// Fibonacci sphere, rotated per frame so successive frames fill the gaps
// between directions instead of resampling the same 64.
vec3 velk_probe_dir(uint i, uint n, float phase)
{
    float k = float(i) + 0.5;
    float phi = acos(1.0 - 2.0 * k / float(n));
    float theta = 3.14159265 * (1.0 + sqrt(5.0)) * k + phase;
    vec3 v = vec3(cos(theta) * sin(phi), sin(theta) * sin(phi), cos(phi));
    // Tipped as well as turned, since turning alone leaves the same bands of latitude
    // covered every frame.
    float tilt = fract(phase * 7.13) * 6.28318531;
    float c = cos(tilt);
    float s = sin(tilt);
    return vec3(v.x, v.y * c - v.z * s, v.y * s + v.z * c);
}
)"

// Which probe and ray a thread of the probe passes handles.
#define VELK_PROBE_RAY_GLSL VELK_GI_FIELD_GLSL VELK_PROBE_DIR_GLSL R"(
float velk_probe_hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return float(x) * (1.0 / 4294967296.0);
}

struct VelkProbeRay {
    uint lane;
    uint dir;
    uint probe_local;
    uint probe;
    int slot;
    int ladder;
    ivec3 texel;
    float stamp;
    vec3 origin;
    vec3 d;
    float phase;
    float spacing;
    bool live;
    bool sweeping;
    bool drop_history;
};

uint velk_probe_hits_word()
{
    return pc.extras.z + pc.extras.x * 256u + pc.extras.x * (64u / uint(VELK_PROBE_DIRS)) * 4u;
}

VelkProbeRay velk_probe_ray()
{
    const uint kDirs = VELK_PROBE_DIRS;
    const uint kProbesPerGroup = 64u / kDirs;
    const uint kLevelGroups = uint(kGiDim * kGiDim * kGiDim) / kProbesPerGroup;

    int finest = int(pc.env.z);
    uint atlas = pc.env.w;
    uint serial = VELK_GLOBALS.present_counter;

    // Every frame updates all the levels, finer ones with a larger share of the groups. Within
    // a share, neighbouring groups take a pair of levels and swap each frame, so that one frame
    // costs the same as the next.
    const uint kShares[4] = uint[4](6u, 2u, 1u, 1u);
    uint group_base = 0u;
    uint share = 0u;
    uint groups = 0u;
    for (; share < 4u; ++share) {
        groups = pc.extras.x * kShares[share] / 10u;
        if (gl_WorkGroupID.x < group_base + groups || share == 3u) break;
        group_base += groups;
    }
    uint in_share = gl_WorkGroupID.x - group_base;
    uint level = ((serial ^ uint(finest) ^ in_share) & 1u) + share * 2u;

    VelkProbeRay p;
    // When the field has to be right at once, each frame replaces one whole level.
    p.sweeping = pc.extras.w != 0xFFFFFFFFu;
    p.drop_history = p.sweeping && (pc.extras.w & 0x100u) != 0u;
    if (p.sweeping) {
        level = pc.extras.w & 0xFFu;
    }
    p.ladder = finest + int(level);
    p.slot = p.ladder & 7;
    float cell = velk_gi_cell(p.ladder);

    p.lane = gl_LocalInvocationIndex;
    p.probe_local = p.lane / kDirs;
    p.dir = p.lane - p.probe_local * kDirs;
    // Whole groups take turns, since a group's probes run concurrently across its lanes.
    uint group_in_level = p.sweeping ? gl_WorkGroupID.x % kLevelGroups
                                     : ((in_share >> 1u) + serial * (groups >> 1u)) % kLevelGroups;
    uint probe_in_level = group_in_level * kProbesPerGroup + p.probe_local;
    p.probe = probe_in_level + uint(p.slot) * uint(kGiDim * kGiDim * kGiDim);
    p.live = atlas != 0u && (!p.sweeping || gl_WorkGroupID.x < kLevelGroups);

    // The world cell this texel holds now: the one in the level's box that wraps to it.
    uint dim = uint(kGiDim);
    p.texel = ivec3(int(probe_in_level % dim), int((probe_in_level / dim) % dim), int(probe_in_level / (dim * dim)));
    vec3 cam = VELK_GLOBALS.cam_pos.xyz;
    vec3 cam_cells = cam / cell;
    vec3 cam_floor = floor(cam_cells);
    ivec3 corner = ivec3(cam_floor) - kGiDim / 2;
    ivec3 world_cell = corner + ((p.texel - corner) & ivec3(kGiDim - 1));
    p.stamp = velk_gi_stamp(world_cell, p.ladder);
    p.origin = (vec3(world_cell) - cam_floor + 0.5 - (cam_cells - cam_floor)) * cell;

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
    p.phase = velk_probe_hash(p.probe * 2654435761u
                              + VELK_GLOBALS.present_counter * 747796405u)
            * 6.28318531;
    p.d = velk_probe_dir(p.dir, kDirs, p.phase);
    p.spacing = cell;
    return p;
}
)"

#define VELK_PROBE_HELPERS_GLSL VELK_PROBE_RAY_GLSL R"(
layout(set = 1, binding = 16, std430) buffer ProbeRays { uint data[]; } probe_rays;
// The ray being shaded, for biasing the lookup at its hit towards where the ray came from.
vec3 velk_probe_ray_dir = vec3(0.0, 1.0, 0.0);
vec3 velk_hit_irradiance(vec3 pos, vec3 N)
{
    return velk_gi_irradiance(pc.env.w, pc.extras.y, int(pc.env.z), pos, N, velk_probe_ray_dir, VELK_GLOBALS.cam_pos.xyz);
}

// Open surfaces are seen from both sides, so only solid shapes say a probe is inside one.
bool velk_gi_inside_solid(RayHit hit, vec3 dir)
{
    if (dot(hit.normal, dir) <= 0.0) return false;
    RtShape s = velk_bvh_shapes.data[hit.shape_index];
    if (s.shape_kind == 1u || s.shape_kind == 2u) return true;
    if (s.shape_kind != 255u || s.mesh_static_base == VELK_INVALID_MESH_STATIC) return false;
    return (velk_mesh_static(s).flags & (VELK_MESH_SCATTERED | VELK_MESH_DOUBLE_SIDED)) == 0u;
}

)"

// Finds what the probe rays hit. Holds no materials, so that the walk through the scene has
// the GPU's registers to itself. Each level is a volume of its own, and its rays go as far
// as they need to.
[[maybe_unused]] constexpr string_view probe_trace_main_src = VELK_PROBE_RAY_GLSL R"(
layout(set = 1, binding = 16, std430) buffer ProbeHits { uint data[]; } probe_hits;

void main()
{
    VelkProbeRay p = velk_probe_ray();
    if (p.dir == 0u) {
        uint header = pc.extras.z + pc.extras.x * 256u + (gl_WorkGroupID.x * (64u / uint(VELK_PROBE_DIRS)) + p.probe_local) * 4u;
        probe_hits.data[header] = (p.live ? 1u : 0u) | (p.sweeping ? 2u : 0u) | (p.drop_history ? 4u : 0u)
            | (uint(p.texel.x) << 8u) | (uint(p.texel.y) << 12u) | (uint(p.texel.z) << 16u) | (uint(p.slot) << 20u);
        probe_hits.data[header + 1u] = floatBitsToUint(p.phase);
        probe_hits.data[header + 2u] = floatBitsToUint(length(vec3(p.spacing)) * 1.5);
        probe_hits.data[header + 3u] = floatBitsToUint(p.stamp);
    }
    uint at = velk_probe_hits_word() + (gl_WorkGroupID.x * 64u + p.lane) * 11u;
    if (!p.live) {
        probe_hits.data[at + 1u] = 0xFFFFFFFFu;
        return;
    }

    Ray ray;
    ray.origin = p.origin;
    ray.dir = p.d;
    RayHit hit;
    // A probe's rays stand for everything around it, out to the next probes, so
    // instances smaller than that are not hit or missed one by one.
    velk_ray_footprint = p.spacing * 0.25;
    // The probe's rays share the sphere around it, each a cone of its share.
    float cone_cos = 1.0 - 2.0 / float(VELK_PROBE_DIRS);
    velk_ray_cone = sqrt(1.0 - cone_cos * cone_cos) / cone_cos;
    bool found = trace_closest_hit(ray, 1e30, hit);

    probe_hits.data[at] = floatBitsToUint(hit.t);
    probe_hits.data[at + 1u] = found ? hit.shape_index : 0xFFFFFFFFu;
    probe_hits.data[at + 2u] = floatBitsToUint(hit.uv.x);
    probe_hits.data[at + 3u] = floatBitsToUint(hit.uv.y);
    probe_hits.data[at + 4u] = floatBitsToUint(hit.normal.x);
    probe_hits.data[at + 5u] = floatBitsToUint(hit.normal.y);
    probe_hits.data[at + 6u] = floatBitsToUint(hit.normal.z);
    probe_hits.data[at + 7u] = floatBitsToUint(velk_ray_medium_depth);
    probe_hits.data[at + 8u] = floatBitsToUint(velk_ray_medium_color.r);
    probe_hits.data[at + 9u] = floatBitsToUint(velk_ray_medium_color.g);
    probe_hits.data[at + 10u] = floatBitsToUint(velk_ray_medium_color.b);
}
)";

[[maybe_unused]] constexpr string_view probe_gi_main_src = VELK_PROBE_HELPERS_GLSL R"(
void main()
{
    VelkProbeRay p = velk_probe_ray();
    uint lane = p.lane;
    uint dir = p.dir;
    uint probe = p.probe;
    vec3 origin = p.origin;
    vec3 d = p.d;
    float spacing = p.spacing;
    // `probe_live` rather than `active`, which GLSL reserves.
    bool probe_live = p.live;

    const float kBrightest = 5.0;

    vec3 radiance = vec3(0.0);
    float near_t = 1e27;
    bool near_back = false;
    velk_probe_ray_dir = d;
    if (probe_live) {
        Ray ray;
        ray.origin = origin;
        ray.dir = d;
        uint hit_at = velk_probe_hits_word() + (gl_WorkGroupID.x * 64u + lane) * 11u;
        RayHit hit;
        hit.t = uintBitsToFloat(probe_rays.data[hit_at]);
        hit.shape_index = probe_rays.data[hit_at + 1u];
        hit.uv = vec2(uintBitsToFloat(probe_rays.data[hit_at + 2u]), uintBitsToFloat(probe_rays.data[hit_at + 3u]));
        hit.normal = vec3(uintBitsToFloat(probe_rays.data[hit_at + 4u]),
                          uintBitsToFloat(probe_rays.data[hit_at + 5u]),
                          uintBitsToFloat(probe_rays.data[hit_at + 6u]));
        velk_ray_medium_depth = uintBitsToFloat(probe_rays.data[hit_at + 7u]);
        velk_ray_medium_color = vec3(uintBitsToFloat(probe_rays.data[hit_at + 8u]),
                                     uintBitsToFloat(probe_rays.data[hit_at + 9u]),
                                     uintBitsToFloat(probe_rays.data[hit_at + 10u]));
        bool found = hit.shape_index != 0xFFFFFFFFu;
        // The shadow ray at the hit is as wide as the probe's own rays.
        velk_ray_footprint = spacing * 0.25;
        if (found) {
            near_t = hit.t;
            near_back = velk_gi_inside_solid(hit, d);
        }
        if (found && !near_back) {
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
            ectx.rel_pos = ray.origin + hit.t * ray.dir;
            ectx.hit_pos = ectx.rel_pos + VELK_GLOBALS.cam_pos.xyz;
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
                Light light = velk_light_at(pc.lights_base + li);
                vec3 L;
                float atten;
                velk_light_sample(light, ectx.rel_pos, L, atten);
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
        } else if (!found) {
            radiance = env_miss_color(d);
        }
        // A single ray on a tiny, very bright thing would otherwise light a whole probe.
        radiance = min(radiance, vec3(kBrightest));
        velk_ray_footprint = 0.0;
)"
                                                                      R"(
        // The ray lost light in what it passed through, and gained what that reflects,
        // lit as rounded objects seen along the ray. What reaches them from elsewhere
        // is the environment.
        if (velk_ray_medium_depth > 0.0) {
            vec3 direct = vec3(0.0);
            for (uint li = 0u; li < pc.light_count; ++li) {
                Light light = velk_light_at(pc.lights_base + li);
                vec3 L;
                float atten;
                velk_light_sample(light, origin, L, atten);
                float cos_g = clamp(dot(L, -d), -1.0, 1.0);
                float g = acos(cos_g);
                direct += light.color_intensity.rgb * light.color_intensity.a * atten * (2.0 / 3.0)
                          * (sin(g) + (3.14159265 - g) * cos_g) / 3.14159265;
            }
            vec3 elsewhere = env_miss_color(d);
            float through = exp(-velk_ray_medium_depth);
            radiance = radiance * through
                + (velk_ray_medium_color / velk_ray_medium_depth) * (direct + elsewhere) * (1.0 - through);
        }
    }

    if (probe_live) {
        uint ray_at = pc.extras.z + (gl_WorkGroupID.x * 64u + lane) * 4u;
        probe_rays.data[ray_at] = floatBitsToUint(radiance.r);
        probe_rays.data[ray_at + 1u] = floatBitsToUint(radiance.g);
        probe_rays.data[ray_at + 2u] = floatBitsToUint(radiance.b);
        probe_rays.data[ray_at + 3u] = floatBitsToUint(near_back ? -near_t : near_t);
    }
}
)";

/// Stores what a probe's rays found: its spherical-harmonic coefficients in the atlas, and its
/// irradiance and distance maps, as DDGI does. Runs after the probe rays are shaded, so that
/// nothing is written while the field is read. A workgroup is one probe and each thread one
/// map entry.
inline constexpr string_view probe_maps_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];
layout(set = 0, binding = 2, rgba32f) uniform writeonly image2D gStorageImagesF32[];
layout(set = 1, binding = 16, std430) readonly buffer ProbeRays { uint data[]; } probe_rays;
)" VELK_GI_FIELD_GLSL VELK_PROBE_DIR_GLSL R"(
layout(push_constant) uniform PC {
    uint rays_word;
    uint headers_word;
    uint maps;
    uint dirs;
    uint atlas;
} pc;

shared vec3 s_dir[64];
shared vec3 s_radiance[64];
shared float s_near[64];
shared uint s_back[64];

void main()
{
    uint lane = gl_LocalInvocationIndex;
    uint probe = gl_WorkGroupID.x;
    uint header = pc.headers_word + probe * 4u;
    uint flags = probe_rays.data[header];
    float phase = uintBitsToFloat(probe_rays.data[header + 1u]);
    float max_distance = uintBitsToFloat(probe_rays.data[header + 2u]);
    float stamp = uintBitsToFloat(probe_rays.data[header + 3u]);

    // Each thread brings in one ray, so every entry can read them all from shared memory.
    if ((flags & 1u) != 0u && lane < pc.dirs) {
        uint at = pc.rays_word + (probe * pc.dirs + lane) * 4u;
        s_radiance[lane] = vec3(uintBitsToFloat(probe_rays.data[at]),
                                uintBitsToFloat(probe_rays.data[at + 1u]),
                                uintBitsToFloat(probe_rays.data[at + 2u]));
        float found_at = uintBitsToFloat(probe_rays.data[at + 3u]);
        s_near[lane] = abs(found_at);
        s_back[lane] = found_at < 0.0 ? 1u : 0u;
        s_dir[lane] = velk_probe_dir(lane, pc.dirs, phase);
    }
    barrier();
    if ((flags & 1u) == 0u) return;

    bool sweeping = (flags & 2u) != 0u;
    bool drop_history = (flags & 4u) != 0u;
    ivec3 texel = ivec3(int((flags >> 8u) & 15u), int((flags >> 12u) & 15u), int((flags >> 16u) & 15u));
    int slot = int((flags >> 20u) & 7u);
    bool same_cell = velk_texture(pc.atlas, velk_gi_uv(texel, slot, 0)).a == stamp;
    bool keep_history = same_cell && !drop_history;

    // Inside something when enough rays reach the back of a solid.
    uint backs = 0u;
    for (uint r = 0u; r < pc.dirs; ++r) {
        if (s_back[r] != 0u) ++backs;
    }

    if (lane == 0u) {
        // Project into SH L1. 4*pi/N is the Monte Carlo weight for directions
        // distributed uniformly over the sphere.
        const float kY0 = 0.282095;
        const float kY1 = 0.488603;
        float w = 4.0 * 3.14159265 / float(pc.dirs);
        vec3 sh[4];
        sh[0] = vec3(0.0);
        sh[1] = vec3(0.0);
        sh[2] = vec3(0.0);
        sh[3] = vec3(0.0);
        for (uint r = 0u; r < pc.dirs; ++r) {
            sh[0] += s_radiance[r] * (kY0 * w);
            sh[1] += s_radiance[r] * (kY1 * s_dir[r].y * w);
            sh[2] += s_radiance[r] * (kY1 * s_dir[r].z * w);
            sh[3] += s_radiance[r] * (kY1 * s_dir[r].x * w);
        }

        // A block holding another cell starts over. Otherwise a running average settles into
        // a fixed rate, so moving lights do not smear indefinitely.
        float count = same_cell && !sweeping ? velk_texture(pc.atlas, velk_gi_uv(texel, slot, 1)).a : 0.0;
        float blend = max(1.0 / (count + 1.0), 0.05);
        float inside_now = float(backs) / float(pc.dirs);
        float inside = same_cell ? mix(velk_texture(pc.atlas, velk_gi_uv(texel, slot, 2)).a, inside_now, 0.25) : inside_now;
        for (int c = 0; c < 4; ++c) {
            ivec2 at = ivec2(texel.x + texel.z * kGiDim, texel.y + (c + slot * kGiRows) * kGiDim);
            vec3 prev = same_cell ? velk_texture(pc.atlas, velk_gi_uv(texel, slot, c)).rgb : vec3(0.0);
            float alpha = c == 0 ? stamp : (c == 1 ? min(count + 1.0, 100.0) : (c == 2 ? inside : 1.0));
            imageStore(gStorageImagesF32[nonuniformEXT(pc.atlas)], at, vec4(mix(prev, sh[c], blend), alpha));
        }
        imageStore(gStorageImagesF32[nonuniformEXT(pc.atlas)],
                   ivec2(texel.x + texel.z * kGiDim, texel.y + (4 + slot * kGiRows) * kGiDim), vec4(0.0, 0.0, 0.0, 1.0));
    }

    // Too many rays reaching the back of something: the probe is inside it, so its maps are
    // left alone.
    if (float(backs) >= float(pc.dirs) * 0.25) return;

    const float kHysteresis = 0.97;
    const float kChangeThreshold = 0.2;
    const float kBrightnessThreshold = 1.0;
    const float kDarkeningStep = 1.0 / 1024.0;

    int x = int(lane) % kGiMapSize;
    int y = int(lane) / kGiMapSize;
    vec2 p = (vec2(float(x), float(y)) + 0.5) / float(kGiMapSize) * 2.0 - 1.0;
    vec3 entry_dir = velk_gi_oct_dir(p);
    float irradiance_weight = 0.0;
    vec3 irradiance_sum = vec3(0.0);
    float distance_weight = 0.0;
    vec2 distance_sum = vec2(0.0);
    for (uint r = 0u; r < pc.dirs; ++r) {
        float facing = max(dot(entry_dir, s_dir[r]), 0.0);
        if (s_back[r] == 0u) {
            irradiance_weight += facing;
            irradiance_sum += facing * s_radiance[r];
        }
        float sharp = pow(facing, 50.0);
        float away = min(s_near[r], max_distance);
        distance_weight += sharp;
        distance_sum += sharp * vec2(away, away * away);
    }

    ivec2 at = velk_gi_map_texel(texel, slot, ivec2(x, y), 0);
    vec3 held = keep_history ? velk_texture(pc.maps, velk_gi_map_uv(at)).rgb : vec3(0.0);
    float hysteresis = dot(held, held) == 0.0 ? 0.0 : kHysteresis;
    vec3 now = pow(irradiance_sum / max(irradiance_weight, 1e-9), vec3(1.0 / kGiIrradianceGamma));
    vec3 delta = now - held;
    if (max(held.r - now.r, max(held.g - now.g, held.b - now.b)) > kChangeThreshold) {
        hysteresis = max(0.0, hysteresis - 0.75);
    }
    if (dot(delta, vec3(0.2126, 0.7152, 0.0722)) > kBrightnessThreshold) {
        delta *= 0.25;
    }
    vec3 step_by = (1.0 - hysteresis) * delta;
    if (max(now.r, max(now.g, now.b)) < max(held.r, max(held.g, held.b))) {
        step_by = min(max(vec3(kDarkeningStep), abs(step_by)), abs(delta)) * sign(step_by);
    }
    imageStore(gStorageImagesF16[nonuniformEXT(pc.maps)], at, vec4(held + step_by, 1.0));

    ivec2 at_distance = velk_gi_map_texel(texel, slot, ivec2(x, y), 1);
    vec2 held_distance = keep_history ? velk_texture(pc.maps, velk_gi_map_uv(at_distance)).rg : vec2(0.0);
    float distance_hysteresis = dot(held_distance, held_distance) == 0.0 ? 0.0 : kHysteresis;
    vec2 now_distance = distance_sum / max(distance_weight, 1e-9);
    imageStore(gStorageImagesF16[nonuniformEXT(pc.maps)], at_distance,
               vec4(mix(now_distance, held_distance, distance_hysteresis), 0.0, 1.0));
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

/// Composes a compute shader that traces rays and shades nothing: the RT prelude and the
/// frame's intersect snippets under @p main_src, without any material.
inline string compose_rt_trace_compute(const IFrameSnippetRegistry& snippets, string_view main_src)
{
    string src;
    src += rt_compute_prelude_src;
    append_rt_intersect_includes(src, snippets);
    append_rt_intersect_shape(src, snippets);
    src += main_src;
    return src;
}

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
    // under a different entry point (the GI probe pass) supply their own main.
    src += main_src.empty() ? rt_compute_main_src : main_src;
    return src;
}

} // namespace velk

#endif // VELK_RENDER_DEFAULT_SHADERS_H
