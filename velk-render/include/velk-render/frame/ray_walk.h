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
uint velk_ray_top_nodes = 0u;
uint velk_ray_partition_nodes = 0u;
uint velk_ray_mesh_nodes = 0u;
uint velk_ray_partitions = 0u;
// The deepest this thread's walks' stack got, against VELK_TRACE_STACK_SIZE.
int velk_ray_deepest = 0;

// The walk's stack: a slice of VELK_TRACE_STACK_SIZE for each thread of the work group, which
// has 64, in shared memory, since as an array of its own it would be in scratch memory.
shared uint s_walk_stack[64u * uint(VELK_TRACE_STACK_SIZE)];

// Which of its object's six axis directions a surface facing `n`, in the object's own space,
// faces: 0 and 1 for +x and -x, then y, then z, as the G-buffer's surface attachment has it.
uint velk_facing_side(vec3 n)
{
    vec3 across = abs(n);
    return across.x >= across.y && across.x >= across.z ? (n.x < 0.0 ? 1u : 0u)
         : across.y >= across.z ? (n.y < 0.0 ? 3u : 2u) : (n.z < 0.0 ? 5u : 4u);
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

// Nodes in the tree over a partition's `records` records. Mirrors cut_tree_nodes.
uint velk_ray_tree_nodes(uint records)
{
    if (records == 0u) return 0u;
    uint below = (records + 3u) / 4u;
    uint total = 0u;
    do {
        below = (below + 1u) / 2u;
        total += below;
    } while (below > 1u);
    return total;
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
    if (top == VELK_RAY_NONE || velk_culling.data[top + 1u] == 0u) return closest ? 0.0 : 1.0;
    uint top_cuts = top + 4u;
    uint top_leaves = top_cuts + velk_culling.data[top] * 8u;
    uint top_nodes = top_leaves + velk_culling.data[top + 1u] * 2u;
    vec3 cam = VELK_GLOBALS.cam_pos.xyz;

    const uint kTop = 0u;
    const uint kPartition = 1u;
    const uint kMesh = 2u;
    const int kStack = VELK_TRACE_STACK_SIZE;
    // This thread's slice of the work group's stack (s_walk_stack).
    uint stack_base = gl_LocalInvocationIndex * uint(VELK_TRACE_STACK_SIZE);
    int sp = 0;
    s_walk_stack[stack_base + uint(sp++)] = velk_culling.data[top + 3u];
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

    // The record being walked, and the ray in its instance's space.
    Ray walk_ray = ray;
    float mesh_scale = 1.0;
    uint cur_draw = VELK_RAY_NONE;
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
                if (!closest) return 0.0;
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
            bool h_l = ray_aabb(walk_ray, node.a.xyz, vec3(node.a.w, node.b.xy), limit, t_l);
            bool h_r = ray_aabb(walk_ray, vec3(node.b.zw, node.c.x), node.c.yzw, limit, t_r);
            // The nearer child on top, so that a near hit or a blocker is found sooner. A child
            // that is not there has a box no ray reaches.
            bool left_first = h_l && (!h_r || t_l <= t_r);
            uint near_ref = left_first ? node.refs.x : node.refs.y;
            uint far_ref = left_first ? node.refs.y : node.refs.x;
            if (h_l && h_r && sp < kStack) s_walk_stack[stack_base + uint(sp++)] = far_ref;
            if ((h_l || h_r) && sp < kStack) s_walk_stack[stack_base + uint(sp++)] = near_ref;
        } else if (records_left > 0u) {
            ++velk_ray_records;
            uint r = records + velk_culling.data[record_at] * VELK_RECORD_END;
            record_at += 1u;
            --records_left;
            uint entry = velk_culling.data[r + VELK_RECORD_ENTRY];
            uint cluster = velk_culling.data[r + VELK_RECORD_CLUSTER];
            uint named = velk_culling.data[r + VELK_RECORD_SLOT];
            // A patch of small members: a shadow ray keeps the part of its light their quads leave,
            // by the share of the patch's sphere they cover, the node's quad keeping their area,
            // seen from every way alike (half of it on average). Probe rays pass it.
            if (entry == VELK_CUT_RAY_QUAD) {
                if (closest) continue;
                vec3 c = velk_ray_vec3(cluster) - cam;
                float radius = uintBitsToFloat(velk_culling.data[cluster + 10u]);
                if (radius <= 0.0) continue;
                vec3 to = c - ray.origin;
                float along = dot(to, ray.dir);
                float off = dot(to, to) - along * along;
                if (off > radius * radius || along + radius < 0.0 || along - radius > best_t) continue;
                float area = 4.0 * length(cross(velk_ray_vec3(cluster + 3u), velk_ray_vec3(cluster + 6u)));
                float covered = min(0.5 * area / (3.14159265 * radius * radius), 1.0);
                through *= 1.0 - covered;
                if (through < 0.01) return 0.0;
                continue;
            }
            // A record for light only is traced as the cluster it is.
            if (velk_cut_light_only(entry)) entry &= ~VELK_CUT_LIGHT_ONLY;
            if (entry == VELK_CUT_QUAD) {
                // A quad: its plane, then where on it along its two sides, 0 to 1 within it.
                vec3 c = velk_ray_vec3(cluster) - cam;
                vec3 u = velk_ray_vec3(cluster + 3u);
                vec3 v = velk_ray_vec3(cluster + 6u);
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
                    if (!closest) return 0.0;
                    best_t = tt;
                    best_key = 0u;
                    best_draw = quads != VELK_RAY_NONE ? velk_culling.data[quads + 5u] : VELK_RAY_NONE;
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
                draw = velk_culling.data[item + 3u];
                kind = velk_culling.data[item + 4u];
                mesh = velk_culling.data[item + 5u];
                key = named + 1u;
            }
            // An instance without a place in its raster region is nothing to rays.
            if (instance == VELK_RAY_NONE) continue;
            ElementInstance inst = velk_instances.data[instance];
            if (inst.world_matrix[3][3] == 0.0) continue;
            cur_instance = instance;
            cur_record = r;
            cur_key = key;
            if (entry != VELK_RAY_NONE) {
                // A cluster of a hierarchy: its triangle tree, or as drawn.
                cur_draw = velk_culling.data[entry + VELK_CLUSTER_ENTRY_DRAW];
                cur_cluster = velk_culling.data[entry + VELK_CLUSTER_ENTRY_TABLE] + cluster * VELK_LOD_CLUSTER_WORDS;
                walk_ray = velk_instance_ray(ray, inst, true, mesh_scale);
                uint rays = velk_culling.data[entry + VELK_CLUSTER_ENTRY_RAY];
                if (rays != VELK_RAY_NONE) {
                    uint header = rays + cluster * 20u;
                    mesh_base = velk_cluster_words.data[header + 13u];
                    word_base = velk_cluster_words.data[header + 14u];
                    level = kMesh;
                    mesh_scope = sp;
                    if (sp < kStack) s_walk_stack[stack_base + uint(sp++)] = velk_cluster_words.data[header + 10u];
                } else {
                    // Its vertices move, so its triangles are tested as they are drawn.
                    drawn_at = velk_culling.data[cur_cluster];
                    drawn_left = velk_culling.data[cur_cluster + 1u];
                    drawn_vertices = velk_culling.data[cur_cluster + 18u];
                }
            } else if (kind == 255u) {
                // A whole mesh: its own tree.
                if (mesh == VELK_INVALID_MESH_STATIC) continue;
                MeshStaticData st = velk_mesh_static_records.data[mesh];
                if (st.blas_node_count == 0u || st.triangle_count == 0u) continue;
                cur_draw = draw;
                cur_cluster = VELK_NO_CLUSTER;
                walk_ray = velk_instance_ray(ray, inst, true, mesh_scale);
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
                        if (!closest) return 0.0;
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
                record_at = ray_list + first;
                records_left = (ref & 3u) + 1u;
                continue;
            }
            ++velk_ray_partition_nodes;
            uint node = nodes + ref * 16u;
            float t_l, t_r;
            bool h_l = ray_aabb(ray, velk_ray_vec3(node) - cam, velk_ray_vec3(node + 3u) - cam, best_t * 1.00001, t_l);
            bool h_r = ray_aabb(ray, velk_ray_vec3(node + 6u) - cam, velk_ray_vec3(node + 9u) - cam, best_t * 1.00001, t_r);
            bool left_first = h_l && (!h_r || t_l <= t_r);
            uint near_ref = velk_culling.data[node + (left_first ? 12u : 13u)];
            uint far_ref = velk_culling.data[node + (left_first ? 13u : 12u)];
            if (h_l && h_r && far_ref != VELK_RAY_NONE && sp < kStack) s_walk_stack[stack_base + uint(sp++)] = far_ref;
            if ((h_l || h_r) && near_ref != VELK_RAY_NONE && sp < kStack) s_walk_stack[stack_base + uint(sp++)] = near_ref;
        } else {
            if (sp == 0) break;
            uint ref = s_walk_stack[stack_base + uint(--sp)];
            if ((ref & VELK_RAY_LEAF) != 0u) {
                ++velk_ray_partitions;
                // A partition: its tree, whose root's box its leaf here already has.
                uint leaf = top_leaves + (ref & ~VELK_RAY_LEAF) * 2u;
                uint cut_entry = top_cuts + velk_culling.data[leaf] * 8u;
                uint cut = velk_culling.data[cut_entry];
                uint state = velk_culling.data[cut + VELK_CUT_STATES_WORD] + velk_culling.data[leaf + 1u] * VELK_CUT_PARTITION_WORDS;
                // What rays trace of it: its ray list, over its records.
                uint count = velk_culling.data[state + VELK_PART_RAY_COUNT];
                if (count == 0u) continue;
                uint parity = velk_culling.data[cut + VELK_CUT_PARITY];
                records = velk_culling.data[cut + VELK_CUT_STORES + parity] + velk_culling.data[state + VELK_PART_FIRST] * VELK_RECORD_END;
                ray_list = velk_culling.data[cut + VELK_CUT_RAY_STORES + parity] + velk_culling.data[state + VELK_PART_FIRST];
                nodes = velk_culling.data[cut + VELK_CUT_NODE_STORES + parity] + velk_culling.data[state + VELK_PART_NODE_FIRST] * 16u;
                instances_base = velk_culling.data[cut_entry + 1u];
                items = velk_culling.data[cut_entry + 2u];
                quads = velk_culling.data[cut_entry + 6u];
                if (velk_culling.data[cut_entry + 5u] != 0u) velk_ray_met_motion = true;
                level = kPartition;
                partition_scope = sp;
                if (sp < kStack) s_walk_stack[stack_base + uint(sp++)] = velk_ray_tree_nodes(count) - 1u;
                continue;
            }
            ++velk_ray_top_nodes;
            uint node = top_nodes + ref * 16u;
            float t_l, t_r;
            bool h_l = ray_aabb(ray, velk_ray_vec3(node) - cam, velk_ray_vec3(node + 3u) - cam, best_t * 1.00001, t_l);
            bool h_r = ray_aabb(ray, velk_ray_vec3(node + 6u) - cam, velk_ray_vec3(node + 9u) - cam, best_t * 1.00001, t_r);
            bool left_first = h_l && (!h_r || t_l <= t_r);
            uint near_ref = velk_culling.data[node + (left_first ? 12u : 13u)];
            uint far_ref = velk_culling.data[node + (left_first ? 13u : 12u)];
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
