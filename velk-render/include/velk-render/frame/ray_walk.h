#ifndef VELK_RENDER_FRAME_RAY_WALK_H
#define VELK_RENDER_FRAME_RAY_WALK_H

#include <velk/string_view.h>

namespace velk {

/**
 * The walk behind every ray, registered as the "velk_trace.glsl" include. Rays see what a view
 * draws: its kept cuts, through the top tree over their partitions (FrameGlobals::ray_top,
 * kRayTopHeaderWords) and each partition's tree over its records (kCutNodeWords). A record is
 * tested in its instance's own space: a cluster through its triangle tree (its ray header), a
 * deforming mesh's cluster through its triangles as drawn, a whole mesh through its own tree,
 * and an item rays see that its own pass draws through its visual's analytic shape. Nothing in
 * the walk chooses a level of detail: the cut has.
 *
 * The including shader declares velk_cluster_words, velk_mesh_nodes, velk_mesh_static_records
 * and, through velk-ui.glsl, velk_instances, defines VELK_GLOBALS and VELK_TRACE_STACK_SIZE, and
 * includes velk_intersect.glsl before it. Rays are relative to the camera, as positions are.
 */
inline constexpr string_view velk_trace_src = R"(
// The reciprocal of a ray's direction, for ray_aabb, worked out once per ray. No component is
// exactly zero: a ray parallel to a slab and starting on its plane would make 0 times infinity,
// and the box would count as missed. Rays from a grid of points toward a light without size do
// run so, along whole lines of points.
vec3 velk_ray_inv_dir(vec3 dir)
{
    return 1.0 / mix(dir, sign(dir + vec3(1e-30)) * 1e-12, lessThan(abs(dir), vec3(1e-12)));
}

// Ray-vs-AABB slab test for a ray from `origin` whose direction's reciprocal is `inv_d`
// (velk_ray_inv_dir). Returns true if it meets the box within [0, t_max] and writes t_near
// (clamped to >= 0) to t_hit.
bool ray_aabb(vec3 origin, vec3 inv_d, vec3 bmin, vec3 bmax, float t_max, out float t_hit)
{
    vec3 t0 = (bmin - origin) * inv_d;
    vec3 t1 = (bmax - origin) * inv_d;
    vec3 tmn = min(t0, t1);
    vec3 tmx = max(t0, t1);
    float tnear = max(max(tmn.x, tmn.y), tmn.z);
    float tfar  = min(min(tmx.x, tmx.y), tmx.z);
    if (tfar < max(tnear, 0.0) || tnear > t_max) return false;
    t_hit = max(tnear, 0.0);
    return true;
}

// How far the surface a ray starts from may be from what rays are tested against, in world
// units. For the shader to set; the shadow techniques read it.
float velk_ray_lift = 0.0;
// Set when a ray is tested against instances that a motion function moves, which could stand
// in its way at another time. For the shader to clear before tracing and read after.
bool velk_ray_met_motion = false;
// The pieces of work (triangles, nodes, records) this thread's walks have done, all of them
// added up. For the shader to read.
uint velk_ray_steps = 0u;
// Of those, the triangles tested and the records; the nodes of the top tree, of partitions'
// trees and of meshes' or clusters' trees; and the partitions entered.
uint velk_ray_tris = 0u;
uint velk_ray_records = 0u;
uint velk_ray_records_hit = 0u;  // of the records, those whose box the ray passes through
// Of those, the patches of small members, the quads, those without an instance, and the clusters.
uint velk_ray_hit_patches = 0u;
uint velk_ray_hit_quads = 0u;
uint velk_ray_hit_none = 0u;
uint velk_ray_hit_clusters = 0u;
// And the whole meshes walked, and those without a tree to walk.
uint velk_ray_hit_meshes = 0u;
uint velk_ray_hit_empty = 0u;
// The whole meshes walked, by their box's widest side (under 1, 4, 16 world units, more) and by
// their triangles (under 64, 512, 4096, more): two counts of 16 bits a word, the first two in
// the first word.
uint velk_ray_mesh_wide[2] = uint[2](0u, 0u);
uint velk_ray_mesh_tris[2] = uint[2](0u, 0u);

// Adds one to count `b` (0 to 3) of `counts`.
void velk_ray_count_in(inout uint counts[2], uint b)
{
    if (b < 2u) {
        counts[0] += 1u << (16u * b);
    } else {
        counts[1] += 1u << (16u * (b - 2u));
    }
}
uint velk_ray_top_nodes = 0u;
uint velk_ray_partition_nodes = 0u;
uint velk_ray_mesh_nodes = 0u;
uint velk_ray_partitions = 0u;
// The deepest this thread's walks' stack got, against VELK_TRACE_STACK_SIZE.
int velk_ray_deepest = 0;
// How far along a shadow ray what stopped it was, or the nearest patch that kept part of its
// light. For the shader to clear before tracing and read after.
float velk_ray_blocker_t = 0.0;

// The walk's stack: a slice of VELK_TRACE_STACK_SIZE for each thread of the work group, which
// has VELK_RT_GROUP_THREADS (64 unless the shader says), in shared memory, since as an array of
// its own it would be in scratch memory.
#ifndef VELK_RT_GROUP_THREADS
#define VELK_RT_GROUP_THREADS 64
#endif
shared uint s_walk_stack[uint(VELK_RT_GROUP_THREADS) * uint(VELK_TRACE_STACK_SIZE)];

// Which of its object's six axis directions a surface facing `n`, in the object's own space,
// faces: 0 and 1 for +x and -x, then y, then z, as the G-buffer's surface attachment has it.
uint velk_facing_side(vec3 n)
{
    vec3 across = abs(n);
    return across.x >= across.y && across.x >= across.z ? (n.x < 0.0 ? 1u : 0u)
         : across.y >= across.z ? (n.y < 0.0 ? 3u : 2u) : (n.z < 0.0 ? 5u : 4u);
}

// How opaque the blended draw `draw` is where a ray hits triangle `corners` of cluster `cluster`
// at weights (1 - u - v, u, v): its material's opacity factor, times its texture's alpha at the
// hit's uv when it has one (VELK_DRAW_OPACITY); 1, solid, when it says nothing of it.
float velk_ray_opacity(uint draw, uint cluster, uvec3 corners, float u, float v)
{
    VelkDrawData d = velk_draw_data.data[draw];
    if ((d.flags & VELK_DRAW_OPACITY) == 0u) return 1.0;
    uint factor_word = (d.flags >> VELK_DRAW_OPACITY_FACTOR_SHIFT) & 255u;
    uint texture_word = (d.flags >> VELK_DRAW_OPACITY_TEXTURE_SHIFT) & 255u;
    float alpha = uintBitsToFloat(velk_material_words.data[d.material_words + factor_word]);
    uint texture = velk_material_words.data[d.material_words + texture_word];
    if (texture != 0u) {
        uint base = velk_cluster_vertex_word(d, cluster);
        vec2 uv = velk_unpack_vertex3d(base + corners.x * VELK_VERTEX3D_WORDS).uv * (1.0 - u - v)
                + velk_unpack_vertex3d(base + corners.y * VELK_VERTEX3D_WORDS).uv * u
                + velk_unpack_vertex3d(base + corners.z * VELK_VERTEX3D_WORDS).uv * v;
        alpha *= textureLod(velk_textures[nonuniformEXT(texture)], uv, 0.0).a;
    }
    return clamp(alpha, 0.0, 1.0);
}

// The composer generates the body after intersect snippets have been included.
// Built-in kinds forward to the functions the shader defines; visual-registered
// kinds call their snippets.
bool intersect_shape(Ray ray, RtShape shape, out RayHit hit);

#define VELK_RAY_NONE 0xFFFFFFFFu
#define VELK_RAY_LEAF 0x80000000u
// The most pieces of work (triangles, nodes, records) one walk does.
#define VELK_WALK_MAX_STEPS 65536u

vec3 velk_ray_vec3(uint at)
{
    return vec3(uintBitsToFloat(velk_culling.data[at]), uintBitsToFloat(velk_culling.data[at + 1u]),
                uintBitsToFloat(velk_culling.data[at + 2u]));
}

// `ray` in an instance's own space. For a mesh, where its vertices are, (local - offset) /
// size. Its direction is of unit length there; `scale` is how far it goes there per unit of
// the ray's t.
Ray velk_instance_ray(Ray ray, ElementInstance inst, bool mesh, out float scale)
{
    mat3 to_local = inverse(mat3(inst.world_matrix));
    Ray local;
    local.origin = to_local * (ray.origin + (VELK_GLOBALS.cam_pos.xyz - inst.world_matrix[3].xyz));
    local.dir = to_local * ray.dir;
    if (mesh) {
        vec3 size = mix(inst.size.xyz, vec3(1.0), equal(inst.size.xyz, vec3(0.0)));
        local.origin = (local.origin - inst.offset.xyz) / size;
        local.dir /= size;
    }
    scale = length(local.dir);
    local.dir /= max(scale, 1e-30);
    return local;
}

// What an item rays see that its own pass draws is to rays: the shape its visual's kind
// names, over its instance's extent.
RtShape velk_instance_shape(ElementInstance inst, uint kind, uint draw)
{
    RtShape shape;
    shape.origin = vec4(inst.offset.xyz, 0.0);
    shape.u_axis = vec4(inst.size.x, 0.0, 0.0, 0.0);
    shape.v_axis = vec4(0.0, inst.size.y, 0.0, 0.0);
    shape.w_axis = vec4(0.0, 0.0, inst.size.z, 0.0);
    shape.color = inst.color;
    shape.params = vec4(kind == 2u ? min(min(inst.size.x, inst.size.y), inst.size.z) * 0.5 : 0.0, 0.0, 0.0, 0.0);
    shape.shape_param = inst.params.x;
    shape.shape_kind = kind;
    shape.material_id = 0u;
    shape.texture_id = 0u;
    shape.material_base = 0u;
    if (draw != VELK_RAY_NONE) {
        VelkDrawData d = velk_draw_data.data[draw];
        shape.material_id = d.material_id;
        shape.texture_id = d.texture_id;
        shape.material_base = d.material_words;
    }
    shape._pad0 = 0u;
    shape._pad1 = 0u;
    shape._pad2 = 0u;
    return shape;
}
)"
                                                    R"(
// The walk: one loop that does one piece of work per iteration, a triangle, a node, or a
// record, so that the lanes of a wave each work from their own state. With @p closest it
// finds the nearest hit, and otherwise how much light gets through within t_max. Of hits as
// near as each other, the item drawn later is kept, as the raster keeps it.
float velk_walk(Ray ray, float t_max, bool closest, out RayHit hit)
{
    hit.t = t_max;
    hit.uv = vec2(0.0);
    hit.normal = vec3(0.0);
    hit.object = 0u;
    hit.local = vec3(0.0);
    hit.side = 0u;
    hit.draw = VELK_RAY_NONE;
    hit.instance = VELK_RAY_NONE;
    hit.cluster = VELK_NO_CLUSTER;
    hit.corners = uvec3(VELK_NO_CORNERS);
    hit.weights = vec2(0.0);
    hit.record = VELK_NO_RECORD;

    uint top = VELK_GLOBALS.ray_top;
    if (top == VELK_RAY_NONE || velk_culling.data[top + VELK_RAY_TOP_LEAVES] == 0u) return closest ? 0.0 : 1.0;
    uint top_cuts = top + VELK_RAY_TOP_HEADER_WORDS;
    uint top_leaves = top_cuts + velk_culling.data[top + VELK_RAY_TOP_CUTS] * VELK_RAY_TOP_CUT_WORDS;
    uint top_nodes = top_leaves + velk_culling.data[top + VELK_RAY_TOP_LEAVES] * VELK_RAY_TOP_LEAF_WORDS;
    vec3 cam = VELK_GLOBALS.cam_pos.xyz;

    const uint kTop = 0u;
    const uint kPartition = 1u;
    const uint kMesh = 2u;
    const int kStack = VELK_TRACE_STACK_SIZE;
    // This thread's slice of the work group's stack (s_walk_stack).
    uint stack_base = gl_LocalInvocationIndex * uint(VELK_TRACE_STACK_SIZE);
    int sp = 0;
    s_walk_stack[stack_base + uint(sp++)] = velk_culling.data[top + VELK_RAY_TOP_ROOT];
    uint level = kTop;
    int partition_scope = 0;
    int mesh_scope = 0;

    // The partition being walked: where its records and its nodes are, and its cut's.
    uint records = 0u;
    uint nodes = 0u;
    uint instances_base = 0u;
    uint items = VELK_RAY_NONE;
    uint quads = VELK_RAY_NONE;  // the view's quad list (kQuadListHeaderWords), for a quad record's draw
    // Its records left in the leaf being walked.
    uint record_at = 0u;   // in the partition's ray list
    uint ray_list = 0u;
    uint records_left = 0u;

    // The reciprocal of the ray's direction, for the boxes of the top and partition trees.
    vec3 inv_dir = velk_ray_inv_dir(ray.dir);
    // The record being walked, and the ray in its instance's space with its direction's
    // reciprocal, for the boxes of its mesh's tree.
    Ray walk_ray = ray;
    vec3 walk_inv_dir = inv_dir;
    float mesh_scale = 1.0;
    uint cur_draw = VELK_RAY_NONE;
    bool cur_blended = false;  // the record walked is of a blended draw (VELK_LOD_RAYS_ONLY)
    uint cur_instance = VELK_RAY_NONE;
    uint cur_cluster = VELK_NO_CLUSTER;
    uint cur_record = VELK_NO_RECORD;
    uint cur_key = 0u;
    uint mesh_base = 0u;
    uint word_base = 0u;
    // A tree's leaf: its next triangle word, its counts (triangles left in the low half, its
    // cluster's vertices in the high half), and its cluster's first position word.
    uint triangle_at = 0u;
    uint leaf_counts = 0u;
    uint positions = 0u;
    // A cluster tested as drawn: its next index word, its triangles left, and its vertices.
    uint drawn_at = 0u;
    uint drawn_left = 0u;
    uint drawn_vertices = 0u;

    float best_t = t_max;
    uint best_key = 0u;
    uint best_draw = VELK_RAY_NONE;
    uint best_instance = VELK_RAY_NONE;
    uint best_cluster = VELK_NO_CLUSTER;
    uint best_record = VELK_NO_RECORD;
    uvec3 best_corners = uvec3(VELK_NO_CORNERS);
    vec2 best_weights = vec2(0.0);
    vec2 best_uv = vec2(0.0);
    vec3 best_normal = vec3(0.0);  // where nothing better is known: in the shape's or mesh's space
    bool best_quad = false;        // best_instance is a quad's word, not an instance
    float through = 1.0;           // the light the patches passed so far let through (VELK_CUT_RAY_QUAD)

    // At most VELK_WALK_MAX_STEPS pieces of work, whatever the trees it reads say: a walk that
    // runs out ends with what it has found.
    for (uint step = 0u; step < VELK_WALK_MAX_STEPS; ++step) {
        ++velk_ray_steps;
        velk_ray_deepest = max(velk_ray_deepest, sp);
        bool tested = false;
        vec3 v0, v1, v2;
        uvec3 corners;
        if ((leaf_counts & 0xFFFFu) != 0u) {
            uint tri = velk_cluster_words.data[triangle_at];
            ++triangle_at;
            --leaf_counts;
            uvec3 local_ids = uvec3(tri & 0xFFu, (tri >> 8u) & 0xFFu, (tri >> 16u) & 0xFFu);
            uint p0 = positions + local_ids.x * 3u;
            uint p1 = positions + local_ids.y * 3u;
            uint p2 = positions + local_ids.z * 3u;
            v0 = vec3(uintBitsToFloat(velk_cluster_words.data[p0]), uintBitsToFloat(velk_cluster_words.data[p0 + 1u]),
                      uintBitsToFloat(velk_cluster_words.data[p0 + 2u]));
            v1 = vec3(uintBitsToFloat(velk_cluster_words.data[p1]), uintBitsToFloat(velk_cluster_words.data[p1 + 1u]),
                      uintBitsToFloat(velk_cluster_words.data[p1 + 2u]));
            v2 = vec3(uintBitsToFloat(velk_cluster_words.data[p2]), uintBitsToFloat(velk_cluster_words.data[p2 + 1u]),
                      uintBitsToFloat(velk_cluster_words.data[p2 + 2u]));
            // Each local vertex's index among the vertices the draw's indices count from.
            uint ids = positions + (leaf_counts >> 16u) * 3u;
            corners = uvec3(velk_cluster_words.data[ids + local_ids.x], velk_cluster_words.data[ids + local_ids.y],
                            velk_cluster_words.data[ids + local_ids.z]);
            tested = true;
        } else if (drawn_left != 0u) {
            corners = uvec3(velk_mesh_words.data[drawn_at], velk_mesh_words.data[drawn_at + 1u],
                            velk_mesh_words.data[drawn_at + 2u]);
            drawn_at += 3u;
            --drawn_left;
            v0 = velk_unpack_vertex3d(drawn_vertices + corners.x * VELK_VERTEX3D_WORDS).position;
            v1 = velk_unpack_vertex3d(drawn_vertices + corners.y * VELK_VERTEX3D_WORDS).position;
            v2 = velk_unpack_vertex3d(drawn_vertices + corners.z * VELK_VERTEX3D_WORDS).position;
            tested = true;
        }
        if (tested) {
            ++velk_ray_tris;
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
            if (tt < 1e-4 * max(length(e1), length(e2))) continue;
            float world_t = tt / mesh_scale;
            if (world_t < best_t * 0.99999 || (world_t <= best_t * 1.00001 && cur_key > best_key)) {
                // A blended surface lets through what it does not cover: a shadow ray keeps that
                // much of its light and goes on; a closest one goes on past one mostly clear.
                if (cur_blended && cur_draw != VELK_RAY_NONE) {
                    float alpha = velk_ray_opacity(cur_draw, cur_cluster, corners, u, v);
                    if (!closest) {
                        // A clear texel blocks nothing, so it is no blocker either.
                        if (alpha > 0.01) {
                            through *= 1.0 - alpha;
                            velk_ray_blocker_t = velk_ray_blocker_t > 0.0 ? min(velk_ray_blocker_t, world_t) : world_t;
                            if (through < 0.01) return 0.0;
                        }
                        continue;
                    }
                    if (alpha < 0.5) continue;
                }
                if (!closest) {
                    velk_ray_blocker_t = world_t;
                    return 0.0;
                }
                best_t = world_t;
                best_key = cur_key;
                best_draw = cur_draw;
                best_instance = cur_instance;
                best_cluster = cur_cluster;
                best_record = cur_record;
                best_corners = corners;
                best_weights = vec2(u, v);
                best_normal = cross(e1, e2);
                best_quad = false;
            }
)"
                                                    R"(
        } else if (level == kMesh) {
            if (sp == mesh_scope) {
                level = kPartition;
                continue;
            }
            uint ref = s_walk_stack[stack_base + uint(--sp)];
            if ((ref & VELK_BLAS_LEAF) != 0u) {
                uint leaf = word_base + (ref & ~VELK_BLAS_LEAF);
                triangle_at = word_base + velk_cluster_words.data[leaf];
                leaf_counts = velk_cluster_words.data[leaf + 1u];
                positions = word_base + velk_cluster_words.data[leaf + 2u];
                continue;
            }
            // Within a cluster or a mesh without one there are only nodes and leaves.
            if ((ref & (VELK_BLAS_CLUSTER | VELK_BLAS_UPPER)) != 0u) continue;
            ++velk_ray_mesh_nodes;
            VelkMeshNode node = velk_mesh_nodes.data[mesh_base + ref];
            float limit = best_t * 1.00001 * mesh_scale;
            float t_l, t_r;
            bool h_l = ray_aabb(walk_ray.origin, walk_inv_dir, node.a.xyz, vec3(node.a.w, node.b.xy), limit, t_l);
            bool h_r = ray_aabb(walk_ray.origin, walk_inv_dir, vec3(node.b.zw, node.c.x), node.c.yzw, limit, t_r);
            // The nearer child on top, so that a near hit or a blocker is found sooner. A child
            // that is not there has a box no ray reaches.
            bool left_first = h_l && (!h_r || t_l <= t_r);
            uint near_ref = left_first ? node.refs.x : node.refs.y;
            uint far_ref = left_first ? node.refs.y : node.refs.x;
            if (h_l && h_r && sp < kStack) s_walk_stack[stack_base + uint(sp++)] = far_ref;
            if ((h_l || h_r) && sp < kStack) s_walk_stack[stack_base + uint(sp++)] = near_ref;
        } else if (records_left > 0u) {
            ++velk_ray_records;
            uint ray_entry = record_at;
            record_at += VELK_CUT_RAY_WORDS;
            --records_left;
            // The record's box first, so that a record the ray misses is never read.
            vec3 box_lo = velk_ray_vec3(ray_entry + VELK_CUT_RAY_MIN) - cam;
            vec3 box_hi = velk_ray_vec3(ray_entry + VELK_CUT_RAY_MAX) - cam;
            if (box_lo.x > box_hi.x) continue;
            float box_t;
            if (!ray_aabb(ray.origin, inv_dir, box_lo, box_hi, best_t * 1.00001, box_t)) continue;
            ++velk_ray_records_hit;
            uint r = records + velk_culling.data[ray_entry + VELK_CUT_RAY_INDEX] * VELK_RECORD_END;
            uint entry = velk_culling.data[r + VELK_RECORD_ENTRY];
            uint cluster = velk_culling.data[r + VELK_RECORD_CLUSTER];
            uint named = velk_culling.data[r + VELK_RECORD_SLOT];
            // A patch of small members: a shadow ray keeps the part of its light their quads leave,
            // by the share of the patch's sphere they cover, the node's quad keeping their area,
            // seen from every way alike (half of it on average). Probe rays pass it.
            if (entry == VELK_CUT_RAY_QUAD) {
                ++velk_ray_hit_patches;
                if (closest) continue;
                vec3 c = velk_ray_vec3(cluster + VELK_QUAD_NODE_CENTER) - cam;
                float radius = uintBitsToFloat(velk_culling.data[cluster + VELK_QUAD_NODE_RADIUS]);
                if (radius <= 0.0) continue;
                vec3 to = c - ray.origin;
                float along = dot(to, ray.dir);
                float off = dot(to, to) - along * along;
                if (off > radius * radius || along + radius < 0.0 || along - radius > best_t) continue;
                float area = 4.0 * length(cross(velk_ray_vec3(cluster + VELK_QUAD_NODE_U), velk_ray_vec3(cluster + VELK_QUAD_NODE_V)));
                float covered = min(0.5 * area / (3.14159265 * radius * radius), 1.0);
                through *= 1.0 - covered;
                velk_ray_blocker_t = velk_ray_blocker_t > 0.0 ? min(velk_ray_blocker_t, max(along, 0.0)) : max(along, 0.0);
                if (through < 0.01) return 0.0;
                continue;
            }
            // A record for light only is traced as the cluster it is.
            if (velk_cut_light_only(entry)) entry &= ~VELK_CUT_LIGHT_ONLY;
            if (entry == VELK_CUT_QUAD) {
                ++velk_ray_hit_quads;
                // A quad: its plane, then where on it along its two sides, 0 to 1 within it.
                vec3 c = velk_ray_vec3(cluster + VELK_QUAD_NODE_CENTER) - cam;
                vec3 u = velk_ray_vec3(cluster + VELK_QUAD_NODE_U);
                vec3 v = velk_ray_vec3(cluster + VELK_QUAD_NODE_V);
                vec3 n = cross(u, v);
                float facing = dot(ray.dir, n);
                if (abs(facing) <= 1e-12 * dot(n, n)) continue;
                float tt = dot(c - ray.origin, n) / facing;
                if (tt <= 0.0) continue;
                vec3 from_corner = ray.origin + ray.dir * tt - (c - u - v);
                float a = dot(from_corner, u) / (2.0 * dot(u, u));
                float b = dot(from_corner, v) / (2.0 * dot(v, v));
                if (a < 0.0 || a > 1.0 || b < 0.0 || b > 1.0) continue;
                if (tt < best_t * 0.99999) {
                    if (!closest) {
                        velk_ray_blocker_t = tt;
                        return 0.0;
                    }
                    best_t = tt;
                    best_key = 0u;
                    best_draw = quads != VELK_RAY_NONE ? velk_culling.data[quads + VELK_QUAD_LIST_DRAW] : VELK_RAY_NONE;
                    best_instance = cluster;
                    best_cluster = VELK_NO_CLUSTER;
                    best_record = r;
                    // The unit quad is a strip: corners 0 (0, 0), 1 (1, 0), 2 (0, 1) and 3 (1, 1),
                    // its triangles 0, 1, 2 and 1, 2, 3.
                    if (a + b <= 1.0) {
                        best_corners = uvec3(0u, 1u, 2u);
                        best_weights = vec2(a, b);
                    } else {
                        best_corners = uvec3(1u, 2u, 3u);
                        best_weights = vec2(1.0 - a, a + b - 1.0);
                    }
                    best_normal = vec3(0.0, 0.0, 1.0);
                    best_quad = true;
                }
                continue;
            }
            uint instance = instances_base + named;
            uint draw = VELK_RAY_NONE;
            uint kind = 255u;
            uint mesh = VELK_INVALID_MESH_STATIC;
            uint key = 0u;
            if (items != VELK_RAY_NONE) {
                uint item = items + named * VELK_PLAIN_ITEM_WORDS;
                instance = velk_culling.data[item];
                draw = velk_culling.data[item + VELK_PLAIN_ITEM_DRAW];
                kind = velk_culling.data[item + VELK_PLAIN_ITEM_KIND];
                mesh = velk_culling.data[item + VELK_PLAIN_ITEM_MESH];
                key = named + 1u;
            }
            // An instance without a place in its raster region is nothing to rays.
            if (instance == VELK_RAY_NONE) {
                ++velk_ray_hit_none;
                continue;
            }
            ElementInstance inst = velk_instances.data[instance];
            if (inst.world_matrix[3][3] == 0.0) {
                ++velk_ray_hit_none;
                continue;
            }
            cur_instance = instance;
            cur_record = r;
            cur_key = key;
            if (entry != VELK_RAY_NONE) {
                ++velk_ray_hit_clusters;
                // A cluster of a hierarchy: its triangle tree, or as drawn.
                cur_draw = velk_culling.data[entry + VELK_CLUSTER_ENTRY_DRAW];
                cur_blended = (velk_culling.data[entry + VELK_CLUSTER_ENTRY_FLAGS] & VELK_LOD_RAYS_ONLY) != 0u;
                cur_cluster = velk_culling.data[entry + VELK_CLUSTER_ENTRY_TABLE] + cluster * VELK_LOD_CLUSTER_WORDS;
                walk_ray = velk_instance_ray(ray, inst, true, mesh_scale);
                walk_inv_dir = velk_ray_inv_dir(walk_ray.dir);
                // Its header names its tree: its own, or for a deforming mesh its part of the mesh's
                // tree, which the GPU refits as the vertices move.
                uint rays = velk_culling.data[entry + VELK_CLUSTER_ENTRY_RAY];
                uint header = rays + cluster * 20u;
                uint root = rays != VELK_RAY_NONE ? velk_cluster_words.data[header + 10u] : VELK_RAY_NONE;
                if (root != VELK_RAY_NONE) {
                    mesh_base = velk_cluster_words.data[header + 13u];
                    word_base = velk_cluster_words.data[header + 14u];
                    level = kMesh;
                    mesh_scope = sp;
                    if (sp < kStack) s_walk_stack[stack_base + uint(sp++)] = root;
                } else {
                    // Without a tree, such as a deforming mesh's whose tree was built again, its
                    // triangles are tested as they are drawn.
                    drawn_at = velk_culling.data[cur_cluster + VELK_LOD_CLUSTER_INDEX];
                    drawn_left = velk_culling.data[cur_cluster + VELK_LOD_CLUSTER_TRIANGLES];
                    drawn_vertices = velk_culling.data[cur_cluster + VELK_LOD_CLUSTER_VERTEX];
                }
            } else if (kind == 255u) {
                // A whole mesh: its own tree.
                if (mesh == VELK_INVALID_MESH_STATIC) {
                    ++velk_ray_hit_empty;
                    continue;
                }
                MeshStaticData st = velk_mesh_static_records.data[mesh];
                if (st.blas_node_count == 0u || st.triangle_count == 0u) {
                    ++velk_ray_hit_empty;
                    continue;
                }
                ++velk_ray_hit_meshes;
                vec3 box_side = box_hi - box_lo;
                float wide = max(max(box_side.x, box_side.y), box_side.z);
                velk_ray_count_in(velk_ray_mesh_wide, wide < 1.0 ? 0u : wide < 4.0 ? 1u : wide < 16.0 ? 2u : 3u);
                velk_ray_count_in(velk_ray_mesh_tris, st.triangle_count < 64u ? 0u : st.triangle_count < 512u ? 1u
                                                      : st.triangle_count < 4096u ? 2u : 3u);
                cur_draw = draw;
                cur_blended = false;
                cur_cluster = VELK_NO_CLUSTER;
                walk_ray = velk_instance_ray(ray, inst, true, mesh_scale);
                walk_inv_dir = velk_ray_inv_dir(walk_ray.dir);
                mesh_base = st.blas_node_base;
                word_base = st.cluster_word_base;
                level = kMesh;
                mesh_scope = sp;
                if (sp < kStack) s_walk_stack[stack_base + uint(sp++)] = st.blas_root;
            } else {
                // An analytic shape, over the instance's extent.
                float scale;
                Ray local = velk_instance_ray(ray, inst, false, scale);
                RayHit h;
                if (intersect_shape(local, velk_instance_shape(inst, kind, draw), h) && h.t > 0.0) {
                    float world_t = h.t / scale;
                    if (world_t < best_t * 0.99999 || (world_t <= best_t * 1.00001 && key > best_key)) {
                        if (!closest) {
                            velk_ray_blocker_t = world_t;
                            return 0.0;
                        }
                        best_t = world_t;
                        best_key = key;
                        best_draw = draw;
                        best_instance = instance;
                        best_cluster = VELK_NO_CLUSTER;
                        best_record = r;
                        best_corners = uvec3(VELK_NO_CORNERS);
                        best_uv = h.uv;
                        best_normal = h.normal;
                        best_quad = false;
                    }
                }
            }
)"
                                                    R"(
        } else if (level == kPartition) {
            if (sp == partition_scope) {
                level = kTop;
                continue;
            }
            uint ref = s_walk_stack[stack_base + uint(--sp)];
            if ((ref & VELK_RAY_LEAF) != 0u) {
                uint first = (ref & ~VELK_RAY_LEAF) >> 2u;
                record_at = ray_list + first * VELK_CUT_RAY_WORDS;
                records_left = (ref & 3u) + 1u;
                continue;
            }
            ++velk_ray_partition_nodes;
            uint node = nodes + ref * VELK_CUT_NODE_WORDS;
            float t_l, t_r;
            bool h_l = ray_aabb(ray.origin, inv_dir, velk_ray_vec3(node + VELK_CUT_NODE_LEFT_MIN) - cam,
                                velk_ray_vec3(node + VELK_CUT_NODE_LEFT_MAX) - cam, best_t * 1.00001, t_l);
            bool h_r = ray_aabb(ray.origin, inv_dir, velk_ray_vec3(node + VELK_CUT_NODE_RIGHT_MIN) - cam,
                                velk_ray_vec3(node + VELK_CUT_NODE_RIGHT_MAX) - cam, best_t * 1.00001, t_r);
            bool left_first = h_l && (!h_r || t_l <= t_r);
            uint near_ref = velk_culling.data[node + (left_first ? VELK_CUT_NODE_LEFT_REF : VELK_CUT_NODE_RIGHT_REF)];
            uint far_ref = velk_culling.data[node + (left_first ? VELK_CUT_NODE_RIGHT_REF : VELK_CUT_NODE_LEFT_REF)];
            if (h_l && h_r && far_ref != VELK_RAY_NONE && sp < kStack) s_walk_stack[stack_base + uint(sp++)] = far_ref;
            if ((h_l || h_r) && near_ref != VELK_RAY_NONE && sp < kStack) s_walk_stack[stack_base + uint(sp++)] = near_ref;
        } else {
            if (sp == 0) break;
            uint ref = s_walk_stack[stack_base + uint(--sp)];
            if ((ref & VELK_RAY_LEAF) != 0u) {
                ++velk_ray_partitions;
                // A partition: its tree, whose root's box its leaf here already has.
                uint leaf = top_leaves + (ref & ~VELK_RAY_LEAF) * VELK_RAY_TOP_LEAF_WORDS;
                uint cut_entry = top_cuts + velk_culling.data[leaf + VELK_RAY_TOP_LEAF_CUT] * VELK_RAY_TOP_CUT_WORDS;
                uint cut = velk_culling.data[cut_entry + VELK_RAY_TOP_CUT_CUT];
                uint state = velk_culling.data[cut + VELK_CUT_STATES_WORD]
                           + velk_culling.data[leaf + VELK_RAY_TOP_LEAF_PARTITION] * VELK_CUT_PARTITION_WORDS;
                // What rays trace of it: its ray list, over its records, in its cut's pool, which
                // its tree's references name by place.
                if (velk_culling.data[state + VELK_PART_RAY_COUNT] == 0u) continue;
                records = velk_culling.data[cut + VELK_CUT_RECORD_POOL];
                ray_list = velk_culling.data[cut + VELK_CUT_RAY_POOL];
                nodes = velk_culling.data[cut + VELK_CUT_NODE_POOL];
                instances_base = velk_culling.data[cut_entry + VELK_RAY_TOP_CUT_INSTANCES_BASE];
                items = velk_culling.data[cut_entry + VELK_RAY_TOP_CUT_ITEMS];
                quads = velk_culling.data[cut_entry + VELK_RAY_TOP_CUT_QUADS];
                if (velk_culling.data[cut_entry + VELK_RAY_TOP_CUT_MOVES] != 0u) velk_ray_met_motion = true;
                level = kPartition;
                partition_scope = sp;
                if (sp < kStack) s_walk_stack[stack_base + uint(sp++)] = velk_culling.data[state + VELK_PART_ROOT];
                continue;
            }
            ++velk_ray_top_nodes;
            uint node = top_nodes + ref * VELK_CUT_NODE_WORDS;
            float t_l, t_r;
            bool h_l = ray_aabb(ray.origin, inv_dir, velk_ray_vec3(node + VELK_CUT_NODE_LEFT_MIN) - cam,
                                velk_ray_vec3(node + VELK_CUT_NODE_LEFT_MAX) - cam, best_t * 1.00001, t_l);
            bool h_r = ray_aabb(ray.origin, inv_dir, velk_ray_vec3(node + VELK_CUT_NODE_RIGHT_MIN) - cam,
                                velk_ray_vec3(node + VELK_CUT_NODE_RIGHT_MAX) - cam, best_t * 1.00001, t_r);
            bool left_first = h_l && (!h_r || t_l <= t_r);
            uint near_ref = velk_culling.data[node + (left_first ? VELK_CUT_NODE_LEFT_REF : VELK_CUT_NODE_RIGHT_REF)];
            uint far_ref = velk_culling.data[node + (left_first ? VELK_CUT_NODE_RIGHT_REF : VELK_CUT_NODE_LEFT_REF)];
            if (h_l && h_r && far_ref != VELK_RAY_NONE && sp < kStack) s_walk_stack[stack_base + uint(sp++)] = far_ref;
            if ((h_l || h_r) && near_ref != VELK_RAY_NONE && sp < kStack) s_walk_stack[stack_base + uint(sp++)] = near_ref;
        }
    }

)"
                                                    R"(
    if (!closest || best_instance == VELK_RAY_NONE) return closest ? 0.0 : through;

    hit.t = best_t;
    hit.draw = best_draw;
    hit.instance = best_instance;
    hit.cluster = best_cluster;
    hit.record = best_record;
    hit.corners = best_corners;
    hit.weights = best_weights;
    ElementInstance inst = best_quad ? velk_quad_instance(best_instance) : velk_instances.data[best_instance];
    hit.object = inst.params.w;
    mat3 to_world = mat3(inst.world_matrix);
    mat3 to_local = inverse(to_world);
    // Where on its object, in the object's own axes but world units, as the G-buffer has it.
    vec3 axis_scale = vec3(length(to_world[0]), length(to_world[1]), length(to_world[2]));
    hit.local = to_local * (ray.origin + ray.dir * best_t + (VELK_GLOBALS.cam_pos.xyz - inst.world_matrix[3].xyz)) * axis_scale;

    vec3 local_normal;
    if (best_corners.x == VELK_NO_CORNERS) {
        hit.uv = best_uv;
        local_normal = best_normal;
    } else {
        // The corners as drawn: their normals and uvs, weighted.
        vec3 size = mix(inst.size.xyz, vec3(1.0), equal(inst.size.xyz, vec3(0.0)));
        vec3 mesh_normal = best_normal;
        if (best_draw != VELK_RAY_NONE) {
            VelkDrawData d = velk_draw_data.data[best_draw];
            uint base = velk_cluster_vertex_word(d, best_cluster);
            VelkVertex3D c0 = velk_unpack_vertex3d(base + best_corners.x * VELK_VERTEX3D_WORDS);
            VelkVertex3D c1 = velk_unpack_vertex3d(base + best_corners.y * VELK_VERTEX3D_WORDS);
            VelkVertex3D c2 = velk_unpack_vertex3d(base + best_corners.z * VELK_VERTEX3D_WORDS);
            vec3 w = vec3(1.0 - best_weights.x - best_weights.y, best_weights);
            vec3 smooth_normal = c0.normal * w.x + c1.normal * w.y + c2.normal * w.z;
            if (dot(smooth_normal, smooth_normal) > 0.0) mesh_normal = smooth_normal;
            hit.uv = c0.uv * w.x + c1.uv * w.y + c2.uv * w.z;
        }
        // From the mesh's space to the instance's: the inverse transpose of the size.
        local_normal = mesh_normal / size;
    }
    hit.normal = normalize(transpose(to_local) * local_normal);
    hit.side = velk_facing_side(local_normal);
    return 1.0;
}

// How much light gets through along a shadow ray within t_max: 0 at the first blocker.
float trace_visibility(Ray ray, float t_max)
{
    RayHit h;
    return velk_walk(ray, t_max, false, h);
}

// Whether anything blocks the ray within t_max.
bool trace_any_hit(Ray ray, float t_max)
{
    return trace_visibility(ray, t_max) <= 0.0;
}

// The nearest hit within t_max, with its normal in world space and what was hit as a pixel
// names it (see RayHit).
bool trace_closest_hit(Ray ray, float t_max, out RayHit hit)
{
    return velk_walk(ray, t_max, true, hit) > 0.5;
}

// Unbounded closest hit. On a miss `hit.t` stays at 1e30, which callers that
// shade the miss rely on.
bool trace_closest_hit(Ray ray, out RayHit hit)
{
    return trace_closest_hit(ray, 1e30, hit);
}
)";

} // namespace velk

#endif // VELK_RENDER_FRAME_RAY_WALK_H
