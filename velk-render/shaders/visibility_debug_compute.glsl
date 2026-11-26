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
#velk_include VELK_LIGHT_POOL_GLSL
vec3 id_color(uint id)
{
    uint h = (id + 1u) * 2654435761u;
    h ^= h >> 15u;
    h *= 2246822519u;
    h ^= h >> 13u;
    return vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0;
}

// A colour per kept cut record by what it is (entry, cluster, slot), which stays from frame to
// frame, also when the record's partition chooses again and its place in the cut's pool moves.
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

// Mode 15: where light comes from. A triangle with pieces: the depth of the piece a pixel reads
// (light_color says the colours). Else a record with points: a colour per point of light, blended
// as a pixel reads them (its triangle's corners), so each patch is one point's reach; one with one
// value flat in its colour; a quad
// in the colour of the node its light is on; magenta as far as that light is not traced yet; what
// has no light yet grey, and what is from no record white.
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
        uint value = velk_culling.data[node + VELK_QUAD_NODE_LIGHT];
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
    vec3 p[3];
    vec2 size = vec2(float(pc.width), float(pc.height));
    for (int k = 0; k < 3; ++k) {
        uint at = velk_cluster_vertex_word(d, cluster) + corners[k] * VELK_VERTEX3D_WORDS;
        vec3 local = vec3(uintBitsToFloat(velk_mesh_words.data[at]), uintBitsToFloat(velk_mesh_words.data[at + 1u]),
                          uintBitsToFloat(velk_mesh_words.data[at + 2u]));
        p[k] = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g);
    }
    // Its weights there as the resolve left them for the claim and the lighting; where the
    // pixel's view ray meets the triangle's plane, for how many pixels a world unit is there.
    vec3 b = velk_pixel_weights(pc.ids_word, pc.width, coord);
    vec3 e1 = p[1] - p[0];
    vec3 e2 = p[2] - p[0];
    vec2 ndc = (vec2(coord) + 0.5) / size * 2.0 - 1.0;
    vec4 on_ray = g.inverse_view_projection * vec4(ndc, 1.0, 1.0);
    vec3 dir = on_ray.xyz / on_ray.w;
    vec3 normal = cross(e1, e2);
    float facing = dot(normal, dir);
    if (abs(facing) < 1e-20) return base;
    float d11 = dot(e1, e1);
    float d22 = dot(e2, e2);
    // In pieces: the level the pixel reads as the lighting finds it (its coarse level where that
    // level's cell is exact), coloured by its depth (red at the root, then orange,
    // yellow, green, cyan, blue, purple, pink, again from red; dark grey its triangle's vertex
    // points), darker toward its points' places so that its grid shows; magenta as far as the
    // points it reads are not traced yet, cyan as far as they are stale since a change.
    float tri_size = sqrt(max(max(d11, d22), dot(p[2] - p[1], p[2] - p[1])));
    VelkLightRoot r = velk_light_root_of(first, triangle, b, tri_size);
    if (r.slot != VELK_LIGHT_NONE) {
        const vec3 kDepthColors[8] = vec3[8](
            vec3(1.0, 0.0, 0.0), vec3(1.0, 0.5, 0.0), vec3(1.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0),
            vec3(0.0, 1.0, 1.0), vec3(0.0, 0.3, 1.0), vec3(0.6, 0.0, 1.0), vec3(1.0, 0.0, 0.7));
        uint root = velk_culling.data[velk_light_root_word(pc.pool_word, first, r.slot)];
        vec3 at = dir * (dot(normal, p[0]) / facing);
        float px_per_world = pixels_per_unit / max(length(at), 1e-4);
        float tri_px = tri_size * px_per_world;
        VelkPieceWay way = velk_piece_way(pc.pool_word, root, b, tri_px, r, r.size * px_per_world, pc.light_pixels);
        if (velk_way_cell(pc.pool_word, first, corners, way, true) == 1u) velk_way_stop_at_coarse(way);
        uint words[3];
        vec3 w;
        velk_way_around(pc.pool_word, first, corners, way.piece, way.at, words, w);
        float untraced = 0.0;
        float stale = 0.0;
        for (uint k = 0u; k < 3u; ++k) {
            if (velk_culling.data[words[k]] == VELK_LIGHT_NONE) untraced += w[k];
            else if (velk_light_point_traces(pc.pool_word, first, way.piece == VELK_PIECE_VERTICES ? VELK_LIGHT_NONE : way.piece,
                                             velk_culling.data[words[k] + 1u]) == 0u) {
                stale += w[k];
            }
        }
        vec3 shown = way.piece == VELK_PIECE_VERTICES ? vec3(0.3) : kDepthColors[way.depth & 7u];
        vec3 color = shown * (0.55 + 0.45 * max(max(w.x, w.y), w.z));
        color = mix(color, vec3(0.0, 1.0, 1.0), clamp(stale, 0.0, 1.0));
        return mix(color, vec3(1.0, 0.0, 1.0), clamp(untraced, 0.0, 1.0));
    }
    vec3 color = vec3(0.0);
    uint words[9];
    uint points[9];
    float weights[9];
    uint owners[9];
    uint read = velk_light_pixel_points(pc.pool_word, first, corners, b, words, points, weights, owners);
    float untrusted = 0.0;
    float retracing = 0.0;
    for (uint k = 0u; k < read; ++k) {
        color += weights[k] * id_color(words[k] * 2654435761u);
        uint traces = velk_light_point_traces(pc.pool_word, owners[k], VELK_LIGHT_NONE, velk_culling.data[words[k] + 1u]);
        bool none = velk_culling.data[words[k]] == VELK_LIGHT_NONE;
        float trust = none ? 0.0 : float(traces);
        untrusted += weights[k] * (1.0 - trust);
        if (!none && traces == 0u) retracing += weights[k];
    }
    color *= 0.45;
    // Magenta as far as the points it reads are not traced yet, cyan as far as they are stale
    // since a change.
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
    if (pc.mode == 8u) return kLevelColors[min(velk_culling.data[cluster + VELK_LOD_CLUSTER_LEVEL], 7u)];

    DebugInstance inst = debug_instances.data[d.instances_base + slot];
    GlobalData g = velk_globals.data[pc.chooser_globals];
    vec3 local = vec3(uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE]),
                      uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 1u]),
                      uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 2u]));
    mat3 to_world = mat3(inst.world_matrix);
    float scale = max(max(length(to_world[0] * inst.size.x), length(to_world[1] * inst.size.y)), length(to_world[2] * inst.size.z));
    float radius = uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale;
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
    vec3 local = vec3(uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE]),
                      uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 1u]),
                      uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 2u]));
    mat3 to_world = mat3(inst.world_matrix);
    float scale = max(max(length(to_world[0] * inst.size.x), length(to_world[1] * inst.size.y)), length(to_world[2] * inst.size.z));
    float radius = uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale;
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
