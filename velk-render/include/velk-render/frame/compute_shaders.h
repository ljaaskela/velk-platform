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
    uint object;      // the object id of what was hit, only set by trace_closest_hit
    // Where on its object the hit is, and which of its axes the surface faces, as the
    // G-buffer's surface attachment has them. Only set by trace_closest_hit.
    vec3 local;
    uint side;
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
// Set when a ray is tested against something that a motion function moves, which could
// stand in its way at another time. For the shader to clear before tracing and read after.
bool velk_ray_met_motion = false;
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

// Which of its object's six axis directions a surface facing `n`, in the object's own space,
// faces: 0 and 1 for +x and -x, then y, then z, as the G-buffer's surface attachment has it.
uint velk_facing_side(vec3 n)
{
    vec3 across = abs(n);
    return across.x >= across.y && across.x >= across.z ? (n.x < 0.0 ? 1u : 0u)
         : across.y >= across.z ? (n.y < 0.0 ? 3u : 2u) : (n.z < 0.0 ? 5u : 4u);
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
    if ((rec.shape_count & VELK_RECORD_RAY_MOTION) == 0u || velk_ray_footprint >= VELK_RAY_MOTION_REACH) return local;
    velk_ray_met_motion = true;
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

    // The three words past kStack hold the frame of the nearest hit so far, which only the
    // hit's own record needs at the end, and registers cannot spare.
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
                stack[kStack] = floatBitsToUint(velk_ray_frame.x);
                stack[kStack + 1] = floatBitsToUint(velk_ray_frame.y);
                stack[kStack + 2] = floatBitsToUint(velk_ray_frame.z);
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
                        stack[kStack] = floatBitsToUint(velk_ray_frame.x);
                        stack[kStack + 1] = floatBitsToUint(velk_ray_frame.y);
                        stack[kStack + 2] = floatBitsToUint(velk_ray_frame.z);
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
            record_medium = footprint > 0.0 && (rec.shape_count & VELK_RECORD_MEDIUM) != 0u && (closest || rec.tree_root == VELK_NO_TREE);
            medium_far = best_t;
#endif
            if (rec.tree_root == VELK_NO_TREE) {
                shape_at = rec.first_shape;
                shapes_left = rec.shape_count & VELK_RECORD_SHAPE_COUNT;
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
    hit.object = rec.object;
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
        hit.side = velk_facing_side(n);
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
        hit.side = velk_facing_side(h.normal);
    }
    // Where on its object, from the ray in the object's own space. Its axes in world units, as
    // the G-buffer has it: the inverse's rows are as long as one over each axis's scale.
    velk_ray_frame = vec3(uintBitsToFloat(stack[kStack]), uintBitsToFloat(stack[kStack + 1]), uintBitsToFloat(stack[kStack + 2]));
    Ray local_ray = velk_local_ray(ray, rec);
#ifdef VELK_RAY_MOTION
    if (!seeded) local_ray = velk_moved_ray(local_ray, rec, best_shape);
#endif
    vec3 axis_scale = 1.0 / max(vec3(length(rec.inv_row0.xyz), length(rec.inv_row1.xyz), length(rec.inv_row2.xyz)), vec3(1e-20));
    hit.local = (local_ray.origin + local_ray.dir * best_t) * axis_scale;
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
// The resolve has no lights, so their field carries one more attachment.
#define RESOLVE_SURFACE pc.lights_base

struct ResolveVertex {
    vec3 local;
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
    r.local = inst.offset.xyz + v.position * inst.size.xyz;
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
        imageStore(gStorageImagesF32[nonuniformEXT(RESOLVE_SURFACE)], coord, vec4(0.0));
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

    // Where on the object this is, in the object's own axes but world units, and which of
    // those axes the surface faces. From the vertices as stored and the unmapped normal, so
    // neither a swaying material nor a normal map moves it.
    mat3 to_world = mat3(inst.world_matrix);
    vec3 axis_scale = vec3(length(to_world[0]), length(to_world[1]), length(to_world[2]));
    vec3 local = v0.local * weights.x + v1.local * weights.y + v2.local * weights.z;
    vec3 facing = transpose(to_world) * ctx.normal;
    vec3 across = abs(facing);
    float side = across.x >= across.y && across.x >= across.z ? (facing.x < 0.0 ? 1.0 : 0.0)
               : across.y >= across.z ? (facing.y < 0.0 ? 3.0 : 2.0)
               : (facing.z < 0.0 ? 5.0 : 4.0);
    imageStore(gStorageImagesF32[nonuniformEXT(RESOLVE_SURFACE)], coord, vec4(local * axis_scale, side));
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

// Light cells: where a surface's light is kept. A cell is a cube in one object's own space,
// so it moves with the object, and only the part of the surface that faces one of the
// object's six axis directions, so the two sides of a thin wall are apart. Cubes double in
// size from one level to the next. A reader picks the level by how wide it is there.
#define VELK_LIGHT_CELL_GLSL R"(
// The instances drawn, as words: an ElementInstance is 32 of them, and its params[3], the
// last, is the object id, the same one rays find in RtRecord.flags. See
// IScene::get_object_ids.
layout(set = 1, binding = 3, std430) readonly buffer VelkInstanceWords { uint data[]; } velk_instance_words;

uint velk_object_id(uint instance)
{
    return velk_instance_words.data[instance * 32u + 31u];
}

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

struct VelkLightCell {
    uint object;  // which placed object: its object id
    int level;    // cubes are 2^level world units across
    ivec3 at;     // which cube, in the object's own axes
    uint side;    // which of the six axis directions the surface faces
};

// The level whose cubes are about `pixels` across on screen at `distance` from the camera.
// `pixel_width` is a pixel's width in world units at distance 1.
int velk_light_cell_level(float distance, float pixel_width, float pixels)
{
    return int(ceil(log2(max(distance * pixel_width * pixels, 1e-6))));
}

float velk_light_cell_size(int level)
{
    return exp2(float(level));
}

// The cell holding `surface`, a position as the G-buffer's surface attachment stores it.
VelkLightCell velk_light_cell(uint object, vec3 surface, uint side, int level)
{
    VelkLightCell c;
    c.object = object;
    c.level = level;
    c.at = ivec3(floor(surface / velk_light_cell_size(level)));
    c.side = side;
    return c;
}

uint velk_light_cell_mix(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint velk_light_cell_hash(VelkLightCell c)
{
    uint h = velk_light_cell_mix(c.object + 0x9e3779b9u);
    h = velk_light_cell_mix(h ^ uint(c.at.x));
    h = velk_light_cell_mix(h ^ uint(c.at.y));
    h = velk_light_cell_mix(h ^ uint(c.at.z));
    return velk_light_cell_mix(h ^ (uint(c.level + 64) * 8u + c.side));
}
)"

// Bounce cells: light cells kBounceUp levels larger than a pixel's, which also gather the
// light arriving from other surfaces with kBounceRays fixed rays. Each has an entry in a
// table of its own in the culling arena, found by hashing the cell's word, of kBounceWords:
//   0      the cell's word, 0 for an empty entry
//   1      the frame it was last used in
//   2      kBounceUntraced, kBounceListed or kBounceTraced
//   3      spare
//   4..9   the light arriving from other surfaces as SH L1 of radiance: red, green and
//          blue halves of each coefficient (velk_bounce_sh_irradiance)
//   10..12 where its rays start, the pixel that listed it, in the world: not from the camera,
//          which moves; only light
//          falloff reads it, which far from the origin does not need every digit
//   13     its normal then (velk_oct)
//   14     kBounceStale when something changed on the way of its rays, which are then traced
//          again; kBounceFromRay when a bounce ray made it rather than a pixel
//   15     the cell's tag when the entry was made, so a slot taken by another cell is seen
//   16..   its rays, kBounceRayWords each: the cell hit, or kNoCell for a miss; the distance
//          as a half in the low 16 bits and the normal hit above (velk_oct8); the albedo
//          there, 8 bits a channel; the emission there (velk_pack_rgbe); the bounce cell
//          over the hit (its word in the cell table), whose light arriving there is passed
//          on, or kNoCell.
#define VELK_BOUNCE_GLSL R"(
const int kBounceUp = 2;
const uint kBounceRays = 64u;
const uint kBounceRayWords = 5u;
const uint kBounceHeader = 16u;
const uint kBounceWords = kBounceHeader + kBounceRays * kBounceRayWords;
const uint kBounceProbes = 4u;
const uint kBounceUntraced = 0u;
const uint kBounceListed = 1u;
const uint kBounceTraced = 2u;
// Word 14's flags. An entry made by a ray (hop 1) has no pixel: its rays start where that ray
// hit, and the cells its own rays hit get no entries made for them, which is where bounce
// ends. A pixel using it clears the flag.
const uint kBounceStale = 1u;
const uint kBounceFromRay = 2u;
// In place of the cell hit: something was hit, but its cell had no room in the table.
const uint kBounceHitNoCell = 0xFFFFFFFEu;
// In place of the cell hit: something was hit, and the cell is not found yet.
const uint kBounceHitPending = 0xFFFFFFFDu;
// An entry unused this many frames gives way to a new one.
const uint kBounceUnusedFrames = 16u;
// The tangent of the half angle of each ray's cone, kBounceRays of them sharing the
// hemisphere: cos = 1 - 1 / 64.
const float kBounceConeTan = 0.1784;
// A single ray on a tiny, very bright thing would otherwise light a whole cell.
const float kBounceBrightest = 5.0;
// How far a ray that hit nothing counts as reaching, for what may move into its way.
const float kBounceMissReach = 1e5;

// How far a cell's rays are turned about its normal, from its word, so neighbours do not
// line up.
float velk_bounce_turn(uint cell)
{
    uint x = cell * 2654435761u;
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15;
    return float(x & 0xFFFFu) / 65536.0 * 6.28318531;
}

// Ray `k` of a cell facing `N`: spread evenly over the hemisphere, each standing for the
// same solid angle.
vec3 velk_bounce_dir(uint k, vec3 N, float turn)
{
    float z = 1.0 - (float(k) + 0.5) / float(kBounceRays);
    float r = sqrt(max(1.0 - z * z, 0.0));
    float phi = float(k) * 2.39996323 + turn;
    vec3 up = abs(N.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);
    return normalize(T * (r * cos(phi)) + B * (r * sin(phi)) + N * z);
}

// A unit normal on the octahedron, and back.
vec2 velk_oct(vec3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-20);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
}

vec3 velk_unoct(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

// The same in 16 bits, 8 a coordinate.
uint velk_oct8(vec3 n)
{
    return packSnorm4x8(vec4(velk_oct(n), 0.0, 0.0)) & 0xFFFFu;
}

vec3 velk_unoct8(uint packed)
{
    return velk_unoct(unpackSnorm4x8(packed & 0xFFFFu).xy);
}

// A colour in one word: 8 bits a channel under a shared power of two.
uint velk_pack_rgbe(vec3 c)
{
    float m = max(max(c.r, c.g), c.b);
    if (m < 1e-6) return 0u;
    int e = int(ceil(log2(m)));
    vec3 v = c / exp2(float(e)) * 255.0;
    return uint(v.r + 0.5) | (uint(v.g + 0.5) << 8u) | (uint(v.b + 0.5) << 16u) | (uint(clamp(e + 128, 0, 255)) << 24u);
}

vec3 velk_unpack_rgbe(uint w)
{
    if (w == 0u) return vec3(0.0);
    float scale = exp2(float(int(w >> 24u) - 128)) / 255.0;
    return vec3(float(w & 255u), float((w >> 8u) & 255u), float((w >> 16u) & 255u)) * scale;
}

// The light from other surfaces arriving at a surface facing `N`, from the SH words 4 to 9 of
// a bounce entry, as irradiance over pi.
vec3 velk_bounce_sh_irradiance(uvec3 a, uvec3 b, vec3 N)
{
    vec2 h0 = unpackHalf2x16(a.x);
    vec2 h1 = unpackHalf2x16(a.y);
    vec2 h2 = unpackHalf2x16(a.z);
    vec2 h3 = unpackHalf2x16(b.x);
    vec2 h4 = unpackHalf2x16(b.y);
    vec2 h5 = unpackHalf2x16(b.z);
    vec3 sh0 = vec3(h0, h1.x);
    vec3 sh1 = vec3(h1.y, h2);
    vec3 sh2 = vec3(h3, h4.x);
    vec3 sh3 = vec3(h4.y, h5);
    return max(0.282095 * sh0 + 0.325735 * (sh1 * N.y + sh2 * N.z + sh3 * N.x), vec3(0.0));
}

// The same entry's light arriving along direction `d`, broad, since SH L1 holds only a rough
// picture of where light comes from.
vec3 velk_bounce_sh_radiance(uvec3 a, uvec3 b, vec3 d)
{
    vec2 h0 = unpackHalf2x16(a.x);
    vec2 h1 = unpackHalf2x16(a.y);
    vec2 h2 = unpackHalf2x16(a.z);
    vec2 h3 = unpackHalf2x16(b.x);
    vec2 h4 = unpackHalf2x16(b.y);
    vec2 h5 = unpackHalf2x16(b.z);
    vec3 sh0 = vec3(h0, h1.x);
    vec3 sh1 = vec3(h1.y, h2);
    vec3 sh2 = vec3(h3, h4.x);
    vec3 sh3 = vec3(h4.y, h5);
    return max(0.282095 * sh0 + 0.488603 * (sh1 * d.y + sh2 * d.z + sh3 * d.x), vec3(0.0));
}
)"

/// Shows the light cells under each pixel in place of the image. Mode 1: each cell a flat
/// colour of its own, darker where the surface turns from the camera. Mode 2: the level as a
/// colour, with its cells as a checker. Mode 3: which way the surface faces on its object.
inline constexpr string_view light_cells_debug_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform usampler2D velk_textures_uint[];
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];
)" VELK_LIGHT_CELL_GLSL R"(
layout(push_constant, scalar) uniform PC {
    uint ids_id;
    uint surface_id;
    uint pos_id;
    uint normal_id;
    uint output_id;
    uint width;
    uint height;
    uint mode;
    uint globals_base;
    float pixels;
} pc;

const vec3 kLevelColors[8] = vec3[8](
    vec3(1.0, 0.0, 0.0), vec3(1.0, 0.5, 0.0), vec3(1.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0),
    vec3(0.0, 1.0, 1.0), vec3(0.0, 0.3, 1.0), vec3(0.6, 0.0, 1.0), vec3(1.0, 0.0, 0.7));

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;

    uvec2 ids = texelFetch(velk_textures_uint[nonuniformEXT(pc.ids_id)], coord, 0).xy;
    vec3 color = vec3(0.0);
    if (ids.x != 0u) {
        VelkDrawData d = velk_draw_data.data[ids.x - 1u];
        uint position = ids.y >> d.triangle_bits;
        uint slot = d.visible_base == VELK_NO_VISIBLE_LIST ? position : velk_culling.data[d.visible_base + position];
        uint object = velk_object_id(d.instances_base + slot);

        vec4 surface = texelFetch(velk_textures[nonuniformEXT(pc.surface_id)], coord, 0);
        vec3 rel = texelFetch(velk_textures[nonuniformEXT(pc.pos_id)], coord, 0).xyz;
        vec3 N = normalize(texelFetch(velk_textures[nonuniformEXT(pc.normal_id)], coord, 0).xyz);

        mat4 vp = velk_globals.data[pc.globals_base].view_projection;
        float pixel_width = 2.0 / (length(vec3(vp[0][1], vp[1][1], vp[2][1])) * float(pc.height));
        int level = velk_light_cell_level(length(rel), pixel_width, pc.pixels);
        VelkLightCell cell = velk_light_cell(object, surface.xyz, uint(surface.w + 0.5), level);

        if (pc.mode == 2u) {
            float checker = ((cell.at.x + cell.at.y + cell.at.z) & 1) == 0 ? 1.0 : 0.6;
            color = kLevelColors[uint(level + 64) & 7u] * checker;
        } else if (pc.mode == 3u) {
            color = kLevelColors[cell.side];
        } else {
            uint h = velk_light_cell_hash(cell);
            vec3 flat_color = vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0;
            color = flat_color * (0.35 + 0.65 * abs(dot(N, normalize(-rel))));
        }
    }
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], coord, vec4(color, 1.0));
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
    uint surface_light_id;     // 4         lighting's light on the surface, for far instances
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
    uint cells_header;         // 64  word of the light cells' header. See CELLS_HEADER
    uint ltc_matrix_id;        // 68  LTC transform table; 0 = area specular off
    uint lights_base;          // 72  index into velk_lights (set = 1 slot 5)
    uint ltc_magnitude_id;     // 76  LTC energy / Fresnel table
    vec2 env_params;           // 80  x = intensity, y = rotation_rad (inline)
    uint bounce_hops;          // 88  how many surfaces bounce light passes over: 1 or 2
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
// `world_pos`, with nothing in the way. What blocks it comes from the light cells, as for any
// light.
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
// Lights each pixel: each light's diffuse light and highlight, shadowed by what the light
// cells hold; indirect light from the bounce cells, or the environment where they have
// nothing; reflections; area lights in closed form; emission. Writes the finished image,
// and for instance_splat_composite_src the light on the surface: indirect light in rgb, and
// in alpha the share of direct light that gets past what is in the way.
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
        imageStore(gStorageImagesF16[nonuniformEXT(pc.surface_light_id)], coord, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }

    // Added to the lit colour, before tonemapping, so that bloom can pick up bright emitters.
    vec3 emissive = velk_texture(pc.emissive_tex_id, uv).rgb;

    // LightingMode encoded in mat.b (0 = Unlit, 1 = Standard, ...).
    uint lighting_mode = uint(mat.b * 255.0 + 0.5);
    float metallic = clamp(mat.r, 0.0, 1.0);
    float roughness = clamp(mat.g, 0.04, 1.0);

    vec3 color = albedo.rgb;
    vec4 surface_light = vec4(0.0, 0.0, 0.0, 1.0);
    if (lighting_mode != 0u) {
        vec3 N = normalize(world_n);
        vec3 V = normalize(-world_pos);
        float NdotV = max(dot(N, V), 0.0);
        vec3 F0 = mix(vec3(0.04), albedo.rgb, metallic);
        float a = roughness * roughness;

        bool cells_here = velk_cells_begin(coord, world_pos, normal_and_error);
        vec3 direct = vec3(0.0);
        vec3 unshadowed = vec3(0.0);
        vec3 highlights = vec3(0.0);
        vec3 area_irr = vec3(0.0);
        vec3 area_spec = vec3(0.0);
        for (uint i = 0u; i < pc.light_count; ++i) {
            Light light = velk_light_at(pc.lights_base + i);
            // Area lights are integrated in closed form, and blocked by what the cells say.
            if (light.flags.x == 3u) {
                vec2 es = velk_area_irradiance(light, world_pos, N, V, roughness);
                vec3 radiance = light.color_intensity.rgb * light.color_intensity.a
                              * (cells_here ? velk_cells_light_through(i) : 1.0);
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
            float through = cells_here ? velk_cells_light_through(i) : 1.0;
            direct += NdotL * radiance * through;
            unshadowed += NdotL * radiance;
            highlights += (D * G) * F / max(4.0 * NdotV * NdotL, 1e-6) * NdotL * radiance * through;
        }

        // Env lighting: single-sample approximation. Diffuse reads along N with a deep LOD
        // so it reads roughly the irradiance average; specular reads the mirror reflection
        // with LOD scaled by roughness as a cheap GGX-prefilter stand-in.
        float env_max_lod = float(textureQueryLevels(velk_textures[nonuniformEXT(pc.env_texture_id)])) - 1.0;
        vec3 env_diffuse = env_miss_color_lod(N, env_max_lod);
        vec3 env_specular = env_miss_color_lod(reflect(-V, N), roughness * env_max_lod);
        vec3 F_env = fresnel_schlick_roughness(NdotV, F0, roughness);

        // Light from other surfaces and the sky, from the bounce cells around the pixel, which
        // see the sky only where it is open. Where none has any yet, the environment stands in.
        vec3 gi = cells_here ? velk_cells_bounce(N) : vec3(-1.0);
        vec3 indirect = gi.r >= 0.0 ? gi : env_diffuse;

        // The environment map cannot tell whether the sky is visible along the reflection.
        // The bounce cells can: how much of the environment's light they find that way is how
        // much of the sharp reflection stays, and what they find in place of the sky fills the
        // rest.
        const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
        vec3 reflected = reflect(-V, N);
        vec3 cells_reflection = cells_here ? velk_cells_bounce_toward(reflected, true) : vec3(-1.0);
        if (cells_reflection.r >= 0.0) {
            float env_broad = dot(env_miss_color_lod(reflected, env_max_lod), kLuma);
            float sky_seen = env_broad > 1e-4 ? clamp(dot(cells_reflection, kLuma) / env_broad, 0.0, 1.0) : 0.0;
            env_specular = env_specular * sky_seen + cells_reflection * (1.0 - sky_seen);
        }

        color = albedo.rgb * (1.0 - metallic) * (direct + indirect + area_irr) + highlights + F_env * env_specular
              + area_spec;
        float lit = dot(unshadowed, kLuma);
        surface_light = vec4(indirect, lit > 1e-4 ? clamp(dot(direct, kLuma) / lit, 0.0, 1.0) : 1.0);
    }

    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(color + emissive, albedo.a));
    imageStore(gStorageImagesF16[nonuniformEXT(pc.surface_light_id)], coord, surface_light);
}
)";

// Light cells, kept and read. Follows deferred_compute_prelude_src, whose push constants it
// uses under the names below. Each pixel finds its cell in a table in the culling arena,
// making it if it is new. A cell's light is traced from the pixel that gets to it first: per
// light, how much of that light the surface sees, and how wide the soft edge of a shadow is
// there, traced once and kept until something changes on the way to a light. The pixel then
// shows direct diffuse light: each light's own falloff and angle at the pixel, times what
// the cells around it say gets through.
//
// A soft edge is as wide as the light is large and the thing casting the shadow is far. A
// wide one is read from larger cells, a few to its width, so that it comes out smooth
// whatever its width, from the same sixteen rays a cell.
//
// A cell is 30 words: a state, the key (object, cube x y z, level and side), the frame its
// light was last known good, the frame a pixel last read it, 16 words of 8 bits per light
// for the first 64 lights, the direct light arriving at it (kCellArrive), and where that is
// gathered (kCellArriveSum). The 8 bits per light hold, in the low 4, how much gets through, in fifteenths; in
// the next 3, how many half levels above its own a cell should be to suit the soft edge
// there; in the top bit, whether something that moves could be in the way, so that the
// light is traced again now and then (kCellLive). A cell unread for a while gives up its
// slot to a new one. The
// state is 0 for an empty slot, otherwise part of the key's hash with bit 0 set once the
// light is there and bit 1 while a thread is tracing it.
//
// This is the part that keeps cells up, for light_cells_update_main_src: its
// velk_cells_begin makes, checks and traces the cells of a pixel. Passes that only read use
// light_cells_read_src, which is far smaller. Where the cells are comes as one word of the
// push constants, CELLS_HEADER: the word in the culling arena of eight words, which are the table's
// first word, its slots, how many pixels across a cell is, the first word of the list of
// changes, the G-buffer's id and surface attachments, and two spare.
[[maybe_unused]] constexpr string_view light_cells_src = R"(
layout(set = 0, binding = 0) uniform usampler2D velk_textures_uint[];
layout(set = 1, binding = 16, std430) buffer VelkLightCellWords { uint data[]; } velk_light_cell_words;
)" VELK_LIGHT_CELL_GLSL VELK_BOUNCE_GLSL R"(
#define CELLS_HEADER pc.cells_header
// The header's words, read once a thread by velk_cells_load, which a main calls first.
uint cells_header[14];
void velk_cells_load()
{
    for (uint w = 0u; w < 14u; ++w) cells_header[w] = velk_light_cell_words.data[CELLS_HEADER + w];
}
// Where each object is, by the slot of its id, and how many slots: see
// IBvh::get_object_transforms. 0xFFFFFFFF if there is no table.
#define CELLS_OBJECTS cells_header[12]
#define CELLS_OBJECT_SLOTS cells_header[13]
#define CELLS_WORD cells_header[0]
#define CELLS_SLOTS cells_header[1]
#define CELLS_PIXELS cells_header[2]
// The bounce cells' table, and how many entries it has, a power of two. See VELK_BOUNCE_GLSL.
#define CELLS_BOUNCE cells_header[8]
#define CELLS_BOUNCE_ENTRIES cells_header[9]
// The bounce cells to trace this frame: a count and three spare words, then 2 words an entry:
// its word in the table, and the pixel that asked, x in the low half and y in the high. Then,
// for each entry, kBounceScratchWords a ray, what the trace found for the passes after it:
// the shape hit, or kNoCell; the hit's uv as halves; the object id; where on the object, 3
// words; which side of it.
#define CELLS_BOUNCE_LIST cells_header[10]
// How many bounce cells a frame may trace.
#define CELLS_BOUNCE_LIST_MAX cells_header[11]
const uint kBounceScratchWords = 8u;
uint velk_bounce_scratch(uint listed, uint ray)
{
    return CELLS_BOUNCE_LIST + 4u + CELLS_BOUNCE_LIST_MAX * 2u + (listed * kBounceRays + ray) * kBounceScratchWords;
}
// What changed this frame, in the same arena: a count, 1 if everything changed, 1 if there is
// anything for the mark passes, a spare word, then 8 words a change: a world box's least
// corner, the frame, its greatest corner, and which light moved or all ones.
#define CELLS_CHANGES cells_header[3]
#define CELLS_IDS cells_header[4]
#define CELLS_SURFACE cells_header[5]
// What to trace this frame, in the same arena: a count, three words on lights that moving
// things could be in the way of (kCellWorkLive*), the counters below, then 8 words an
// entry, which is one light for one cell: the word the cell starts at, or kNoCell for an
// entry not in use; the pixel that asked, x in the low half and y in the high, or
// kCellNoPixel for a cell a ray found; how many levels above that pixel's own the cell is,
// in the low 8 bits, and the light above them; and for a cell a ray found, the normal
// (velk_cell_pack_normal) and the place on the cell from the camera; and a spare.
#define CELLS_WORK cells_header[6]
// How many entries a frame may trace. Cells past that wait for a later frame.
#define CELLS_ENTRIES cells_header[7]
const uint kCellWorkMax = 32768u;
const uint kCellWorkWords = 8u;
const uint kCellNoPixel = 0xFFFFFFFFu;

// A unit normal in one word, octahedral, 16 bits a coordinate.
uint velk_cell_pack_normal(vec3 n)
{
    return packSnorm2x16(velk_oct(n));
}

vec3 velk_cell_unpack_normal(uint packed)
{
    return velk_unoct(unpackSnorm2x16(packed));
}
const uint kCellWorkHeader = 40u;
// Of the lights that moving things could be in the way of: how many the cells read this
// frame hold, every how many frames each is due, and how many were due this frame.
const uint kCellWorkLiveLoad = 1u;
const uint kCellWorkLivePeriod = 2u;
const uint kCellWorkLiveAsked = 3u;
// Counters that only grow, for the log: what was asked of the list, what it got, and why
// cells wait. The CPU reads them now and then and prints the differences.
const uint kStatAsked = 0u;      // entries asked for
const uint kStatListed = 1u;     // entries listed
const uint kStatCells = 2u;      // cells listed
const uint kStatRefused = 3u;    // cells refused for want of room in the list
const uint kStatBusy = 4u;       // cells not traced yet that another thread held
const uint kStatNoSlot = 5u;     // pixels that found no slot for their own cell
const uint kStatWaiting = 6u;    // pixels whose own cell was not traced after all
const uint kStatFrames = 7u;     // frames
const uint kStatTraced = 8u;     // entries the trace pass traced
const uint kStatChanged = 9u;    // cells due because something changed
const uint kStatMade = 10u;      // cells made
const uint kStatStale = 11u;     // cells due for every light, last checked before the oldest change kept
const uint kStatCoarseAsked = 12u;    // entries asked for in the first round, for cells kCellUp levels up
const uint kStatCoarseRefused = 13u;  // cells refused in the first round
const uint kStatCoarseWaiting = 14u;  // pixels waiting whose cell kCellUp levels up was not traced either
const uint kStatLive = 15u;           // cells listed only because moving things could be in the way
const uint kStatLiveLoad = 16u;       // lights in cells read that moving things could be in the way of
const uint kStatBounceAskNew = 17u;   // bounce cells with no rays traced asking to be listed
const uint kStatBounceAskStale = 18u; // bounce cells with rays crossed by a change asking to be listed
const uint kStatBounceListed = 19u;   // bounce cells listed for tracing
const uint kStatBounceGathered = 20u; // bounce cells gathered
const uint kStatBounceAllUnknown = 21u; // of those, cells none of whose rays had anything to read
const uint kStatBounceRaysUnknown = 22u; // rays gathered with nothing to read: hit cell with no light yet, or none
const uint kStatBounceSelfHits = 23u; // bounce rays that first hit the back of their own object close by
const uint kStatHopMade = 24u;        // bounce cells made by bounce rays
const uint kStatHopRefused = 25u;     // bounce cells bounce rays wanted made, with no room in the table
const uint kStatHopListed = 26u;      // bounce cells made by bounce rays listed for tracing
const uint kStatHopGathered = 27u;    // bounce cells made by bounce rays gathered
const uint kStatChangeBoxSun = 28u;   // lights on cells found changed: a moved box on the way to a sun
const uint kStatChangeBoxArea = 29u;  // the same, to an area light
const uint kStatChangeBoxOther = 30u; // the same, to a point or spot light
const uint kStatChangeLight = 31u;    // lights on cells found changed: the light itself moved
const uint kStatChangeInside = 32u;   // of those found changed by a box, the cell is inside the box
const uint kStatChangeCoarse = 33u;   // lights on cells found changed, on cells above a pixel's own level
const uint kStatMarked = 34u;         // cells the mark pass marked a light on
const uint kStatMarkUnplaced = 35u;   // cells the mark pass could not place, so took every light
void velk_cell_stat(uint which, uint by)
{
    atomicAdd(velk_light_cell_words.data[CELLS_WORK + 4u + which], by);
}

const uint kCellWords = 32u;
// The lights something changed on the way to since the cell was traced for them, 64 bits
// in 2 words, set by light_cells_mark_main_src and cleared by the trace.
const uint kCellChanged = 30u;
// The direct light arriving at a cell, summed over its lights, from the surface's facing
// and each light's falloff and how much of it gets through, as the mean over the places on
// the cell that were read last time: red and green as halves, then blue as a half and in
// the high 16 bits the frame it was worked out in.
const uint kCellArrive = 24u;
// Where that mean is gathered this frame: red, green and blue in fixed point, kArriveScale
// a unit, then how many places were added. velk_cell_arrive adds; the fold pass at the start
// of the next frame's update turns it into kCellArrive and empties it.
const uint kCellArriveSum = 26u;
const float kArriveScale = 256.0;
// In the low half of kCellArrive's second word in place of blue: made in the frame in the
// high half, so it has no light of its own to read yet.
const uint kArriveMade = 0xFFFFu;
// How many levels above pixel-sized cells a wide soft edge may be read from.
const int kCellUp = 3;
// The cells a reader blends at one level. See velk_cells_around.
const int kAround = 18;
// How many cells wide a soft edge should be at the level it is read from.
const float kCellsPerEdge = 4.0;
const uint kCellReady = 1u;
const uint kCellBusy = 2u;
// In what a cell holds for a light: something that moves could be in the way.
const uint kCellLive = 128u;
// The longest a light that moving things could be in the way of goes untraced, in frames.
const uint kCellLivePeriodMax = 256u;
const uint kCellProbes = 8u;
const uint kCellLights = 64u;
const uint kNoCell = 0xFFFFFFFFu;
// A cell no pixel has read for this many frames may give up its slot to a new one that
// finds no empty slot.
const uint kCellUnreadFrames = 8u;

uint velk_cell_tag(uint hash)
{
    return (hash & 0xFFFFFFF0u) | 8u;
}

bool velk_cell_is(uint at, VelkLightCell c)
{
    return velk_light_cell_words.data[at + 1u] == c.object
        && velk_light_cell_words.data[at + 2u] == uint(c.at.x)
        && velk_light_cell_words.data[at + 3u] == uint(c.at.y)
        && velk_light_cell_words.data[at + 4u] == uint(c.at.z)
        && velk_light_cell_words.data[at + 5u] == (uint(c.level + 64) * 8u + c.side);
}

// The word a cell starts at, or kNoCell. With `make`, an absent cell is made, and `made`
// says this thread made it and holds it as busy.
uint velk_cell_find(VelkLightCell c, bool make, out bool made)
{
    made = false;
    uint hash = velk_light_cell_hash(c);
    uint tag = velk_cell_tag(hash);
    uint now = VELK_GLOBALS.present_counter;
    // Where a new cell could go: an empty slot, or else the one whose cell was read longest
    // ago, if not in the last kCellUnreadFrames frames, so that cells that left the view
    // give way before those still in it.
    uint spare = kNoCell;
    uint spare_state = 0u;
    uint spare_age = kCellUnreadFrames;
    for (uint p = 0u; p < kCellProbes; ++p) {
        uint at = CELLS_WORD + ((hash + p) & (CELLS_SLOTS - 1u)) * kCellWords;
        uint state = velk_light_cell_words.data[at];
        if (state == 0u) {
            // Nothing was ever put past an empty slot.
            spare = at;
            spare_state = 0u;
            break;
        }
        // Its key is written before its light, so a traced cell can be told from another
        // with the same tag.
        if ((state & 0xFFFFFFFCu) == tag && ((state & kCellReady) == 0u || velk_cell_is(at, c))) return at;
        uint age = now - velk_light_cell_words.data[at + 7u];
        if ((state & kCellBusy) == 0u && age >= spare_age) {
            spare = at;
            spare_state = state;
            spare_age = age + 1u;
        }
    }
    if (!make || spare == kNoCell) return kNoCell;
    // Another thread may take the slot first. This pixel then has no cell this frame.
    if (atomicCompSwap(velk_light_cell_words.data[spare], spare_state, tag | kCellBusy) != spare_state) return kNoCell;
    velk_light_cell_words.data[spare + 1u] = c.object;
    velk_light_cell_words.data[spare + 2u] = uint(c.at.x);
    velk_light_cell_words.data[spare + 3u] = uint(c.at.y);
    velk_light_cell_words.data[spare + 4u] = uint(c.at.z);
    velk_light_cell_words.data[spare + 5u] = uint(c.level + 64) * 8u + c.side;
    velk_light_cell_words.data[spare + 7u] = now;
    made = true;
    return spare;
}

uint velk_cell_mix(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

bool velk_segment_touches(vec3 from, vec3 to, vec3 box_min, vec3 box_max)
{
    vec3 d = to - from;
    vec3 inv_d = 1.0 / d;
    vec3 t0 = (box_min - from) * inv_d;
    vec3 t1 = (box_max - from) * inv_d;
    vec3 tmn = min(t0, t1);
    vec3 tmx = max(t0, t1);
    return max(max(max(tmn.x, tmn.y), tmn.z), 0.0) <= min(min(min(tmx.x, tmx.y), tmx.z), 1.0);
}
)"
                                                                      R"(
// Which lights the surface at `world_pos` may see differently for this frame's changes, as a
// bit a light, the first 32 in x. A change is a box, grown here by `margin`: one that lies on
// the way to a light. Or it is a light that moved, named in the change's last word, which
// alters that light alone, wherever it reaches. Every light if everything changed.
uvec2 velk_cell_changed_lights(vec3 world_pos, vec3 N, float margin)
{
    uint count = velk_light_cell_words.data[CELLS_CHANGES];
    if (velk_light_cell_words.data[CELLS_CHANGES + 1u] != 0u) return uvec2(0xFFFFFFFFu);
    uvec2 changed = uvec2(0u);
    vec3 cam = VELK_GLOBALS.cam_pos.xyz;
    uint lights = min(pc.light_count, kCellLights);
    for (uint b = 0u; b < count; ++b) {
        uint at = CELLS_CHANGES + 4u + b * 8u;
        uint moved_light = velk_light_cell_words.data[at + 7u];
        if (moved_light != 0xFFFFFFFFu) {
            if (moved_light >= lights) continue;
            Light light = velk_light_at(pc.lights_base + moved_light);
            vec3 to = light.position.xyz - world_pos;
            bool reaches = light.flags.x == 3u
                ? velk_area_diffuse(light.position.xyz, light.direction.xyz, light.params.w, world_pos, N) > 0.0
                : dot(to, to) < light.params.x * light.params.x && dot(N, to) > 0.0;
            uvec2 bit = moved_light < 32u ? uvec2(1u << moved_light, 0u) : uvec2(0u, 1u << (moved_light - 32u));
            if (reaches && (changed & bit) == uvec2(0u)) {
                changed |= bit;
                velk_cell_stat(kStatChangeLight, 1u);
            }
            continue;
        }
        vec3 box_min = vec3(uintBitsToFloat(velk_light_cell_words.data[at]),
                            uintBitsToFloat(velk_light_cell_words.data[at + 1u]),
                            uintBitsToFloat(velk_light_cell_words.data[at + 2u])) - cam - margin;
        vec3 box_max = vec3(uintBitsToFloat(velk_light_cell_words.data[at + 4u]),
                            uintBitsToFloat(velk_light_cell_words.data[at + 5u]),
                            uintBitsToFloat(velk_light_cell_words.data[at + 6u])) - cam + margin;
        for (uint i = 0u; i < lights; ++i) {
            uvec2 bit = i < 32u ? uvec2(1u << i, 0u) : uvec2(0u, 1u << (i - 32u));
            if ((changed & bit) != uvec2(0u)) continue;
            Light light = velk_light_at(pc.lights_base + i);
            if (light.flags.x == 3u) {
                // A square: the way to its middle and to each corner.
                vec3 a;
                vec3 b;
                velk_area_axes(light.direction.xyz, light.params.w, a, b);
                bool crossed = false;
                for (int c = 0; c < 5 && !crossed; ++c) {
                    vec2 on = c == 0 ? vec2(0.0) : vec2((c & 1) != 0 ? 1.0 : -1.0, c >= 3 ? 1.0 : -1.0);
                    vec3 corner = light.position.xyz + a * on.x + b * on.y;
                    crossed = dot(N, corner - world_pos) > 0.0 && velk_segment_touches(world_pos, corner, box_min, box_max);
                }
                if (crossed) {
                    changed |= bit;
                    velk_cell_stat(kStatChangeBoxArea, 1u);
                    if (all(greaterThanEqual(world_pos, box_min)) && all(lessThanEqual(world_pos, box_max))) {
                        velk_cell_stat(kStatChangeInside, 1u);
                    }
                }
                continue;
            }
            vec3 to = world_pos - light.direction.xyz * 1e4;
            if (light.flags.x != 0u) {
                to = light.position.xyz;
                vec3 by = to - world_pos;
                if (dot(by, by) >= light.params.x * light.params.x) continue;
            }
            if (dot(N, to - world_pos) <= 0.0) continue;
            if (velk_segment_touches(world_pos, to, box_min, box_max)) {
                changed |= bit;
                velk_cell_stat(light.flags.x == 0u ? kStatChangeBoxSun : kStatChangeBoxOther, 1u);
                if (all(greaterThanEqual(world_pos, box_min)) && all(lessThanEqual(world_pos, box_max))) {
                    velk_cell_stat(kStatChangeInside, 1u);
                }
            }
        }
    }
    return changed;
}

// Where the cell with key `object`, `at`, `level` and `side` is, from the camera, and the way
// its side faces, from its object's place now (see CELLS_OBJECTS): the middle of its cube.
// False if its object's place is not known.
bool velk_cell_place(uint object, ivec3 at, int level, uint side, out vec3 world_pos, out vec3 N)
{
    world_pos = vec3(0.0);
    N = vec3(0.0, 1.0, 0.0);
    uint slot = object & 0x3FFFFFFu;
    if (CELLS_OBJECTS == 0xFFFFFFFFu || object == 0u || slot >= CELLS_OBJECT_SLOTS) return false;
    uint e = CELLS_OBJECTS + slot * 13u;
    if (velk_light_cell_words.data[e + 12u] != (object >> 26u) + 1u) return false;
    vec4 r0 = uintBitsToFloat(uvec4(velk_light_cell_words.data[e], velk_light_cell_words.data[e + 1u],
                                    velk_light_cell_words.data[e + 2u], velk_light_cell_words.data[e + 3u]));
    vec4 r1 = uintBitsToFloat(uvec4(velk_light_cell_words.data[e + 4u], velk_light_cell_words.data[e + 5u],
                                    velk_light_cell_words.data[e + 6u], velk_light_cell_words.data[e + 7u]));
    vec4 r2 = uintBitsToFloat(uvec4(velk_light_cell_words.data[e + 8u], velk_light_cell_words.data[e + 9u],
                                    velk_light_cell_words.data[e + 10u], velk_light_cell_words.data[e + 11u]));
    mat3 m = mat3(vec3(r0.x, r1.x, r2.x), vec3(r0.y, r1.y, r2.y), vec3(r0.z, r1.z, r2.z));
    // Keys are in the object's axes scaled by their lengths, as the G-buffer's surface is.
    vec3 axis_scale = max(vec3(length(m[0]), length(m[1]), length(m[2])), vec3(1e-12));
    vec3 middle = (vec3(at) + 0.5) * velk_light_cell_size(level);
    world_pos = m * (middle / axis_scale) + vec3(r0.w, r1.w, r2.w) - VELK_GLOBALS.cam_pos.xyz;
    vec3 n = vec3(0.0);
    n[side >> 1u] = (side & 1u) != 0u ? -1.0 : 1.0;
    N = normalize(transpose(inverse(m)) * n);
    return true;
}
)"
                                                                      R"(
// The bounce entry of cell `cell` (its word in the cell table), or kNoCell. With `make`, an
// absent one takes an empty entry, or one unused for kBounceUnusedFrames, and `made` says so.
uint velk_bounce_find(uint cell, bool make, out bool made)
{
    made = false;
    uint now = VELK_GLOBALS.present_counter;
    uint tag = velk_light_cell_words.data[cell] & 0xFFFFFFFCu;
    uint hash = velk_light_cell_mix(cell * 2654435761u);
    uint spare = kNoCell;
    uint spare_owner = 0u;
    for (uint p = 0u; p < kBounceProbes; ++p) {
        uint at = CELLS_BOUNCE + ((hash + p) & (CELLS_BOUNCE_ENTRIES - 1u)) * kBounceWords;
        uint owner = velk_light_cell_words.data[at];
        bool same_cell = owner == cell && velk_light_cell_words.data[at + 15u] == tag;
        if (same_cell) return at;
        bool unused = now - velk_light_cell_words.data[at + 1u] > kBounceUnusedFrames
                   && velk_light_cell_words.data[at + 2u] != kBounceListed;
        if (spare == kNoCell && (owner == 0u || unused || owner == cell)) {
            spare = at;
            spare_owner = owner;
        }
    }
    if (!make || spare == kNoCell) return kNoCell;
    // Another thread may take the entry first; this pixel then has none this frame.
    if (atomicCompSwap(velk_light_cell_words.data[spare], spare_owner, cell) != spare_owner) return kNoCell;
    velk_light_cell_words.data[spare + 1u] = now;
    velk_light_cell_words.data[spare + 2u] = kBounceUntraced;
    velk_light_cell_words.data[spare + 14u] = 0u;
    for (uint w = 4u; w < 10u; ++w) velk_light_cell_words.data[spare + w] = 0u;
    velk_light_cell_words.data[spare + 15u] = tag;
    made = true;
    return spare;
}
)";

/// What tracing light cells needs. Follows light_cells_src.
[[maybe_unused]] constexpr string_view light_cells_trace_src = R"(
// How much of a light a cell's surface sees is traced with rays that start from places
// spread over the cell's surface around `world_pos`, its middle, the same ones every time,
// so that the result is the share of the cell that is lit and does not change between
// tracings. `world_u` and `world_v` are the world steps for a unit step along the two
// object axes the surface lies in, or zero if unknown, when every ray starts at the middle.

// How many rays a cell takes for a light: a grid's worth, four by four, or eight by eight
// for a light with a size on a cell above pixel size. A larger cell takes more, so that rays
// per area of surface stay about the same and the steps its value moves in get finer.
int velk_cell_grid(Light light, int up)
{
    return light.params.w > 0.0 && up > 0 ? 8 : 4;
}

// Ray `k` of those. Each starts from its own place in the grid over the cell's surface and
// goes toward its own part of the light if the light has a size. How many get through is
// how much of the cell is lit, which is the soft edge: wider for a larger light and for a
// shadow falling further from what casts it. Returns 1 if it got through, else 0 with how
// far it got in `stopped_at`. `moving` says whether something that moves could be in its way.
float velk_cell_ray(Light light, uint i, int k, int grid, vec3 world_pos, vec3 world_u, vec3 world_v, vec3 N,
                    float cell_size, float lift, out float stopped_at, out bool moving)
{
    stopped_at = 0.0;
    moving = false;
    int rays = grid * grid;
    float size = max(light.params.w, 0.0);
    vec2 place = (vec2(float(k % grid), float(k / grid)) + 0.5) / float(grid) - 0.5;
    vec3 from = world_pos + (world_u * place.x + world_v * place.y) * cell_size;
    vec3 toward = -light.direction.xyz;
    float reach = 1e30;
    // How far short of where it aims a ray stops: the light's size for a sphere, which the
    // ray aims at the middle of.
    float short_by = max(size, 0.005);
    if (light.flags.x == 3u) {
        // A square: spread evenly over it, in an order unlike the grid's.
        vec3 a;
        vec3 b;
        velk_area_axes(light.direction.xyz, size, a, b);
        int j = (k * (rays == 16 ? 7 : 23) + 3) % rays;
        vec2 on = (vec2(float(j % grid), float(j / grid)) + 0.5) / float(grid) * 2.0 - 1.0;
        vec3 to_light = light.position.xyz + a * on.x + b * on.y - from;
        reach = length(to_light);
        toward = to_light / max(reach, 1e-6);
        short_by = 0.005;
        // Below the surface's horizon: the closed form leaves that part of the light out
        // already, so it counts as getting through.
        if (dot(N, toward) <= 0.0) return 1.0;
    } else if (light.flags.x != 0u) {
        vec3 to_light = light.position.xyz - from;
        reach = length(to_light);
        toward = to_light / max(reach, 1e-6);
    }
    if (dot(N, toward) <= 0.0) return 0.0;
    if (size > 0.0 && light.flags.x != 3u) {
        float spread = light.flags.x == 0u ? tan(size) : size / max(reach, 1e-4);
        vec3 up = abs(toward.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 T = normalize(cross(up, toward));
        vec3 B = cross(toward, T);
        // Spread evenly over the light's disc, in an order unlike the grid's, and the same
        // for every cell, so that cells side by side differ only by what they see.
        float turn_by = float(velk_cell_mix(i * 747796405u + 1u) & 0xFFFFu) / 65536.0 * 6.28318531;
        float j = float((k * (rays == 16 ? 7 : 23) + 3) % rays);
        float turn = j * 2.39996323 + turn_by;
        float out_by = sqrt((j + 0.5) / float(rays)) * spread;
        toward = normalize(toward + T * (out_by * cos(turn)) + B * (out_by * sin(turn)));
    }
    Ray r;
    r.origin = from + toward * 0.005 + N * lift;
    r.dir = toward;
    velk_ray_lift = lift;
    velk_ray_met_motion = false;
    // The nearest thing in the way, since how far it is sets the soft edge's width.
    RayHit stop;
    bool stopped = trace_closest_hit(r, max(reach - short_by, 0.0), stop);
    velk_ray_lift = 0.0;
    moving = velk_ray_met_motion;
    if (!stopped) return 1.0;
    stopped_at = stop.t;
    return 0.0;
}
)"
                                                                      R"(
// What the threads of a group found, one ray each: whether it got through, how far it got
// if not, and whether something that moves could be in its way.
shared float s_through[64];
shared float s_stopped_at[64];
shared bool s_moving[64];

// Where on cell `own` its rays start, as the pixel `packed` that listed it shows: the cell's
// middle on the surface, how to step over the surface from it along the two object axes the
// surface lies in (zero if unknown), its normal, and how far to lift the rays off it. False
// if the pixel shows nothing.
bool velk_cell_place_from_pixel(uint packed, uint own, float cell_size, out vec3 middle_world, out vec3 world_u,
                                out vec3 world_v, out vec3 N, out float lift)
{
    world_u = vec3(0.0);
    world_v = vec3(0.0);
    ivec2 coord = ivec2(int(packed & 0xFFFFu), int(packed >> 16u));
    uvec2 ids = texelFetch(velk_textures_uint[nonuniformEXT(CELLS_IDS)], coord, 0).xy;
    vec4 normal_and_error = texelFetch(velk_textures[nonuniformEXT(pc.normal_tex_id)], coord, 0);
    uint bits = velk_draw_data.data[max(ids.x, 1u) - 1u].triangle_bits;
    uint position = ids.y >> bits;
    vec4 surface = texelFetch(velk_textures[nonuniformEXT(CELLS_SURFACE)], coord, 0);
    vec3 world_pos = texelFetch(velk_textures[nonuniformEXT(pc.worldpos_tex_id)], coord, 0).xyz;
    N = normalize(normal_and_error.xyz);
    uint side = uint(surface.w + 0.5);

    // Where the surface steps along the object's axes in the world comes from how position on
    // the object and in the world change together across the next pixels, if they show the
    // same surface.
    vec3 middle = (vec3(ivec3(velk_light_cell_words.data[own + 2u], velk_light_cell_words.data[own + 3u],
                               velk_light_cell_words.data[own + 4u])) + 0.5) * cell_size;
    uint axis = side >> 1u;
    uint ua = (axis + 1u) % 3u;
    uint va = (axis + 2u) % 3u;
    vec2 to_middle = vec2(middle[ua] - surface.xyz[ua], middle[va] - surface.xyz[va]);
    ivec2 image_size = textureSize(velk_textures[nonuniformEXT(pc.normal_tex_id)], 0);
    ivec2 nx = coord + ivec2(coord.x + 1 < image_size.x ? 1 : -1, 0);
    ivec2 ny = coord + ivec2(0, coord.y + 1 < image_size.y ? 1 : -1);
    uvec2 ids_x = texelFetch(velk_textures_uint[nonuniformEXT(CELLS_IDS)], nx, 0).xy;
    uvec2 ids_y = texelFetch(velk_textures_uint[nonuniformEXT(CELLS_IDS)], ny, 0).xy;
    vec4 surface_x = texelFetch(velk_textures[nonuniformEXT(CELLS_SURFACE)], nx, 0);
    vec4 surface_y = texelFetch(velk_textures[nonuniformEXT(CELLS_SURFACE)], ny, 0);
    vec3 normal_x = texelFetch(velk_textures[nonuniformEXT(pc.normal_tex_id)], nx, 0).xyz;
    vec3 normal_y = texelFetch(velk_textures[nonuniformEXT(pc.normal_tex_id)], ny, 0).xyz;
    // The same object, the same side of it, and facing the same way: one flat piece of
    // surface, not the next face over an edge.
    if (ids_x.x == ids.x && (ids_x.y >> bits) == position
        && ids_y.x == ids.x && (ids_y.y >> bits) == position
        && surface_x.w == surface.w && surface_y.w == surface.w
        && dot(normal_x, N) > 0.995 * length(normal_x) && dot(normal_y, N) > 0.995 * length(normal_y)) {
        vec3 sx = surface_x.xyz - surface.xyz;
        vec3 sy = surface_y.xyz - surface.xyz;
        vec3 wx = texelFetch(velk_textures[nonuniformEXT(pc.worldpos_tex_id)], nx, 0).xyz - world_pos;
        vec3 wy = texelFetch(velk_textures[nonuniformEXT(pc.worldpos_tex_id)], ny, 0).xyz - world_pos;
        float det = sx[ua] * sy[va] - sy[ua] * sx[va];
        if (abs(det) > 1e-12) {
            // The world step for a unit step along each of the two object axes.
            world_u = (wx * sy[va] - wy * sx[va]) / det;
            world_v = (wy * sx[ua] - wx * sy[ua]) / det;
            // A step much longer than the object step is a silhouette, not a surface.
            if (dot(world_u, world_u) > 4.0 || dot(world_v, world_v) > 4.0) {
                world_u = vec3(0.0);
                world_v = vec3(0.0);
            }
        }
    }
    middle_world = world_pos + world_u * to_middle.x + world_v * to_middle.y;
    // Lifted by a little of the cell's size, since the places rays start from are only as
    // exact as the differences between pixels they were found from.
    lift = normal_and_error.w + 0.02 * cell_size;
    return ids.x != 0u;
}
)";

/// The main of the pass that traces light cells. Follows light_cells_src and
/// light_cells_trace_src.
[[maybe_unused]] constexpr string_view light_cells_trace_main_src = R"(
// Traces what light_cells_touch_src listed. A thread group takes one cell and one light,
// and each of its threads one ray, so that no thread traces ray after ray while the pass
// waits for it. The first thread then puts together what the cell holds for that light.
// With `pc.mode` 2 the group empties the list instead, for after the tracing, and
// works out how often lights that moving things could be in the way of are traced again:
// as often as the room the rest of the list left this frame allows. With `pc.mode` 3,
// before anything else of the frame, a thread a slot turns the light gathered on its cell
// last frame into the cell's mean (kCellArriveSum into kCellArrive) and empties it; a cell
// nothing reached keeps the mean it had.
void main()
{
    velk_cells_load();
    uint lane = gl_LocalInvocationIndex;
    uint work = CELLS_WORK;
    if (pc.mode == 3u) {
        uint slot = gl_WorkGroupID.x * 64u + lane;
        if (slot >= CELLS_SLOTS) return;
        uint at = CELLS_WORD + slot * kCellWords;
        uint count = velk_light_cell_words.data[at + kCellArriveSum + 3u];
        if (velk_light_cell_words.data[at] == 0u || count == 0u) return;
        vec3 mean = vec3(uvec3(velk_light_cell_words.data[at + kCellArriveSum], velk_light_cell_words.data[at + kCellArriveSum + 1u],
                               velk_light_cell_words.data[at + kCellArriveSum + 2u]))
                  / (float(count) * kArriveScale);
        uint now = VELK_GLOBALS.present_counter & 0xFFFFu;
        velk_light_cell_words.data[at + kCellArrive] = packHalf2x16(mean.rg);
        velk_light_cell_words.data[at + kCellArrive + 1u] = (packHalf2x16(vec2(mean.b, 0.0)) & 0xFFFFu) | (now << 16u);
        for (uint w = 0u; w < 4u; ++w) velk_light_cell_words.data[at + kCellArriveSum + w] = 0u;
        return;
    }
    if (pc.mode == 2u) {
        if (gl_WorkGroupID.x == 0u && lane == 0u) {
            uint asked = velk_light_cell_words.data[work];
            uint load = velk_light_cell_words.data[work + kCellWorkLiveLoad];
            uint others = asked - min(velk_light_cell_words.data[work + kCellWorkLiveAsked], asked);
            uint room = CELLS_ENTRIES > others ? CELLS_ENTRIES - others : 0u;
            uint period = room == 0u ? kCellLivePeriodMax : clamp((load + room - 1u) / room, 1u, kCellLivePeriodMax);
            velk_light_cell_words.data[work + kCellWorkLivePeriod] = period;
            velk_light_cell_words.data[work + kCellWorkLiveLoad] = 0u;
            velk_light_cell_words.data[work + kCellWorkLiveAsked] = 0u;
            velk_light_cell_words.data[work] = 0u;
            velk_light_cell_words.data[CELLS_BOUNCE_LIST] = 0u;
            velk_cell_stat(kStatFrames, 1u);
            velk_cell_stat(kStatLiveLoad, load);
        }
        return;
    }
    // The same for every thread of the group, so that all of them reach the barrier below
    // or none does.
    uint listed = min(velk_light_cell_words.data[work], CELLS_ENTRIES);
    uint entry = work + kCellWorkHeader + gl_WorkGroupID.x * kCellWorkWords;
    uint own = gl_WorkGroupID.x < listed ? velk_light_cell_words.data[entry] : kNoCell;
    if (own == kNoCell) return;
    if (lane == 0u) velk_cell_stat(kStatTraced, 1u);
    uint packed = velk_light_cell_words.data[entry + 1u];
    int up = int(velk_light_cell_words.data[entry + 2u] & 255u);
    uint i = velk_light_cell_words.data[entry + 2u] >> 8u;
    Light light = velk_light_at(pc.lights_base + i);
    int grid = velk_cell_grid(light, up);
    float cell_size = velk_light_cell_size(int(velk_light_cell_words.data[own + 5u] >> 3u) - 64);

    vec3 middle_world;
    vec3 world_u = vec3(0.0);
    vec3 world_v = vec3(0.0);
    vec3 N;
    float lift;
    bool valid;
    if (packed == kCellNoPixel) {
        // Found by a ray rather than a pixel: the entry says where on the cell and which way
        // it faces, and every ray starts there.
        middle_world = vec3(uintBitsToFloat(velk_light_cell_words.data[entry + 4u]),
                            uintBitsToFloat(velk_light_cell_words.data[entry + 5u]),
                            uintBitsToFloat(velk_light_cell_words.data[entry + 6u]));
        N = velk_cell_unpack_normal(velk_light_cell_words.data[entry + 3u]);
        lift = 0.02 * cell_size;
        valid = true;
    } else {
        valid = velk_cell_place_from_pixel(packed, own, cell_size, middle_world, world_u, world_v, N, lift);
    }

    // One ray a thread.
    s_through[lane] = 0.0;
    s_stopped_at[lane] = 0.0;
    s_moving[lane] = false;
    if (int(lane) < grid * grid && valid) {
        float stopped_at;
        bool moving;
        s_through[lane] = velk_cell_ray(light, i, int(lane), grid, middle_world, world_u, world_v, N, cell_size,
                                        lift, stopped_at, moving);
        s_stopped_at[lane] = stopped_at;
        s_moving[lane] = moving;
    }
    barrier();
    if (lane != 0u) return;

    float through = 0.0;
    float stopped = 0.0;
    float stopped_at_all = 0.0;
    bool moving = false;
    for (int k = 0; k < grid * grid; ++k) {
        through += s_through[k];
        moving = moving || s_moving[k];
        if (s_stopped_at[k] > 0.0) {
            stopped += 1.0;
            stopped_at_all += s_stopped_at[k];
        }
    }
    uint fifteenths = uint(clamp(through / float(grid * grid), 0.0, 1.0) * 15.0 + 0.5);
    // The soft edge's width here: the light's width as seen from what casts the shadow,
    // carried on to the surface. Then how many half levels larger than this cell a cell
    // would be with kCellsPerEdge of them across that.
    uint half_levels = 0u;
    float size = max(light.params.w, 0.0);
    if (size > 0.0 && stopped > 0.0) {
        float to_blocker = stopped_at_all / stopped;
        float to_light = light.flags.x == 0u ? 1e30 : distance(light.position.xyz, middle_world);
        float edge = light.flags.x == 0u ? 2.0 * tan(size) * to_blocker
                                         : 2.0 * size * to_blocker / max(to_light - to_blocker, 1e-3);
        half_levels = uint(clamp(2.0 * log2(max(edge, 1e-6) / (kCellsPerEdge * cell_size)) + 0.5, 0.0, 7.0));
    }
    // Motion smaller than the cell would not show in it.
    uint live = 0u;
#ifdef VELK_RAY_MOTION
    if (moving && VELK_RAY_MOTION_REACH >= cell_size) live = kCellLive;
#endif
    // Four lights share a word, and other groups may be at the other three.
    uint shift = (i & 3u) * 8u;
    atomicAnd(velk_light_cell_words.data[own + 8u + (i >> 2u)], ~(255u << shift));
    atomicOr(velk_light_cell_words.data[own + 8u + (i >> 2u)], (fifteenths | (half_levels << 4u) | live) << shift);
    // Traced now: nothing changed for this light since.
    atomicAnd(velk_light_cell_words.data[own + kCellChanged + (i >> 5u)], ~(1u << (i & 31u)));
}
)";

/// Notes which light cells the pixels need, makes the new ones, checks them against what
/// changed, and lists those to trace for light_cells_trace_src. Follows light_cells_src.
/// It traces nothing and holds no arrays, since nearly every thread only finds a cell.
[[maybe_unused]] constexpr string_view light_cells_touch_src = R"(
// The pixel being worked on, for the functions below. Set by velk_cells_begin.
ivec2 coord;
uvec2 ids;
VelkDrawData d;
uint position;
uint object;
vec4 surface;
vec3 world_pos;
vec3 N;
uint side;
vec4 normal_and_error;

// The pass runs three times a frame, `pc.mode` saying which round it is. The first
// lists only the cells kCellUp levels up that were never traced, which pixels waiting for
// their own cell show meanwhile; one covers many cells, so a view coming into sight has a
// rough shadow at once. The second lists the other cells never traced, which show as gaps.
// The third the rest, with what is left of the list.
const uint kCellRoundCoarse = 0u;
const uint kCellRoundGaps = 1u;
const uint kCellRoundRest = 2u;

// Whether the place being worked on, `world_pos` and `N` below, came from a pixel. Passes
// that find cells with rays clear it, and their entries then carry the place themselves.
bool cells_from_pixel = true;

// The lights cell `at` holds as ones that moving things could be in the way of, a bit a
// light, the first 32 in x.
uvec2 velk_cell_live_lights(uint at)
{
    uvec2 live = uvec2(0u);
    uint words = (min(pc.light_count, kCellLights) + 3u) / 4u;
    for (uint w = 0u; w < words; ++w) {
        uint bits = velk_light_cell_words.data[at + 8u + w] & 0x80808080u;
        while (bits != 0u) {
            uint i = w * 4u + uint(findLSB(bits)) / 8u;
            bits &= bits - 1u;
            if (i < 32u) live.x |= 1u << i; else live.y |= 1u << (i - 32u);
        }
    }
    return live;
}

// Keeps cell `c` up for the pixel: made if new, checked against what changed, and listed
// for tracing if it is due and it is this pixel's turn. `up` is how many levels above the
// pixel's own it is. Returns the word it starts at, or kNoCell.
uint velk_cell_keep(VelkLightCell c, int up)
{
    float cell_size = velk_light_cell_size(c.level);
    bool made;
    if (pc.mode == kCellRoundCoarse && up != kCellUp) return velk_cell_find(c, false, made);
    uint own = velk_cell_find(c, true, made);
    uint now = VELK_GLOBALS.present_counter;
    if (pc.mode != kCellRoundRest && own != kNoCell && !made
        && (velk_light_cell_words.data[own] & kCellReady) != 0u) {
        return own;
    }
    // Several pixels share a cell. The first to come to it this frame notes it as read and
    // keeps it up; the rest only use it. The first time through, holding the cell does that.
    if (pc.mode == kCellRoundRest && own != kNoCell && !made
        && (velk_light_cell_words.data[own + 7u] == now || atomicExchange(velk_light_cell_words.data[own + 7u], now) == now)) {
        return own;
    }
    if (own != kNoCell) {
        uint tag = velk_light_cell_words.data[own] & 0xFFFFFFFCu;
        // Traced: the lights something changed on the way to since are marked on it (see
        // light_cells_mark_main_src), and only those are traced again.
        uvec2 which = uvec2(0xFFFFFFFFu);
        bool changed = false;
        if (velk_light_cell_words.data[own] == (tag | kCellReady)) {
            which = uvec2(velk_light_cell_words.data[own + kCellChanged], velk_light_cell_words.data[own + kCellChanged + 1u]);
            changed = (which.x | which.y) != 0u;
            if (changed && up > 0 && which != uvec2(0xFFFFFFFFu)) {
                velk_cell_stat(kStatChangeCoarse, uint(bitCount(which.x) + bitCount(which.y)));
            }
            if (!changed) velk_light_cell_words.data[own + 6u] = VELK_GLOBALS.present_counter;
        }
        // What the cell goes back to if it is not listed after all.
        uint as_it_was = tag;
        bool hold = made;
        // Not traced yet and nobody at it.
        if (!hold) {
            uint was = atomicCompSwap(velk_light_cell_words.data[own], tag, tag | kCellBusy);
            hold = was == tag;
            if (was == (tag | kCellBusy)) velk_cell_stat(kStatBusy, 1u);
        }
        // Traced: again if something changed on the way to a light. Besides that one cell in
        // 256 is due each frame, for what the list of changes does not hold.
        bool refresh = (((own / kCellWords) + VELK_GLOBALS.present_counter) & 255u) == 0u;
        // Lights that moving things could be in the way of are traced again in turn, each
        // cell every so many frames, as many as the list has room for after the rest. The
        // trace pass works out how many frames from how many there were.
        uvec2 live = uvec2(0u);
        bool live_due = false;
        if (pc.mode == kCellRoundRest && velk_light_cell_words.data[own] == (tag | kCellReady)) {
            live = velk_cell_live_lights(own);
            uint live_lights = bitCount(live.x) + bitCount(live.y);
            if (live_lights != 0u) {
                atomicAdd(velk_light_cell_words.data[CELLS_WORK + kCellWorkLiveLoad], live_lights);
                uint period = max(velk_light_cell_words.data[CELLS_WORK + kCellWorkLivePeriod], 1u);
                live_due = ((own / kCellWords) + now) % period == 0u;
            }
        }
        bool due = changed || refresh || live_due;
        if (!hold && due) {
            hold = atomicCompSwap(velk_light_cell_words.data[own], tag | kCellReady,
                                  tag | kCellReady | kCellBusy) == (tag | kCellReady);
            as_it_was = tag | kCellReady;
            if (!changed) which = live_due && !refresh ? live : uvec2(0xFFFFFFFFu);
            else if (live_due) which |= live;
        } else if (hold || !due) {
            // New, or never traced: every light.
            which = uvec2(0xFFFFFFFFu);
        }
        if (hold) {
            // The cell is this thread's until it lets go. Each light to trace that reaches the
            // surface here becomes one entry of the list, traced by a group of its own.
            uint lights = min(pc.light_count, kCellLights);
            uvec2 reaching = uvec2(0u);
            uint entries = 0u;
            for (uint i = 0u; i < lights; ++i) {
                if (((i < 32u ? which.x >> i : which.y >> (i - 32u)) & 1u) == 0u) continue;
                Light light = velk_light_at(pc.lights_base + i);
                if (light.flags.x == 3u) {
                    // A square reaches as far as its closed form does: wherever any of it is
                    // above the surface's horizon.
                    if (velk_area_diffuse(light.position.xyz, light.direction.xyz, light.params.w, world_pos, N) <= 0.0) {
                        continue;
                    }
                } else {
                    vec3 to = -light.direction.xyz;
                    if (light.flags.x != 0u) {
                        to = light.position.xyz - world_pos;
                        if (dot(to, to) >= light.params.x * light.params.x) continue;
                        if (light.flags.x == 2u
                            && dot(-normalize(to), light.direction.xyz) <= light.params.z) continue;
                    }
                    if (dot(N, to) <= 0.0) continue;
                }
                if (i < 32u) reaching.x |= 1u << i; else reaching.y |= 1u << (i - 32u);
                ++entries;
            }
            // Marks for lights that do not reach it need nothing traced.
            atomicAnd(velk_light_cell_words.data[own + kCellChanged], reaching.x);
            atomicAnd(velk_light_cell_words.data[own + kCellChanged + 1u], reaching.y);
            // All of them have to fit in what the frame may trace, or the cell waits. A part
            // of the list taken and not used is marked as holding no cell.
            uint limit = CELLS_ENTRIES;
            uint first = entries > 0u ? atomicAdd(velk_light_cell_words.data[CELLS_WORK], entries) : 0u;
            velk_cell_stat(kStatAsked, entries);
            if (pc.mode == kCellRoundCoarse) velk_cell_stat(kStatCoarseAsked, entries);
            if (changed) velk_cell_stat(kStatChanged, 1u);
            if (changed && which == uvec2(0xFFFFFFFFu)) velk_cell_stat(kStatStale, 1u);
            if (made) velk_cell_stat(kStatMade, 1u);
            bool only_live = live_due && !changed && !refresh;
            if (only_live) atomicAdd(velk_light_cell_words.data[CELLS_WORK + kCellWorkLiveAsked], entries);
            if (first + entries > limit) {
                for (uint e = first; e < min(first + entries, CELLS_ENTRIES); ++e) {
                    velk_light_cell_words.data[CELLS_WORK + kCellWorkHeader + e * kCellWorkWords] = kNoCell;
                }
                velk_light_cell_words.data[own] = as_it_was;
                velk_cell_stat(kStatRefused, 1u);
                if (pc.mode == kCellRoundCoarse) velk_cell_stat(kStatCoarseRefused, 1u);
            } else {
                velk_cell_stat(kStatListed, entries);
                velk_cell_stat(kStatCells, 1u);
                if (only_live) velk_cell_stat(kStatLive, 1u);
                // A cell holds only shadow: a light that does not reach here counts as
                // unblocked, so that if it moves closer it lights the surface at once and its
                // shadow follows when traced. The same for an area light, which works out its
                // own shadow. A light to trace keeps what it held until the trace replaces it,
                // so what is read of it before then this frame is last time's. A new cell
                // held another cell's light, so it starts empty.
                for (uint w = 0u; made && w < kCellWords - 8u; ++w) velk_light_cell_words.data[own + 8u + w] = 0u;
                if (made) {
                    velk_light_cell_words.data[own + kCellArrive + 1u] = ((now & 0xFFFFu) << 16u) | kArriveMade;
                }
                uint e = first;
                for (uint i = 0u; i < lights; ++i) {
                    if (((i < 32u ? which.x >> i : which.y >> (i - 32u)) & 1u) == 0u) continue;
                    uint shift = (i & 3u) * 8u;
                    uint word = velk_light_cell_words.data[own + 8u + (i >> 2u)];
                    bool traced = ((i < 32u ? reaching.x >> i : reaching.y >> (i - 32u)) & 1u) != 0u;
                    if (!traced) word = (word & ~(255u << shift)) | (15u << shift);
                    velk_light_cell_words.data[own + 8u + (i >> 2u)] = word;
                    if (!traced) continue;
                    uint at = CELLS_WORK + kCellWorkHeader + e * kCellWorkWords;
                    velk_light_cell_words.data[at] = own;
                    velk_light_cell_words.data[at + 1u] = cells_from_pixel ? (uint(coord.x) | (uint(coord.y) << 16u)) : kCellNoPixel;
                    velk_light_cell_words.data[at + 2u] = uint(up) | (i << 8u);
                    if (!cells_from_pixel) {
                        velk_light_cell_words.data[at + 3u] = velk_cell_pack_normal(N);
                        velk_light_cell_words.data[at + 4u] = floatBitsToUint(world_pos.x);
                        velk_light_cell_words.data[at + 5u] = floatBitsToUint(world_pos.y);
                        velk_light_cell_words.data[at + 6u] = floatBitsToUint(world_pos.z);
                    }
                    ++e;
                }
                // Everything listed is traced before anything reads the cells, so it counts
                // as traced and good from here.
                velk_light_cell_words.data[own + 6u] = VELK_GLOBALS.present_counter;
                velk_light_cell_words.data[own] = tag | kCellReady;
            }
        }
    }
    return own;
}

// What a cell holds for light `i`. See the layout above.
uint velk_cell_light(uint at, uint i)
{
    return (velk_light_cell_words.data[at + 8u + (i >> 2u)] >> ((i & 3u) * 8u)) & 255u;
}

// Adds the direct light arriving at the place `world_pos` (from the camera) facing `N` on
// traced cell `at` to the cell's mean for this frame. Every place read on the cell adds
// one. See kCellArrive and kCellArriveSum.
void velk_cell_arrive(uint at, vec3 world_pos, vec3 N)
{
    if ((velk_light_cell_words.data[at] & kCellReady) == 0u) return;
    // Made this frame: nothing traced on it until later in the frame.
    if (velk_light_cell_words.data[at + kCellArrive + 1u] == (((VELK_GLOBALS.present_counter & 0xFFFFu) << 16u) | kArriveMade)) {
        return;
    }
    vec3 sum = vec3(0.0);
    for (uint i = 0u; i < pc.light_count; ++i) {
        Light light = velk_light_at(pc.lights_base + i);
        float through = i < kCellLights ? float(velk_cell_light(at, i) & 15u) / 15.0 : 1.0;
        if (light.flags.x == 3u) {
            float area = velk_area_diffuse(light.position.xyz, light.direction.xyz, light.params.w, world_pos, N);
            sum += area * light.color_intensity.rgb * light.color_intensity.a * through;
            continue;
        }
        vec3 L = -light.direction.xyz;
        float atten = 1.0;
        if (light.flags.x != 0u) {
            vec3 to_light = light.position.xyz - world_pos;
            float dist = length(to_light);
            L = to_light / max(dist, 1e-6);
            float t = clamp(1.0 - dist / max(light.params.x, 1e-6), 0.0, 1.0);
            atten = t * t;
            if (light.flags.x == 2u) atten *= smoothstep(light.params.z, light.params.y, dot(-L, light.direction.xyz));
        }
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0 || atten <= 0.0) continue;
        sum += NdotL * light.color_intensity.rgb * light.color_intensity.a * atten * through;
    }
    // Up to 8192 a channel each, so that a thousand places still fit in 32 bits.
    uvec3 fixed_sum = uvec3(clamp(sum * kArriveScale, vec3(0.0), vec3(2097151.0)) + 0.5);
    atomicAdd(velk_light_cell_words.data[at + kCellArriveSum], fixed_sum.r);
    atomicAdd(velk_light_cell_words.data[at + kCellArriveSum + 1u], fixed_sum.g);
    atomicAdd(velk_light_cell_words.data[at + kCellArriveSum + 2u], fixed_sum.b);
    atomicAdd(velk_light_cell_words.data[at + kCellArriveSum + 3u], 1u);
}

)"
                                                                      R"(
// Keeps up the bounce entry of cell `cell` for the pixel at `coord`, and lists it for tracing:
// made and listed if new, in the round that lists cells with no light yet, so these come
// first; in the last round, noted as used by the first pixel to get to it this frame, and
// listed again if something changed on the way of its rays, with what the list has left.
void velk_bounce_keep(uint cell)
{
    bool made;
    uint at = velk_bounce_find(cell, true, made);
    if (at == kNoCell) return;
    // A pixel uses it: no longer at the end of bounce. Its rays keep their start until traced
    // again.
    if ((velk_light_cell_words.data[at + 14u] & kBounceFromRay) != 0u) {
        atomicAnd(velk_light_cell_words.data[at + 14u], ~kBounceFromRay);
    }
    uint list = CELLS_BOUNCE_LIST;
    if (pc.mode == kCellRoundGaps) {
        // Only one pixel moves it from untraced to listed.
        if (atomicCompSwap(velk_light_cell_words.data[at + 2u], kBounceUntraced, kBounceListed) != kBounceUntraced) return;
        velk_cell_stat(kStatBounceAskNew, 1u);
    } else {
        uint now = VELK_GLOBALS.present_counter;
        if (velk_light_cell_words.data[at + 1u] == now || atomicExchange(velk_light_cell_words.data[at + 1u], now) == now) {
            return;
        }
        if (velk_light_cell_words.data[at + 2u] != kBounceTraced
            || (velk_light_cell_words.data[at + 14u] & kBounceStale) == 0u) {
            return;
        }
        velk_cell_stat(kStatBounceAskStale, 1u);
    }
    uint n = atomicAdd(velk_light_cell_words.data[list], 1u);
    if (n >= CELLS_BOUNCE_LIST_MAX) {
        // No room: a new cell waits for a later frame, and a stale one keeps what it holds.
        if (pc.mode == kCellRoundGaps) velk_light_cell_words.data[at + 2u] = kBounceUntraced;
        return;
    }
    velk_cell_stat(kStatBounceListed, 1u);
    velk_light_cell_words.data[at + 2u] = kBounceListed;
    velk_light_cell_words.data[list + 4u + n * 2u] = at;
    velk_light_cell_words.data[list + 4u + n * 2u + 1u] = uint(coord.x) | (uint(coord.y) << 16u);
}

// Takes up the pixel at `pixel`, whose surface is at `rel_pos` from the camera with the
// G-buffer's normal and error `normal_error`: keeps up its own cell, and the larger cells a
// wide soft edge there is read from. False if there is no surface there.
bool velk_cells_begin(ivec2 pixel, vec3 rel_pos, vec4 normal_error)
{
    velk_cells_load();
    coord = pixel;
    ids = texelFetch(velk_textures_uint[nonuniformEXT(CELLS_IDS)], coord, 0).xy;
    normal_and_error = normal_error;
    if (ids.x == 0u || dot(normal_and_error.xyz, normal_and_error.xyz) < 1e-6) return false;
    d = velk_draw_data.data[ids.x - 1u];
    position = ids.y >> d.triangle_bits;
    uint slot = d.visible_base == VELK_NO_VISIBLE_LIST ? position : velk_culling.data[d.visible_base + position];
    object = velk_object_id(d.instances_base + slot);

    surface = texelFetch(velk_textures[nonuniformEXT(CELLS_SURFACE)], coord, 0);
    world_pos = rel_pos;
    N = normalize(normal_and_error.xyz);
    side = uint(surface.w + 0.5);

    mat4 vp = VELK_GLOBALS.view_projection;
    float pixel_width = 2.0 / (length(vec3(vp[0][1], vp[1][1], vp[2][1])) * float(pc.height));
    int level = velk_light_cell_level(length(world_pos), pixel_width, float(CELLS_PIXELS));
    uint own = velk_cell_keep(velk_light_cell(object, surface.xyz, side, level), 0);
    if (pc.mode == kCellRoundRest && own != kNoCell) velk_cell_arrive(own, world_pos, N);
    // The bounce cell over the pixel, which gathers the light from other surfaces here.
    if (pc.mode == kCellRoundGaps || pc.mode == kCellRoundRest) {
        uint bounce_cell = velk_cell_keep(velk_light_cell(object, surface.xyz, side, level + kBounceUp), kBounceUp);
        if (bounce_cell != kNoCell) velk_bounce_keep(bounce_cell);
    }

    // Its own cell says how wide the widest soft edge here is. That is read from larger
    // cells, three by three of them around the pixel at each level, so those are kept up
    // as well, whether or not a pixel of their own asks.
    float widest = 0.0;
    if (own != kNoCell && (velk_light_cell_words.data[own] & kCellReady) != 0u) {
        uint held_lights = min(pc.light_count, kCellLights);
        for (uint i = 0u; i < held_lights; ++i) {
            uint held = velk_cell_light(own, i);
            if ((held & 15u) != 15u) widest = max(widest, float((held >> 4u) & 7u) * 0.5);
        }
    }
    int up_levels = min(int(ceil(widest)), kCellUp);
    // Its own cell not traced yet, waiting for room in the list: the cells kCellUp levels
    // larger are shown meanwhile, and kept up for that. Many pixels share one of them, so a
    // view coming into sight has them soon.
    bool waiting = own == kNoCell || (velk_light_cell_words.data[own] & kCellReady) == 0u;
    if (pc.mode == kCellRoundCoarse && !waiting) return true;
    if (pc.mode == kCellRoundRest && own == kNoCell) velk_cell_stat(kStatNoSlot, 1u);
    if (pc.mode == kCellRoundRest && waiting) velk_cell_stat(kStatWaiting, 1u);
    uint axis = side >> 1u;
    uint ua = (axis + 1u) % 3u;
    uint va = (axis + 2u) % 3u;
    for (int up = waiting ? kCellUp : 1; up <= (waiting ? kCellUp : up_levels); ++up) {
        // The same three by three a reader blends: around the cell whose middle is nearest.
        VelkLightCell c = velk_light_cell(object, surface.xyz, side, level + up);
        ivec3 nearest = ivec3(floor(surface.xyz / velk_light_cell_size(level + up)));
        for (int k = 0; k < 9; ++k) {
            c.at[ua] = nearest[ua] + k % 3 - 1;
            c.at[va] = nearest[va] + k / 3 - 1;
            uint kept = velk_cell_keep(c, up);
            // The middle one holds the pixel.
            if (k == 4 && pc.mode == kCellRoundRest && kept != kNoCell) velk_cell_arrive(kept, world_pos, N);
            if (k == 4 && waiting && pc.mode == kCellRoundRest
                && (kept == kNoCell || (velk_light_cell_words.data[kept] & kCellReady) == 0u)) {
                velk_cell_stat(kStatCoarseWaiting, 1u);
            }
        }
    }
    return true;
}
)";

/// Keeps the light cells in view up: one pixel in each 2x2 block takes up its cells. Follows
/// deferred_compute_prelude_src, light_cells_src and light_cells_touch_src. Runs before
/// anything reads the cells, so that the passes that read carry none of this.
[[maybe_unused]] constexpr string_view light_cells_update_main_src = R"(
void main()
{
    ivec2 at_pixel = ivec2(gl_GlobalInvocationID.xy) * 2;
    if (at_pixel.x >= int(pc.width) || at_pixel.y >= int(pc.height)) return;
    vec3 rel_pos = texelFetch(velk_textures[nonuniformEXT(pc.worldpos_tex_id)], at_pixel, 0).xyz;
    vec4 normal_error = texelFetch(velk_textures[nonuniformEXT(pc.normal_tex_id)], at_pixel, 0);
    velk_cells_begin(at_pixel, rel_pos, normal_error);
}
)";

/// Traces the rays of the bounce cells listed, a group a cell and a ray a thread, and writes
/// down what each hit, for light_cells_bounce_hits_main_src and the material pass: nothing
/// else is done here, so the walk through the scene has the registers to itself. Follows
/// light_cells_src and light_cells_trace_src.
[[maybe_unused]] constexpr string_view light_cells_bounce_trace_main_src = R"(
void main()
{
    velk_cells_load();
    uint lane = gl_LocalInvocationIndex;
    uint list = CELLS_BOUNCE_LIST;
    uint listed = min(velk_light_cell_words.data[list], CELLS_BOUNCE_LIST_MAX);
    uint n = gl_WorkGroupID.x;
    if (n >= listed) return;
    uint at = velk_light_cell_words.data[list + 4u + n * 2u];
    uint packed = velk_light_cell_words.data[list + 4u + n * 2u + 1u];
    uint cell = velk_light_cell_words.data[at];
    float cell_size = velk_light_cell_size(int(velk_light_cell_words.data[cell + 5u] >> 3u) - 64);
    vec3 middle;
    vec3 cell_n;
    float lift;
    bool from_pixel = packed != kCellNoPixel;
    bool valid = true;
    if (from_pixel) {
        vec3 world_u;
        vec3 world_v;
        valid = velk_cell_place_from_pixel(packed, cell, cell_size, middle, world_u, world_v, cell_n, lift);
    } else {
        // Made by a bounce ray: its rays start where that ray hit, kept when it was made.
        middle = vec3(uintBitsToFloat(velk_light_cell_words.data[at + 10u]),
                      uintBitsToFloat(velk_light_cell_words.data[at + 11u]),
                      uintBitsToFloat(velk_light_cell_words.data[at + 12u])) - VELK_GLOBALS.cam_pos.xyz;
        cell_n = velk_cell_unpack_normal(velk_light_cell_words.data[at + 13u]);
        lift = 0.02 * cell_size;
    }
    uint ray_at = at + kBounceHeader + lane * kBounceRayWords;
    uint scratch = velk_bounce_scratch(n, lane);
    uint flags = velk_light_cell_words.data[at + 14u];
    if (!valid) {
        // The pixel that asked shows nothing now: the cell waits for another, keeping what it
        // holds if it was traced before.
        if (lane == 0u) {
            bool held = (flags & kBounceStale) != 0u;
            velk_light_cell_words.data[at + 2u] = held ? kBounceTraced : kBounceUntraced;
        }
        velk_light_cell_words.data[scratch] = kNoCell;
        return;
    }
    if (from_pixel) {
        // The rays start from the pixel itself, with its own normal, rather than from the
        // cell's middle: on a curved surface the middle faces elsewhere, and rays about the
        // pixel's normal would start into the surface there.
        ivec2 at_pixel = ivec2(int(packed & 0xFFFFu), int(packed >> 16u));
        middle = texelFetch(velk_textures[nonuniformEXT(pc.worldpos_tex_id)], at_pixel, 0).xyz;
    }
    if (lane == 0u) {
        velk_light_cell_words.data[at + 14u] = flags & kBounceFromRay;
        if (from_pixel) {
            vec3 absolute = middle + VELK_GLOBALS.cam_pos.xyz;
            velk_light_cell_words.data[at + 10u] = floatBitsToUint(absolute.x);
            velk_light_cell_words.data[at + 11u] = floatBitsToUint(absolute.y);
            velk_light_cell_words.data[at + 12u] = floatBitsToUint(absolute.z);
            velk_light_cell_words.data[at + 13u] = velk_cell_pack_normal(cell_n);
        }
        velk_light_cell_words.data[at + 2u] = kBounceTraced;
    }

    vec3 dir = velk_bounce_dir(lane, cell_n, velk_bounce_turn(cell));
    Ray ray;
    ray.origin = middle + cell_n * lift + dir * 0.005;
    ray.dir = dir;
    // Each ray stands for a cone, and for the cell it leaves from.
    velk_ray_cone = kBounceConeTan;
    velk_ray_footprint = cell_size * 0.5;
    RayHit hit;
    bool found = trace_closest_hit(ray, 1e30, hit);
    // The back of the cell's own object, close by: the ray started inside the surface rays
    // see, as on a curved object drawn with flat triangles, so it goes on from there.
    uint own_object = velk_light_cell_words.data[cell + 1u];
    if (found && hit.object == own_object && hit.t < cell_size && dot(hit.normal, dir) > 0.0) {
        velk_cell_stat(kStatBounceSelfHits, 1u);
        ray.origin += dir * (hit.t + 0.01 * cell_size);
        found = trace_closest_hit(ray, 1e30, hit);
    }
    velk_ray_cone = 0.0;
    velk_ray_footprint = 0.0;
    velk_light_cell_words.data[ray_at + 2u] = 0u;
    velk_light_cell_words.data[ray_at + 3u] = 0u;
    velk_light_cell_words.data[ray_at + 4u] = kNoCell;
    if (!found) {
        velk_light_cell_words.data[ray_at] = kNoCell;
        velk_light_cell_words.data[ray_at + 1u] = 0u;
        velk_light_cell_words.data[scratch] = kNoCell;
        return;
    }

    // The back of a surface is the other side of it, which has cells of its own.
    vec3 hit_n = normalize(hit.normal);
    uint hit_side = hit.side;
    if (dot(hit_n, dir) > 0.0) {
        hit_n = -hit_n;
        hit_side ^= 1u;
    }
    velk_light_cell_words.data[ray_at] = kBounceHitPending;
    velk_light_cell_words.data[ray_at + 1u] = (packHalf2x16(vec2(min(hit.t, 60000.0), 0.0)) & 0xFFFFu) | (velk_oct8(hit_n) << 16u);
    velk_light_cell_words.data[scratch] = hit.shape_index;
    velk_light_cell_words.data[scratch + 1u] = packHalf2x16(hit.uv);
    velk_light_cell_words.data[scratch + 2u] = hit.object;
    velk_light_cell_words.data[scratch + 3u] = floatBitsToUint(hit.local.x);
    velk_light_cell_words.data[scratch + 4u] = floatBitsToUint(hit.local.y);
    velk_light_cell_words.data[scratch + 5u] = floatBitsToUint(hit.local.z);
    velk_light_cell_words.data[scratch + 6u] = hit_side;
}
)";

/// Finds the cells the rays of the bounce cells listed hit, a group a cell and a ray a thread,
/// from what light_cells_bounce_trace_main_src wrote down, and keeps them as a pixel keeps its
/// own: made if new, and listed for their direct light. Runs before the pass that traces
/// direct light, so the cells it lists are traced in the same frame. Follows light_cells_src
/// and light_cells_touch_src; traces nothing.
[[maybe_unused]] constexpr string_view light_cells_bounce_hits_main_src = R"(
void main()
{
    velk_cells_load();
    uint lane = gl_LocalInvocationIndex;
    uint list = CELLS_BOUNCE_LIST;
    uint n = gl_WorkGroupID.x;
    if (n >= min(velk_light_cell_words.data[list], CELLS_BOUNCE_LIST_MAX)) return;
    uint at = velk_light_cell_words.data[list + 4u + n * 2u];
    uint ray_at = at + kBounceHeader + lane * kBounceRayWords;
    if (velk_light_cell_words.data[ray_at] != kBounceHitPending) return;
    uint scratch = velk_bounce_scratch(n, lane);

    uint cell = velk_light_cell_words.data[at];
    float cell_size = velk_light_cell_size(int(velk_light_cell_words.data[cell + 5u] >> 3u) - 64);
    vec3 cell_n = velk_cell_unpack_normal(velk_light_cell_words.data[at + 13u]);
    vec3 dir = velk_bounce_dir(lane, cell_n, velk_bounce_turn(cell));
    vec3 from = vec3(uintBitsToFloat(velk_light_cell_words.data[at + 10u]),
                     uintBitsToFloat(velk_light_cell_words.data[at + 11u]),
                     uintBitsToFloat(velk_light_cell_words.data[at + 12u])) - VELK_GLOBALS.cam_pos.xyz;
    uint packed = velk_light_cell_words.data[ray_at + 1u];
    float t = unpackHalf2x16(packed).x;

    // The cell hit is about as large as the cone is wide there, and kept as a pixel keeps its
    // own, here from the hit rather than a pixel.
    float width = cell_size + 2.0 * t * kBounceConeTan;
    int level = int(ceil(log2(max(width, 1e-6))));
    cells_from_pixel = false;
    world_pos = from + dir * t;
    N = velk_unoct8(packed >> 16u);
    object = velk_light_cell_words.data[scratch + 2u];
    side = velk_light_cell_words.data[scratch + 6u];
    coord = ivec2(0);
    vec3 local = vec3(uintBitsToFloat(velk_light_cell_words.data[scratch + 3u]),
                      uintBitsToFloat(velk_light_cell_words.data[scratch + 4u]),
                      uintBitsToFloat(velk_light_cell_words.data[scratch + 5u]));
    uint kept = velk_cell_keep(velk_light_cell(object, local, side, level), 0);
    velk_light_cell_words.data[ray_at] = kept != kNoCell ? kept : kBounceHitNoCell;
    // The bounce cell over the hit, kBounceUp levels up as over a pixel, whose light arriving
    // there the gather passes on.
    velk_light_cell_words.data[ray_at + 4u] = velk_cell_keep(velk_light_cell(object, local, side, level + kBounceUp), kBounceUp);
}
)";

/// Marks on every traced light cell the lights this frame's changes could alter there, a
/// thread a slot of the table, from where its object is now: a moved box on the way to a
/// light, or a light that moved. The marks stay on the cell until it is traced for those
/// lights, so a cell read again after a long time knows what changed meanwhile. A cell whose
/// object's place is not known takes every light. Runs only on frames with changes. Follows
/// light_cells_src.
[[maybe_unused]] constexpr string_view light_cells_mark_main_src = R"(
void main()
{
    velk_cells_load();
    if (velk_light_cell_words.data[CELLS_CHANGES + 2u] == 0u) return;
    uint slot = gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex;
    if (slot >= CELLS_SLOTS) return;
    uint at = CELLS_WORD + slot * kCellWords;
    if ((velk_light_cell_words.data[at] & kCellReady) == 0u) return;
    uint object = velk_light_cell_words.data[at + 1u];
    ivec3 cube = ivec3(int(velk_light_cell_words.data[at + 2u]), int(velk_light_cell_words.data[at + 3u]),
                       int(velk_light_cell_words.data[at + 4u]));
    uint level_side = velk_light_cell_words.data[at + 5u];
    int level = int(level_side >> 3u) - 64;
    vec3 place;
    vec3 facing;
    uvec2 which = uvec2(0xFFFFFFFFu);
    if (velk_cell_place(object, cube, level, level_side & 7u, place, facing)) {
        // The place is the cube's middle, up to most of a cell off the surface, so the margin
        // is wider than a pixel's.
        which = velk_cell_changed_lights(place, facing, 2.0 * velk_light_cell_size(level));
    } else {
        velk_cell_stat(kStatMarkUnplaced, 1u);
    }
    // Only lights there are.
    uint lights = min(pc.light_count, kCellLights);
    which &= uvec2(lights >= 32u ? 0xFFFFFFFFu : (1u << lights) - 1u,
                   lights >= 64u ? 0xFFFFFFFFu : lights > 32u ? (1u << (lights - 32u)) - 1u : 0u);
    if ((which.x | which.y) == 0u) return;
    velk_cell_stat(kStatMarked, 1u);
    atomicOr(velk_light_cell_words.data[at + kCellChanged], which.x);
    atomicOr(velk_light_cell_words.data[at + kCellChanged + 1u], which.y);
}
)";

/// Keeps up the bounce cells over the hits of the rays of bounce cells pixels use, a group a
/// cell and a ray a thread, so that the light arriving there from other surfaces is there to
/// pass on: made if absent, starting their rays where this ray hit, noted as used, and listed
/// for tracing. Cells made this way make none over their own hits, which is where bounce ends.
/// Reads the hits as the last trace left them. Runs in two rounds as the touch pass does, so
/// cells with no light yet come first: after the pixels' new cells, the new ones (pc.mode
/// kHopsNew); after the pixels' stale cells, the rest (kHopsRest). Follows light_cells_src;
/// traces nothing.
[[maybe_unused]] constexpr string_view light_cells_bounce_hops_main_src = R"(
const uint kHopsNew = 1u;
const uint kHopsRest = 2u;

void main()
{
    velk_cells_load();
    uint lane = gl_LocalInvocationIndex;
    uint at = CELLS_BOUNCE + gl_WorkGroupID.x * kBounceWords;
    uint cell = velk_light_cell_words.data[at];
    uint now = VELK_GLOBALS.present_counter;
    // Traced entries pixels use; the same for every thread of the group.
    if (cell == 0u || velk_light_cell_words.data[at + 2u] != kBounceTraced
        || (velk_light_cell_words.data[at + 14u] & kBounceFromRay) != 0u
        || now - velk_light_cell_words.data[at + 1u] > 2u) return;
    uint ray_at = at + kBounceHeader + lane * kBounceRayWords;
    uint hit = velk_light_cell_words.data[ray_at];
    uint over = velk_light_cell_words.data[ray_at + 4u];
    // The markers for a miss, no room and not found yet are the top three values.
    if (hit >= kBounceHitPending || over == kNoCell) return;

    // The rest round: noted as used by the first ray to get to it this frame, if a pixel was
    // not first, and listed if something changed on the way of its rays.
    bool made;
    uint list = CELLS_BOUNCE_LIST;
    if (pc.mode == kHopsRest) {
        uint entry = velk_bounce_find(over, false, made);
        if (entry == kNoCell) return;
        if (velk_light_cell_words.data[entry + 1u] == now || atomicExchange(velk_light_cell_words.data[entry + 1u], now) == now) {
            return;
        }
        if (velk_light_cell_words.data[entry + 2u] != kBounceTraced
            || (velk_light_cell_words.data[entry + 14u] & kBounceStale) == 0u) {
            return;
        }
        uint n = atomicAdd(velk_light_cell_words.data[list], 1u);
        // No room: it keeps what it holds.
        if (n >= CELLS_BOUNCE_LIST_MAX) return;
        velk_cell_stat(kStatHopListed, 1u);
        velk_light_cell_words.data[entry + 2u] = kBounceListed;
        velk_light_cell_words.data[list + 4u + n * 2u] = entry;
        velk_light_cell_words.data[list + 4u + n * 2u + 1u] = kCellNoPixel;
        return;
    }

    // The round for new ones: made if absent, and listed if made by a ray and not traced yet.
    // A pixel's own that is not traced yet has no start for its rays and waits for its pixels.
    uint entry = velk_bounce_find(over, true, made);
    if (entry == kNoCell) {
        velk_cell_stat(kStatHopRefused, 1u);
        return;
    }
    if (made) {
        // Its rays start where this one hit, facing the way the surface there faces.
        velk_cell_stat(kStatHopMade, 1u);
        vec3 cell_n = velk_cell_unpack_normal(velk_light_cell_words.data[at + 13u]);
        vec3 dir = velk_bounce_dir(lane, cell_n, velk_bounce_turn(cell));
        vec3 from = vec3(uintBitsToFloat(velk_light_cell_words.data[at + 10u]),
                         uintBitsToFloat(velk_light_cell_words.data[at + 11u]),
                         uintBitsToFloat(velk_light_cell_words.data[at + 12u]));
        uint packed = velk_light_cell_words.data[ray_at + 1u];
        vec3 start = from + dir * unpackHalf2x16(packed).x;
        velk_light_cell_words.data[entry + 10u] = floatBitsToUint(start.x);
        velk_light_cell_words.data[entry + 11u] = floatBitsToUint(start.y);
        velk_light_cell_words.data[entry + 12u] = floatBitsToUint(start.z);
        velk_light_cell_words.data[entry + 13u] = velk_cell_pack_normal(velk_unoct8(packed >> 16u));
        // Listed before it is marked as made by a ray, so no other ray lists it as well.
        velk_light_cell_words.data[entry + 2u] = kBounceListed;
        velk_light_cell_words.data[entry + 14u] = kBounceFromRay;
    } else {
        // Only one ray moves it from untraced to listed.
        if ((velk_light_cell_words.data[entry + 14u] & kBounceFromRay) == 0u
            || atomicCompSwap(velk_light_cell_words.data[entry + 2u], kBounceUntraced, kBounceListed) != kBounceUntraced) {
            return;
        }
    }
    uint n = atomicAdd(velk_light_cell_words.data[list], 1u);
    if (n >= CELLS_BOUNCE_LIST_MAX) {
        // No room: it waits for a later frame.
        velk_light_cell_words.data[entry + 2u] = kBounceUntraced;
        return;
    }
    velk_cell_stat(kStatHopListed, 1u);
    velk_light_cell_words.data[list + 4u + n * 2u] = entry;
    velk_light_cell_words.data[list + 4u + n * 2u + 1u] = kCellNoPixel;
}
)";

/// Gathers what each bounce cell's rays see, a group a cell and a ray a thread, into the
/// cell's SH: for a hit, the light the cell hit holds as arriving there times the albedo
/// there, plus what the surface there gives off; for a miss, the sky as wide as the ray's
/// cone. Rays whose hit has no light yet are left out. Follows light_cells_src.
[[maybe_unused]] constexpr string_view light_cells_bounce_gather_main_src = R"(
shared vec4 s_bounce_radiance[64];
shared vec3 s_bounce_dir[64];

void main()
{
    velk_cells_load();
    uint lane = gl_LocalInvocationIndex;
    uint at = CELLS_BOUNCE + gl_WorkGroupID.x * kBounceWords;
    uint cell = velk_light_cell_words.data[at];
    uint now = VELK_GLOBALS.present_counter;
    // Entries traced and in use; the same for every thread of the group.
    if (cell == 0u || velk_light_cell_words.data[at + 2u] != kBounceTraced
        || now - velk_light_cell_words.data[at + 1u] > 2u) return;

    vec3 cell_n = velk_cell_unpack_normal(velk_light_cell_words.data[at + 13u]);
    vec3 dir = velk_bounce_dir(lane, cell_n, velk_bounce_turn(cell));
    uint ray_at = at + kBounceHeader + lane * kBounceRayWords;
    uint hit = velk_light_cell_words.data[ray_at];
    vec3 radiance = vec3(0.0);
    float known = 1.0;
    if (hit == kNoCell) {
        float env_width = pc.env_texture_id != 0u ? float(textureSize(velk_textures[nonuniformEXT(pc.env_texture_id)], 0).x) : 1.0;
        radiance = env_miss_color_lod(dir, max(log2(env_width * kBounceConeTan / 3.14159265), 0.0));
    } else if (hit == kBounceHitNoCell || hit == kBounceHitPending) {
        known = 0.0;
    } else {
        uint b = velk_light_cell_words.data[hit + kCellArrive + 1u];
        if ((b & 0xFFFFu) == kArriveMade || (b >> 16u) == 0u) {
            known = 0.0;
        } else {
            vec3 arriving = vec3(unpackHalf2x16(velk_light_cell_words.data[hit + kCellArrive]), unpackHalf2x16(b).x);
            // Light from other surfaces arriving there too, from the bounce entry over the hit
            // if it has any; it holds one bounce more each frame.
            uint over = velk_light_cell_words.data[ray_at + 4u];
            if (over != kNoCell && pc.bounce_hops >= 2u) {
                bool made;
                uint entry = velk_bounce_find(over, false, made);
                if (entry != kNoCell && velk_light_cell_words.data[entry + 2u] == kBounceTraced) {
                    uvec3 a = uvec3(velk_light_cell_words.data[entry + 4u], velk_light_cell_words.data[entry + 5u],
                                    velk_light_cell_words.data[entry + 6u]);
                    uvec3 c = uvec3(velk_light_cell_words.data[entry + 7u], velk_light_cell_words.data[entry + 8u],
                                    velk_light_cell_words.data[entry + 9u]);
                    arriving += velk_bounce_sh_irradiance(a, c, velk_unoct8(velk_light_cell_words.data[ray_at + 1u] >> 16u));
                }
            }
            radiance = unpackUnorm4x8(velk_light_cell_words.data[ray_at + 2u]).rgb * arriving
                     + velk_unpack_rgbe(velk_light_cell_words.data[ray_at + 3u]);
        }
    }
    s_bounce_radiance[lane] = vec4(min(radiance, vec3(kBounceBrightest)), known);
    s_bounce_dir[lane] = dir;
    barrier();
    if (lane != 0u) return;

    // SH L1 of radiance. Each ray stands for the same share of
    // the hemisphere; those left out are left out of the shares too.
    const float kY0 = 0.282095;
    const float kY1 = 0.488603;
    vec3 sh[4] = vec3[4](vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));
    float rays = 0.0;
    for (uint r = 0u; r < kBounceRays; ++r) {
        vec4 s = s_bounce_radiance[r];
        if (s.w <= 0.0) continue;
        vec3 d = s_bounce_dir[r];
        rays += 1.0;
        sh[0] += s.rgb * kY0;
        sh[1] += s.rgb * (kY1 * d.y);
        sh[2] += s.rgb * (kY1 * d.z);
        sh[3] += s.rgb * (kY1 * d.x);
    }
    velk_cell_stat(kStatBounceGathered, 1u);
    if ((velk_light_cell_words.data[at + 14u] & kBounceFromRay) != 0u) velk_cell_stat(kStatHopGathered, 1u);
    velk_cell_stat(kStatBounceRaysUnknown, kBounceRays - uint(rays));
    if (rays <= 0.0) {
        velk_cell_stat(kStatBounceAllUnknown, 1u);
        return;
    }
    float w = 2.0 * 3.14159265 / rays;
    for (int c = 0; c < 4; ++c) sh[c] *= w;
    velk_light_cell_words.data[at + 4u] = packHalf2x16(sh[0].rg);
    velk_light_cell_words.data[at + 5u] = packHalf2x16(vec2(sh[0].b, sh[1].r));
    velk_light_cell_words.data[at + 6u] = packHalf2x16(sh[1].gb);
    velk_light_cell_words.data[at + 7u] = packHalf2x16(sh[2].rg);
    velk_light_cell_words.data[at + 8u] = packHalf2x16(vec2(sh[2].b, sh[3].r));
    velk_light_cell_words.data[at + 9u] = packHalf2x16(sh[3].gb);
}
)";

/// Marks the bounce cells this frame's changes could alter, a group a cell and a ray a
/// thread: a change on the stretch to what a ray hit, or a long way for a miss, which
/// something may now be in the way of. The boxes of a thing that moved, before and after, are
/// both listed, so a ray whose hit moved away counts too. Every traced entry is looked at,
/// used now or not, so the mark is there whenever a pixel uses it again: the cell is traced
/// again then, and read as it is until then. Follows light_cells_src.
[[maybe_unused]] constexpr string_view light_cells_bounce_changes_main_src = R"(
shared bool s_bounce_crossed[64];

void main()
{
    velk_cells_load();
    if (velk_light_cell_words.data[CELLS_CHANGES + 2u] == 0u) return;
    uint lane = gl_LocalInvocationIndex;
    uint at = CELLS_BOUNCE + gl_WorkGroupID.x * kBounceWords;
    uint cell = velk_light_cell_words.data[at];
    // The same for every thread of the group.
    if (cell == 0u || velk_light_cell_words.data[at + 2u] != kBounceTraced
        || (velk_light_cell_words.data[at + 14u] & kBounceStale) != 0u) return;

    bool crossed = velk_light_cell_words.data[CELLS_CHANGES + 1u] != 0u;
    if (!crossed) {
        vec3 cell_n = velk_cell_unpack_normal(velk_light_cell_words.data[at + 13u]);
        vec3 dir = velk_bounce_dir(lane, cell_n, velk_bounce_turn(cell));
        uint ray_at = at + kBounceHeader + lane * kBounceRayWords;
        vec3 cam = VELK_GLOBALS.cam_pos.xyz;
        vec3 from = vec3(uintBitsToFloat(velk_light_cell_words.data[at + 10u]),
                         uintBitsToFloat(velk_light_cell_words.data[at + 11u]),
                         uintBitsToFloat(velk_light_cell_words.data[at + 12u])) - cam;
        float reach = velk_light_cell_words.data[ray_at] == kNoCell ? kBounceMissReach
                                                                   : unpackHalf2x16(velk_light_cell_words.data[ray_at + 1u]).x;
        vec3 to = from + dir * reach;
        uint changes = velk_light_cell_words.data[CELLS_CHANGES];
        for (uint c = 0u; c < changes && !crossed; ++c) {
            uint change = CELLS_CHANGES + 4u + c * 8u;
            // A moved light changes what the hits hold, which the gather reads anew each frame.
            if (velk_light_cell_words.data[change + 7u] != 0xFFFFFFFFu) continue;
            vec3 box_min = vec3(uintBitsToFloat(velk_light_cell_words.data[change]),
                                uintBitsToFloat(velk_light_cell_words.data[change + 1u]),
                                uintBitsToFloat(velk_light_cell_words.data[change + 2u])) - cam;
            vec3 box_max = vec3(uintBitsToFloat(velk_light_cell_words.data[change + 4u]),
                                uintBitsToFloat(velk_light_cell_words.data[change + 5u]),
                                uintBitsToFloat(velk_light_cell_words.data[change + 6u])) - cam;
            crossed = velk_segment_touches(from, to, box_min, box_max);
        }
    }
    s_bounce_crossed[lane] = crossed;
    barrier();
    if (lane != 0u) return;
    bool any_crossed = false;
    for (uint r = 0u; r < kBounceRays; ++r) any_crossed = any_crossed || s_bounce_crossed[r];
    if (any_crossed) velk_light_cell_words.data[at + 14u] |= kBounceStale;
}
)";

/// Brings the light arriving at the cells bounce rays hit, for those no pixel reads: an eighth
/// of each bounce cell's rays a frame add a sample where they hit to their cell's mean (see
/// velk_cell_arrive). A group takes eight bounce cells and a thread one ray of one of them.
/// Follows light_cells_src and light_cells_touch_src; traces nothing.
[[maybe_unused]] constexpr string_view light_cells_bounce_arrive_main_src = R"(
void main()
{
    velk_cells_load();
    uint lane = gl_LocalInvocationIndex;
    uint entry = gl_WorkGroupID.x * 8u + lane / 8u;
    if (entry >= CELLS_BOUNCE_ENTRIES) return;
    uint at = CELLS_BOUNCE + entry * kBounceWords;
    uint cell = velk_light_cell_words.data[at];
    uint now = VELK_GLOBALS.present_counter;
    if (cell == 0u || velk_light_cell_words.data[at + 2u] != kBounceTraced
        || now - velk_light_cell_words.data[at + 1u] > 2u) return;
    uint ray = (lane % 8u) * 8u + (now & 7u);
    uint ray_at = at + kBounceHeader + ray * kBounceRayWords;
    uint hit = velk_light_cell_words.data[ray_at];
    // The markers for a miss, no room and not found yet are the top three values.
    if (hit >= kBounceHitPending) return;
    vec3 cell_n = velk_cell_unpack_normal(velk_light_cell_words.data[at + 13u]);
    vec3 dir = velk_bounce_dir(ray, cell_n, velk_bounce_turn(cell));
    vec3 from = vec3(uintBitsToFloat(velk_light_cell_words.data[at + 10u]),
                     uintBitsToFloat(velk_light_cell_words.data[at + 11u]),
                     uintBitsToFloat(velk_light_cell_words.data[at + 12u])) - VELK_GLOBALS.cam_pos.xyz;
    uint packed = velk_light_cell_words.data[ray_at + 1u];
    velk_cell_arrive(hit, from + dir * unpackHalf2x16(packed).x, velk_unoct8(packed >> 16u));
}
)";

/// Finds the material where the bounce cells listed this frame hit, a group a cell and a ray
/// a thread: the albedo, and what the surface gives off. Composed with the materials by
/// compose_rt_compute. `pc.extras.x` is the word of the list of bounce cells and `y` the most
/// it lists (CELLS_BOUNCE_LIST and CELLS_BOUNCE_LIST_MAX).
[[maybe_unused]] constexpr string_view light_cells_bounce_shade_main_src = VELK_BOUNCE_GLSL R"(
layout(set = 1, binding = 16, std430) buffer BounceWords { uint data[]; } bounce_words;

void main()
{
    uint lane = gl_LocalInvocationIndex;
    uint list = pc.extras.x;
    uint most = pc.extras.y;
    uint n = gl_WorkGroupID.x;
    if (n >= min(bounce_words.data[list], most)) return;
    uint at = bounce_words.data[list + 4u + n * 2u];
    if (bounce_words.data[at + 2u] != kBounceTraced) return;
    // See velk_bounce_scratch.
    uint scratch = list + 4u + most * 2u + (n * kBounceRays + lane) * 8u;
    uint shape_index = bounce_words.data[scratch];
    if (shape_index == 0xFFFFFFFFu) return;
    uint ray_at = at + kBounceHeader + lane * kBounceRayWords;

    vec3 middle = vec3(uintBitsToFloat(bounce_words.data[at + 10u]), uintBitsToFloat(bounce_words.data[at + 11u]),
                       uintBitsToFloat(bounce_words.data[at + 12u])) - VELK_GLOBALS.cam_pos.xyz;
    vec3 cell_n = velk_unoct(unpackSnorm2x16(bounce_words.data[at + 13u]));
    vec3 dir = velk_bounce_dir(lane, cell_n, velk_bounce_turn(bounce_words.data[at]));
    uint packed = bounce_words.data[ray_at + 1u];
    float t = unpackHalf2x16(packed).x;

    RtShape s = velk_bvh_shapes.data[shape_index];
    EvalContext ectx;
    ectx.material_base = s.material_base;
    ectx.texture_id = s.texture_id;
    ectx.shape_param = s.shape_param;
    ectx.uv = unpackHalf2x16(bounce_words.data[scratch + 1u]);
    ectx.uv1 = ectx.uv;
    ectx.base = s.color;
    ectx.ray_dir = dir;
    ectx.normal = velk_unoct8(packed >> 16u);
    ectx.rel_pos = middle + dir * t;
    ectx.hit_pos = ectx.rel_pos + VELK_GLOBALS.cam_pos.xyz;
    ectx.tangent = vec4(0.0);
    MaterialEval e = velk_eval_material(s.material_id, ectx);
    bool lit = e.lighting_mode == VELK_LIGHTING_STANDARD;
    vec3 albedo = lit ? e.color.rgb * (1.0 - clamp(e.metallic, 0.0, 1.0)) : vec3(0.0);
    vec3 emission = e.emissive + (lit ? vec3(0.0) : e.color.rgb);
    bounce_words.data[ray_at + 2u] = packUnorm4x8(vec4(clamp(albedo, 0.0, 1.0), 1.0));
    bounce_words.data[ray_at + 3u] = velk_pack_rgbe(emission);
}
)";

/// Reads the light cells. Follows deferred_compute_prelude_src.
/// A pass calls velk_cells_begin for a pixel and then velk_cells_light_through for each
/// light. It writes nothing and traces nothing: light_cells_update_main_src keeps the cells
/// up. See light_cells_src for what a cell holds and for CELLS_HEADER.
[[maybe_unused]] constexpr string_view light_cells_read_src = R"(
layout(set = 0, binding = 0) uniform usampler2D velk_textures_uint[];
layout(set = 1, binding = 16, std430) readonly buffer VelkLightCellWords { uint data[]; } velk_light_cell_words;
)" VELK_LIGHT_CELL_GLSL VELK_BOUNCE_GLSL R"(
#define CELLS_HEADER pc.cells_header
// The header's words this shader uses, read once a pixel by velk_cells_begin.
uint cells_word;
uint cells_slots;
uint cells_pixels;
uint cells_ids;
uint cells_surface_id;
#define CELLS_WORD cells_word
#define CELLS_SLOTS cells_slots
#define CELLS_PIXELS cells_pixels
#define CELLS_IDS cells_ids
#define CELLS_SURFACE cells_surface_id

const uint kCellWords = 32u;
const uint kCellArrive = 24u;
const int kCellUp = 3;
const int kAround = 18;
const uint kCellReady = 1u;
const uint kCellProbes = 8u;
const uint kCellLights = 64u;
const uint kNoCell = 0xFFFFFFFFu;

// The bounce entry of traced cell `cell`, or kNoCell. See velk_bounce_find.
uint velk_bounce_lookup(uint cell)
{
    uint bounce = velk_light_cell_words.data[CELLS_HEADER + 8u];
    uint entries = velk_light_cell_words.data[CELLS_HEADER + 9u];
    uint tag = velk_light_cell_words.data[cell] & 0xFFFFFFFCu;
    uint hash = velk_light_cell_mix(cell * 2654435761u);
    for (uint p = 0u; p < kBounceProbes; ++p) {
        uint at = bounce + ((hash + p) & (entries - 1u)) * kBounceWords;
        if (velk_light_cell_words.data[at] == cell && velk_light_cell_words.data[at + 15u] == tag) return at;
    }
    return kNoCell;
}

// The light from other surfaces arriving at a surface facing `N`, from bounce entry `at`, as
// irradiance over pi. See velk_bounce_sh_irradiance.
vec3 velk_bounce_irradiance(uint at, vec3 N)
{
    uvec3 a = uvec3(velk_light_cell_words.data[at + 4u], velk_light_cell_words.data[at + 5u], velk_light_cell_words.data[at + 6u]);
    uvec3 b = uvec3(velk_light_cell_words.data[at + 7u], velk_light_cell_words.data[at + 8u], velk_light_cell_words.data[at + 9u]);
    return velk_bounce_sh_irradiance(a, b, N);
}

// The word a traced cell starts at, or kNoCell.
uint velk_cell_lookup(VelkLightCell c)
{
    uint hash = velk_light_cell_hash(c);
    uint tag = (hash & 0xFFFFFFF0u) | 8u;
    for (uint p = 0u; p < kCellProbes; ++p) {
        uint at = CELLS_WORD + ((hash + p) & (CELLS_SLOTS - 1u)) * kCellWords;
        uint state = velk_light_cell_words.data[at];
        if (state == 0u) return kNoCell;
        if ((state & 0xFFFFFFFCu) != tag || (state & kCellReady) == 0u) continue;
        if (velk_light_cell_words.data[at + 1u] == c.object
            && velk_light_cell_words.data[at + 2u] == uint(c.at.x)
            && velk_light_cell_words.data[at + 3u] == uint(c.at.y)
            && velk_light_cell_words.data[at + 4u] == uint(c.at.z)
            && velk_light_cell_words.data[at + 5u] == (uint(c.level + 64) * 8u + c.side)) {
            return at;
        }
    }
    return kNoCell;
}

// What a cell holds for light `i`: in the low 4 bits how much gets through, in fifteenths,
// and in the high 4 how many half levels above its own suit the soft edge there.
uint velk_cell_light(uint at, uint i)
{
    return (velk_light_cell_words.data[at + 8u + (i >> 2u)] >> ((i & 3u) * 8u)) & 255u;
}

// The pixel taken up by velk_cells_begin.
ivec2 coord;
vec3 world_pos;
vec3 N;
uint cells_object;
vec3 cells_surface;
uint cells_side;
int cells_level;
// The level of its own cell, whether or not that is traced.
int cells_own_level;
// Its own cell if traced, or kNoCell.
uint cells_own = 0xFFFFFFFFu;
// The traced cells around it at its own level, kNoCell for the rest, and their weights in all.
uint cells_around[kAround];
float cells_total = 0.0;

// A reader blends the cells around a point: three by three along the two object axes the
// surface lies in, weighted by a rounded curve so that no grid shows and an edge comes out
// about two cells wide, and the nearest two along the third axis. This is where those are
// at one level, and the weights' parts.
struct VelkCellBlend {
    int level;
    ivec3 nearest;
    int along_base;
    vec3 curve_u;
    vec3 curve_v;
    float along_f;
};

VelkCellBlend velk_cell_blend(int level)
{
    uint axis = cells_side >> 1u;
    uint ua = (axis + 1u) % 3u;
    uint va = (axis + 2u) % 3u;
    vec3 g = cells_surface / velk_light_cell_size(level) - 0.5;
    VelkCellBlend b;
    b.level = level;
    b.nearest = ivec3(floor(g + 0.5));
    vec3 t = g - vec3(b.nearest);
    b.curve_u = vec3(0.5 * (0.5 - t[ua]) * (0.5 - t[ua]), 0.75 - t[ua] * t[ua], 0.5 * (0.5 + t[ua]) * (0.5 + t[ua]));
    b.curve_v = vec3(0.5 * (0.5 - t[va]) * (0.5 - t[va]), 0.75 - t[va] * t[va], 0.5 * (0.5 + t[va]) * (0.5 + t[va]));
    b.along_base = int(floor(g[axis]));
    b.along_f = g[axis] - float(b.along_base);
    return b;
}

// The `k`th of the cells a blend covers, if traced, and its weight.
uint velk_cell_blend_at(VelkCellBlend b, int k, out float weight)
{
    uint axis = cells_side >> 1u;
    uint ua = (axis + 1u) % 3u;
    uint va = (axis + 2u) % 3u;
    int iu = k % 3;
    int iv = (k / 3) % 3;
    int ia = k / 9;
    weight = b.curve_u[iu] * b.curve_v[iv] * (ia == 0 ? 1.0 - b.along_f : b.along_f);
    VelkLightCell c;
    c.object = cells_object;
    c.level = b.level;
    c.side = cells_side;
    c.at = ivec3(0);
    c.at[ua] = b.nearest[ua] + iu - 1;
    c.at[va] = b.nearest[va] + iv - 1;
    c.at[axis] = b.along_base + ia;
    return velk_cell_lookup(c);
}

VelkCellBlend cells_blend;

// Takes up the pixel at `pixel`, whose surface is at `rel_pos` from the camera with the
// G-buffer's normal and error `normal_error`. False if there is no surface there.
bool velk_cells_begin(ivec2 pixel, vec3 rel_pos, vec4 normal_error)
{
    coord = pixel;
    world_pos = rel_pos;
    cells_own = kNoCell;
    cells_total = 0.0;
    cells_word = velk_light_cell_words.data[CELLS_HEADER];
    cells_slots = velk_light_cell_words.data[CELLS_HEADER + 1u];
    cells_pixels = velk_light_cell_words.data[CELLS_HEADER + 2u];
    cells_ids = velk_light_cell_words.data[CELLS_HEADER + 4u];
    cells_surface_id = velk_light_cell_words.data[CELLS_HEADER + 5u];
    uvec2 ids = texelFetch(velk_textures_uint[nonuniformEXT(CELLS_IDS)], pixel, 0).xy;
    if (ids.x == 0u || dot(normal_error.xyz, normal_error.xyz) < 1e-6) return false;
    N = normalize(normal_error.xyz);
    VelkDrawData d = velk_draw_data.data[ids.x - 1u];
    uint position = ids.y >> d.triangle_bits;
    uint slot = d.visible_base == VELK_NO_VISIBLE_LIST ? position : velk_culling.data[d.visible_base + position];
    cells_object = velk_object_id(d.instances_base + slot);
    vec4 surface = texelFetch(velk_textures[nonuniformEXT(CELLS_SURFACE)], pixel, 0);
    cells_surface = surface.xyz;
    cells_side = uint(surface.w + 0.5);

    mat4 vp = VELK_GLOBALS.view_projection;
    float pixel_width = 2.0 / (length(vec3(vp[0][1], vp[1][1], vp[2][1])) * float(pc.height));
    cells_level = velk_light_cell_level(length(rel_pos), pixel_width, float(CELLS_PIXELS));
    cells_own_level = cells_level;
    cells_own = velk_cell_lookup(velk_light_cell(cells_object, cells_surface, cells_side, cells_level));

    // With no traced cell around at its own level yet, the pixel reads the cells kCellUp
    // levels larger instead, which light_cells_touch_src keeps up meanwhile.
    for (int pass = 0; pass < 2 && cells_total <= 0.0; ++pass) {
        if (pass == 1) cells_level += kCellUp;
        cells_blend = velk_cell_blend(cells_level);
        for (int k = 0; k < kAround; ++k) {
            float weight;
            uint at = velk_cell_blend_at(cells_blend, k, weight);
            cells_around[k] = at;
            if (at != kNoCell) cells_total += weight;
        }
    }
    return true;
}

// How much of light `i` gets through from the cells of `level` around the pixel, looked up
// as needed. Negative if none of them is traced.
float velk_cells_level_through(uint i, int level)
{
    VelkCellBlend b = velk_cell_blend(level);
    float sum = 0.0;
    float total = 0.0;
    for (int k = 0; k < kAround; ++k) {
        float weight;
        uint at = velk_cell_blend_at(b, k, weight);
        if (at == kNoCell) continue;
        sum += weight * float(velk_cell_light(at, i) & 15u) / 15.0;
        total += weight;
    }
    return total > 0.0 ? sum / total : -1.0;
}

// How much of light `i` gets through at the pixel taken up by velk_cells_begin, 0 to 1. A
// light past those a cell holds, or with no traced cell around yet, counts as unshadowed.
float velk_cells_light_through(uint i)
{
    if (cells_total <= 0.0 || i >= kCellLights) return 1.0;
    // From the cells of the pixel's own level: how much gets through, and how wide the soft
    // edge is, as levels above this one, going by the cells that have any of it.
    float own_through = 0.0;
    float edge_sum = 0.0;
    float edge_weight = 0.0;
    for (int k = 0; k < kAround; ++k) {
        uint at = cells_around[k];
        if (at == kNoCell) continue;
        int iu = k % 3;
        int iv = (k / 3) % 3;
        float weight = cells_blend.curve_u[iu] * cells_blend.curve_v[iv]
                     * (k < 9 ? 1.0 - cells_blend.along_f : cells_blend.along_f);
        uint held = velk_cell_light(at, i);
        own_through += weight * float(held & 15u) / 15.0;
        if ((held & 15u) != 15u) {
            edge_sum += weight * float((held >> 4u) & 7u) * 0.5;
            edge_weight += weight;
        }
    }
    own_through /= cells_total;
    float wanted = edge_weight > 0.0 ? min(edge_sum / edge_weight, float(kCellUp)) : 0.0;
    if (wanted <= 0.0) return own_through;
    // A wide soft edge is read from larger cells: the two levels either side of the one it
    // wants, blended, so that the choice of level does not show as a line. A level whose
    // cells are not traced yet gives way to the one below it.
    int below = int(floor(wanted));
    float lower = below == 0 ? own_through : velk_cells_level_through(i, cells_level + below);
    if (lower < 0.0) lower = own_through;
    float upper = lower;
    if (wanted > float(below) && below + 1 <= kCellUp) {
        upper = velk_cells_level_through(i, cells_level + below + 1);
        if (upper < 0.0) upper = lower;
    }
    return mix(lower, upper, wanted - float(below));
}

// The light from other surfaces at the pixel taken up by velk_cells_begin, blended from the
// bounce cells around it as direct light is from its own cells: with `along` false, as
// irradiance over pi on a surface facing `d`; with it
// true, arriving along `d`. Negative if none has any yet.
vec3 velk_cells_bounce_toward(vec3 d, bool along)
{
    VelkCellBlend b = velk_cell_blend(cells_own_level + kBounceUp);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int k = 0; k < kAround; ++k) {
        float weight;
        uint cell = velk_cell_blend_at(b, k, weight);
        if (cell == kNoCell) continue;
        uint at = velk_bounce_lookup(cell);
        if (at == kNoCell || velk_light_cell_words.data[at + 2u] != kBounceTraced) continue;
        // A cell's rays cover the hemisphere about its own normal. On a curved surface the
        // pixel may face well away from that, toward directions the cell has nothing for, so
        // it reads at most 45 degrees off the cell's normal.
        vec3 cell_n = velk_unoct(unpackSnorm2x16(velk_light_cell_words.data[at + 13u]));
        float off = dot(d, cell_n);
        vec3 read_d = d;
        if (off < 0.70710678) {
            vec3 across = d - cell_n * off;
            float across_length = length(across);
            vec3 side_dir = across_length > 1e-4 ? across / across_length : vec3(0.0);
            read_d = normalize(cell_n * 0.70710678 + side_dir * 0.70710678);
        }
        uvec3 sa = uvec3(velk_light_cell_words.data[at + 4u], velk_light_cell_words.data[at + 5u], velk_light_cell_words.data[at + 6u]);
        uvec3 sb = uvec3(velk_light_cell_words.data[at + 7u], velk_light_cell_words.data[at + 8u], velk_light_cell_words.data[at + 9u]);
        sum += weight * (along ? velk_bounce_sh_radiance(sa, sb, read_d) : velk_bounce_sh_irradiance(sa, sb, read_d));
        total += weight;
    }
    return total > 0.0 ? sum / total : vec3(-1.0);
}

// Irradiance on a surface facing `normal`. See velk_cells_bounce_toward.
vec3 velk_cells_bounce(vec3 normal)
{
    return velk_cells_bounce_toward(normal, false);
}
)";

/// Shows the light cells' light in place of the image. Follows deferred_compute_prelude_src
/// and light_cells_read_src. Mode 4: direct diffuse light, each light's own falloff and
/// angle at the pixel times what the cells say gets through. Mode 5: green where the
/// pixel's own cell is traced, red where not. Mode 6: the direct light arriving at the
/// pixel's own cell as the cell holds it, one value a cell, times the surface colour;
/// magenta where it was not worked out in the last few frames. Mode 7: the light from other
/// surfaces its bounce cell holds, for the pixel's normal, without the surface colour;
/// magenta where it has none. Mode 8: the same as
/// pixels read it, blended from the bounce cells around them (velk_cells_bounce).
[[maybe_unused]] constexpr string_view light_cells_main_src = R"(
#define CELLS_MODE pc.mode

void main()
{
    ivec2 at_pixel = ivec2(gl_GlobalInvocationID.xy);
    if (at_pixel.x >= int(pc.width) || at_pixel.y >= int(pc.height)) return;

    vec3 rel_pos = texelFetch(velk_textures[nonuniformEXT(pc.worldpos_tex_id)], at_pixel, 0).xyz;
    vec4 normal_error = texelFetch(velk_textures[nonuniformEXT(pc.normal_tex_id)], at_pixel, 0);
    if (!velk_cells_begin(at_pixel, rel_pos, normal_error)) {
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], at_pixel, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }

    if (CELLS_MODE == 5u) {
        vec3 state_color = vec3(1.0, 0.0, 0.0);
        if (cells_own != kNoCell) {
            state_color = (velk_light_cell_words.data[cells_own] & kCellReady) != 0u ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 1.0, 0.0);
        }
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord,
                   vec4(state_color * (0.35 + 0.65 * abs(dot(N, normalize(-world_pos)))), 1.0));
        return;
    }

    vec4 albedo = texelFetch(velk_textures[nonuniformEXT(pc.albedo_tex_id)], coord, 0);
    float metallic = clamp(texelFetch(velk_textures[nonuniformEXT(pc.material_tex_id)], coord, 0).r, 0.0, 1.0);

    if (CELLS_MODE == 8u) {
        vec3 bounce = velk_cells_bounce(N);
        if (bounce.r < 0.0) bounce = vec3(1.0, 0.0, 1.0);
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(bounce, 1.0));
        return;
    }

    if (CELLS_MODE == 7u) {
        vec3 bounce = vec3(1.0, 0.0, 1.0);
        uint cell = velk_cell_lookup(velk_light_cell(cells_object, cells_surface, cells_side, cells_own_level + kBounceUp));
        uint at = cell != kNoCell ? velk_bounce_lookup(cell) : kNoCell;
        if (at != kNoCell && velk_light_cell_words.data[at + 2u] == kBounceTraced) {
            bounce = velk_bounce_irradiance(at, N);
        }
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(bounce, 1.0));
        return;
    }

    if (CELLS_MODE == 6u) {
        vec3 arriving = vec3(1.0, 0.0, 1.0);
        if (cells_own != kNoCell) {
            uint rg = velk_light_cell_words.data[cells_own + kCellArrive];
            uint b = velk_light_cell_words.data[cells_own + kCellArrive + 1u];
            uint age = ((VELK_GLOBALS.present_counter & 0xFFFFu) - (b >> 16u)) & 0xFFFFu;
            // 0xFFFF in place of blue marks a cell with no light of its own yet.
            if (age <= 4u && (b & 0xFFFFu) != 0xFFFFu) {
                arriving = vec3(unpackHalf2x16(rg), unpackHalf2x16(b).x) * albedo.rgb * (1.0 - metallic);
            }
        }
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(arriving, 1.0));
        return;
    }

    vec3 direct = vec3(0.0);
    for (uint i = 0u; i < pc.light_count; ++i) {
        Light light = velk_light_at(pc.lights_base + i);
        float through = velk_cells_light_through(i);
        if (light.flags.x == 3u) {
            float area = velk_area_diffuse(light.position.xyz, light.direction.xyz, light.params.w, world_pos, N);
            direct += area * light.color_intensity.rgb * light.color_intensity.a * through;
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
            float t = clamp(1.0 - dist / max(light.params.x, 1e-6), 0.0, 1.0);
            atten = t * t;
            if (light.flags.x == 2u) {
                atten *= smoothstep(light.params.z, light.params.y, dot(-L, light.direction.xyz));
            }
        }
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0 || atten <= 0.0) continue;

        direct += NdotL * light.color_intensity.rgb * light.color_intensity.a * atten * through;
    }
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord,
               vec4(albedo.rgb * (1.0 - metallic) * direct, 1.0));
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
    // under a different entry point (the bounce cells' material pass) supply their own main.
    src += main_src.empty() ? rt_compute_main_src : main_src;
    return src;
}

} // namespace velk

#endif // VELK_RENDER_DEFAULT_SHADERS_H
