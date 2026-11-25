// The direct light the pixel with ids `ids` at `at` (relative to the camera) gets from the
// points of light: up to VELK_LIGHT_SLOTS lights, filled in order, the rest VELK_LIGHT_NONE, and
// how much of each gets through. False when it has no traced light yet. `b` is where the pixel
// is in its triangle (velk_pixel_weights), `tri_size` the triangle's longest edge in world
// units, `light_pixels` kLightPixels. `radiance` is the word of its record's radiance, or its
// quad node's, and `radiance_dir` that of the way its pixels face (a record's only), each
// VELK_LIGHT_NONE for none, even when it returns false.
bool velk_light_direct(uint pool, uvec2 ids, vec3 b, vec3 at, float tri_size, float light_pixels,
                       out uint lights[VELK_LIGHT_SLOTS], out float through[VELK_LIGHT_SLOTS], out uint radiance,
                       out uint radiance_dir)
{
    vec2 sums[VELK_LIGHT_SLOTS];
    for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
        lights[k] = VELK_LIGHT_NONE;
        sums[k] = vec2(0.0);
        through[k] = 0.0;
    }
    radiance = VELK_LIGHT_NONE;
    radiance_dir = VELK_LIGHT_NONE;
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint record = velk_ids_record(d, ids);
    if (record == VELK_NO_RECORD) return false;
    GlobalData g = velk_globals.data[d.globals_base];
    uint entry = VELK_LIGHT_WORDS.data[record + VELK_RECORD_ENTRY];
    // Not for a whole mesh or a shape, which no cluster bounds: one value could not hold light
    // that changes across a whole floor.
    if (entry != 0xFFFFFFFFu) {
        radiance = record + VELK_RECORD_RADIANCE;
        radiance_dir = record + VELK_RECORD_RADIANCE_DIR;
    }

    if (entry == VELK_CUT_QUAD) {
        mat4 vp = g.view_projection;
        float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
        uint node = velk_light_quad_node(VELK_LIGHT_WORDS.data[record + VELK_RECORD_QUAD_TREE], VELK_LIGHT_WORDS.data[record + VELK_RECORD_QUAD_NODE],
                                         g.cam_pos.xyz, pixels_per_unit, light_pixels);
        radiance = node + VELK_QUAD_NODE_RADIANCE;
        radiance_dir = VELK_LIGHT_NONE;
        uint value = VELK_LIGHT_WORDS.data[node + VELK_QUAD_NODE_LIGHT];
        if (value >= VELK_LIGHT_PENDING || (value & VELK_LIGHT_ONE_TRACED) == 0u) return false;
        vec3 center = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[node + VELK_QUAD_NODE_CENTER]),
                           uintBitsToFloat(VELK_LIGHT_WORDS.data[node + VELK_QUAD_NODE_CENTER + 1u]),
                           uintBitsToFloat(VELK_LIGHT_WORDS.data[node + VELK_QUAD_NODE_CENTER + 2u])) - g.cam_pos.xyz;
        velk_light_add_one(value, center, lights, sums);
    } else {
        uint value = VELK_LIGHT_WORDS.data[record + VELK_RECORD_LIGHT];
        if (value >= VELK_LIGHT_PENDING) return false;
        ElementInstance inst = velk_instances.data[d.instances_base + velk_ids_slot(d, ids)];
        if (value >= VELK_LIGHT_ONE_VALUE) {
            if ((value & VELK_LIGHT_ONE_TRACED) == 0u) return false;
            // From its cluster's sphere, or its whole mesh's, or a shape's box's middle.
            uint sphere = entry == 0xFFFFFFFFu ? VELK_LIGHT_WORDS.data[record + VELK_RECORD_MESH_TABLE]
                        : VELK_LIGHT_WORDS.data[entry + VELK_CLUSTER_ENTRY_TABLE] + VELK_LIGHT_WORDS.data[record + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS + VELK_LOD_CLUSTER_OWN_SPHERE;
            vec3 local = sphere == VELK_LIGHT_NONE ? vec3(0.5)
                       : vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere]), uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere + 1u]),
                              uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere + 2u]));
            velk_light_add_one(value, velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g), lights,
                               sums);
        } else {
            if (value >= VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES]) return false;
            uint first = velk_light_page(pool, value);
            uint cluster = velk_ids_cluster(d, ids);
            if (cluster == VELK_NO_CLUSTER) return false;
            // The triangle's corners, and how many pixels a world unit at the pixel is.
            uint word = d.index_base + velk_cluster_first_index(cluster) + velk_ids_triangle(d, ids) * 3u;
            uvec3 corners = uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
            mat4 vp = g.view_projection;
            float px_per_world = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5 / max(length(at), 1e-4);
            if (!velk_light_add_points(pool, first, velk_ids_triangle(d, ids), corners, b, at, px_per_world, tri_size, lights,
                                       sums)) {
                return false;
            }
        }
    }
    for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) through[k] = sums[k].y / max(sums[k].x, 1e-6);
    return true;
}

// For VELK_LIGHT_READ: what the pixel with ids `ids` at weights `b` (`at` relative to the camera,
// `tri_size` its triangle's longest edge) reads of its triangle's pieces, into `e`
// (VELK_LIGHT_EXPLAIN_*). VELK_LIGHT_NONE where it does not apply.
// The blocker word (velk_light_page_blocker) of the point whose two words start at word `w` of
// the pool, found by the segment it is in; VELK_LIGHT_NONE past the pool's segments.
uint velk_light_word_blocker(uint pool, uint w)
{
    uint segments = (VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES] + 16383u) >> 14u;
    for (uint s = 0u; s < min(segments, 64u); ++s) {
        uint segment = VELK_LIGHT_WORDS.data[pool + VELK_POOL_SEGMENT_WORDS + s];
        if (w >= segment && w < segment + VELK_LIGHT_SEGMENT_PAGE_WORDS) {
            return segment + VELK_LIGHT_SEGMENT_PAGE_WORDS + (w - segment) / 2u;
        }
    }
    return VELK_LIGHT_NONE;
}

void velk_light_explain(uint pool, uvec2 ids, vec3 b, vec3 at, float tri_size, out uint e[VELK_LIGHT_EXPLAIN_WORDS])
{
    for (uint k = 0u; k < VELK_LIGHT_EXPLAIN_WORDS; ++k) e[k] = VELK_LIGHT_NONE;
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint record = velk_ids_record(d, ids);
    if (record == VELK_NO_RECORD) return;
    uint value = VELK_LIGHT_WORDS.data[record + VELK_RECORD_LIGHT];
    e[VELK_LIGHT_EXPLAIN_RECORD] = value;
    if (value >= VELK_LIGHT_ONE_VALUE || value >= VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES]) return;
    uint first = velk_light_page(pool, value);
    uint cluster = velk_ids_cluster(d, ids);
    if (cluster == VELK_NO_CLUSTER) return;
    uint t = velk_ids_triangle(d, ids);
    e[VELK_LIGHT_EXPLAIN_TRIANGLE] = t;
    e[VELK_LIGHT_EXPLAIN_PIECED] = velk_light_pieced(first);
    e[VELK_LIGHT_EXPLAIN_COARSE] = VELK_LIGHT_WORDS.data[first + VELK_PAGE_COARSE];
    uint word = d.index_base + velk_cluster_first_index(cluster) + t * 3u;
    uvec3 corners = uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
    ElementInstance inst = velk_instances.data[d.instances_base + velk_ids_slot(d, ids)];
    VelkLightRoot r = velk_light_root_of(first, t, b, tri_size);
    if (r.slot == VELK_LIGHT_NONE) return;
    GlobalData g = velk_globals.data[d.globals_base];
    mat4 vp = g.view_projection;
    float px_per_world = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5 / max(length(at), 1e-4);
    uint root = VELK_LIGHT_WORDS.data[velk_light_root_word(pool, first, r.slot)];
    e[VELK_LIGHT_EXPLAIN_ROOT] = root;
    VelkPieceWay way = velk_piece_way(pool, root, b, tri_size * px_per_world, r, r.size * px_per_world, VELK_LIGHT_PIXELS);
    uint cell = velk_way_cell(pool, first, corners, way, true);
    e[VELK_LIGHT_EXPLAIN_DEEPEST] = way.piece == VELK_PIECE_VERTICES ? VELK_LIGHT_NONE : way.depth;
    if (cell == 1u) velk_way_stop_at_coarse(way);
    e[VELK_LIGHT_EXPLAIN_PIECE] = way.piece;
    e[VELK_LIGHT_EXPLAIN_WAY] = way.depth | (cell << 8u) | (uint(clamp(tri_size * px_per_world, 0.0, 65535.0)) << 16u);
    // Its vertex points, then the level it reads: each of the three points around it, its light,
    // its traces and its weight in 255ths.
    for (uint level = 0u; level < 2u; ++level) {
        uint page = level == 0u ? VELK_PIECE_VERTICES : way.piece;
        uint words[3];
        vec3 w;
        velk_way_around(pool, first, corners, page, level == 0u ? b : way.at, words, w);
        uint base = level == 0u ? VELK_LIGHT_EXPLAIN_VERTICES : VELK_LIGHT_EXPLAIN_LEVEL;
        uint weights = 0u;
        for (uint k = 0u; k < 3u; ++k) {
            e[base + k] = VELK_LIGHT_WORDS.data[words[k]];
            e[base + 3u + k] = velk_light_point_traces(pool, first, page == VELK_PIECE_VERTICES ? VELK_LIGHT_NONE : page,
                                                       VELK_LIGHT_WORDS.data[words[k] + 1u]);
            // And from bit 8, its first slot's blocker code (VELK_LIGHT_BLOCKER_*).
            uint bw = velk_light_word_blocker(pool, words[k]);
            if (bw != VELK_LIGHT_NONE) e[base + 3u + k] |= (VELK_LIGHT_WORDS.data[bw] & 15u) << 8u;
            weights |= uint(clamp(w[k], 0.0, 1.0) * 255.0 + 0.5) << (8u * k);
            uint state = VELK_LIGHT_WORDS.data[words[k] + 1u];
            e[VELK_LIGHT_EXPLAIN_LISTED + level * 3u + k] = state == VELK_LIGHT_NONE ? VELK_LIGHT_NONE : state & VELK_PIECE_FRAME_BITS;
        }
        e[base + 6u] = weights;
        e[VELK_LIGHT_EXPLAIN_CHANGED + level] = page == VELK_PIECE_VERTICES ? VELK_LIGHT_WORDS.data[first + VELK_PAGE_CHANGED]
                                                                             : VELK_LIGHT_WORDS.data[velk_piece_changed_word(pool, page)];
    }
    e[VELK_LIGHT_EXPLAIN_FRAME] = VELK_LIGHT_WORDS.data[pool + VELK_POOL_FRAME] & VELK_PIECE_FRAME_BITS;
    // Its corners and the pixel in the world, and the way its first light shines.
    for (uint k = 0u; k < 3u; ++k) {
        uint v = velk_cluster_vertex_word(d, cluster) + corners[k] * VELK_VERTEX3D_WORDS;
        vec3 local = vec3(uintBitsToFloat(velk_mesh_words.data[v]), uintBitsToFloat(velk_mesh_words.data[v + 1u]),
                          uintBitsToFloat(velk_mesh_words.data[v + 2u]));
        vec3 p = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g) + g.cam_pos.xyz;
        for (uint c = 0u; c < 3u; ++c) e[VELK_LIGHT_EXPLAIN_CORNERS + 3u * k + c] = floatBitsToUint(p[c]);
    }
    for (uint c = 0u; c < 3u; ++c) e[VELK_LIGHT_EXPLAIN_AT + c] = floatBitsToUint(at[c] + g.cam_pos[c]);
    uint slot = VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS];
    if (slot != VELK_LIGHT_NONE) {
        vec3 dir = velk_light_at(pc.lights_base + slot).direction.xyz;
        for (uint c = 0u; c < 3u; ++c) e[VELK_LIGHT_EXPLAIN_LIGHT + c] = floatBitsToUint(dir[c]);
    }
}
