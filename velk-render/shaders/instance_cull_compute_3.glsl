// Tested where it was last frame, against what was drawn then.
bool velk_lod_occluded(uint word, mat4 world, float scale, vec3 cam, uint entry, uint cluster, uint slot)
{
    if (pc.pyramid_word == 0xFFFFFFFFu) return false;
    GlobalData g = velk_globals.data[pc.globals_base];
    float radius = uintBitsToFloat(cull_words.data[word + 3u]) * scale;
    return velk_behind_pyramid(pc.pyramid_word, g.prev_view_projection, velk_lod_center(word, world, cam) + g.cam_moved, radius)
        && velk_retest(entry, cluster, slot, 0xFFFFFFFFu);
}

// Drawn unless hidden; a deforming mesh's spheres are where it is at rest, which says nothing
// about whether it is hidden. Chosen every frame, so from no kept cut record.
void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     bool deforming)
{
    // Clusters for rays only: their batch draws itself.
    if ((cull_words.data[entry + VELK_CLUSTER_ENTRY_FLAGS] & VELK_LOD_RAYS_ONLY) != 0u) return;
    uint cluster = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + c * VELK_LOD_CLUSTER_WORDS;
    // Traced: chosen (0) or held back by occlusion (2), as the kept cut's draw notes it, with no
    // record.
    GlobalData g = velk_globals.data[pc.globals_base];
    bool traced = g.trace != VELK_TRACE_NONE
               && velk_trace_sphere(g, velk_lod_center(cluster + VELK_LOD_CLUSTER_OWN_SPHERE, world, g.cam_pos.xyz),
                                    uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale);
    if (deforming || !velk_lod_occluded(cluster + VELK_LOD_CLUSTER_SPHERE, world, scale, cam, entry, c, slot)) {
        velk_lod_take(entry, c, slot, 0xFFFFFFFFu, world, scale, cam, lod_cam, pixels_per_unit);
        if (traced) velk_trace_note(g, VELK_TRACE_DRAW, 0u, entry, c, 0xFFFFFFFFu);
    } else if (traced) {
        velk_trace_note(g, VELK_TRACE_DRAW, 2u, entry, c, 0xFFFFFFFFu);
    }
}

void main()
{
    uint slot = gl_GlobalInvocationID.x;
    GlobalData globals = velk_globals.data[pc.globals_base];
    g_lod_cam = globals.cam_pos.xyz;
    if (slot >= pc.slot_count) return;

    uint instance = pc.instances_base + slot;
    uint table_word = pc.table_word;
    uint item_index = slot;
    if (pc.items_word != 0xFFFFFFFFu) {
        uint item = pc.items_word + item_index * VELK_PLAIN_ITEM_WORDS;
        instance = cull_words.data[item];
        slot = cull_words.data[item + VELK_PLAIN_ITEM_SLOT];
        table_word = cull_words.data[item + VELK_PLAIN_ITEM_TABLE];
        // Rays only: drawn by its own pass. Or an instance without a place in its raster region.
        if (table_word == 0xFFFFFFFFu || instance == 0xFFFFFFFFu) return;
    }

    CullInstance ci = cull_instances.data[instance];
    mat4 world = velk_sized_world(ci.world_matrix, ci.offset.xyz, ci.size.xyz);
    if (world[3][3] == 0.0) return;

    uint table = table_word + ci.params.y * VELK_TABLE_END;
    vec4 sphere = vec4(uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER]),
                       uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER + 1u]),
                       uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER + 2u]),
                       uintBitsToFloat(cull_words.data[table + VELK_TABLE_RADIUS]));
    vec3 center = velk_rel_position(world, sphere.xyz, globals);
    float scale2 = max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)),
                       dot(world[2].xyz, world[2].xyz));
    float radius = sphere.w * sqrt(scale2);
    uint first_entry = cull_words.data[table + VELK_TABLE_FIRST_ENTRY];
    uint entries = cull_words.data[table + VELK_TABLE_ENTRY_COUNT];
    // A deforming mesh's sphere is where it was at rest, which says nothing about whether it
    // is in view or hidden.
    bool deforming = entries != 0u && (cull_words.data[first_entry + VELK_CLUSTER_ENTRY_FLAGS] & VELK_LOD_DEFORMING) != 0u;

    // Traced: an instance the traced pixel's ray passes, culled every frame (kTraceInstance).
    bool traced = globals.trace != VELK_TRACE_NONE && velk_trace_sphere(globals, center, radius);
    if (!deforming && pc.max_distance > 0.0 && length(center) - radius > pc.max_distance) {
        if (traced) velk_trace_note(globals, 8u, 1u, first_entry, slot, entries);
        return;
    }

    // The sphere against each clip plane. Planes are rows of the matrix added to or
    // taken from its last row, so their normals' lengths are at most the sum of the
    // rows' lengths, which keeps this conservative. Near and far are tested with the
    // wider of the two conventions for clip z.
    mat4 vp = globals.view_projection;
    vec4 clip = vp * vec4(center, 1.0);
    vec3 r0 = vec3(vp[0][0], vp[1][0], vp[2][0]);
    vec3 r1 = vec3(vp[0][1], vp[1][1], vp[2][1]);
    vec3 r2 = vec3(vp[0][2], vp[1][2], vp[2][2]);
    vec3 r3 = vec3(vp[0][3], vp[1][3], vp[2][3]);
    float w_reach = radius * length(r3);
    if (!deforming) {
        if (abs(clip.x) > clip.w + w_reach + radius * length(r0) || abs(clip.y) > clip.w + w_reach + radius * length(r1)
            || abs(clip.z) > clip.w + w_reach + radius * length(r2)) {
            if (traced) velk_trace_note(globals, 8u, 2u, first_entry, slot, entries);
            return;
        }
    }

    g_instance = instance;
    g_table = table_word;
    if (!deforming && pc.pyramid_word != 0xFFFFFFFFu
        && velk_behind_pyramid(pc.pyramid_word, globals.prev_view_projection, center + globals.cam_moved, radius)
        && velk_retest(0xFFFFFFFFu, 0u, slot, 0xFFFFFFFFu)) {
        if (traced) velk_trace_note(globals, 8u, 3u, first_entry, slot, entries);
        return;
    }
    if (traced) velk_trace_note(globals, 8u, 0u, first_entry, slot, entries);

    if (pc.items_word == 0xFFFFFFFFu) {
        uint index = atomicAdd(cull_words.data[table + VELK_TABLE_COUNT], 1u);
        uint capacity = cull_words.data[table + VELK_TABLE_CAPACITY];
        if (index < capacity) {
            uint list = cull_words.data[table + VELK_TABLE_LIST];
            cull_words.data[list + index] = slot;
            cull_words.data[list + capacity + index] = 0xFFFFFFFFu;
        } else {
            // No room: the count, which the draw's instance count is, gives it back.
            atomicAdd(cull_words.data[table + VELK_TABLE_COUNT], 0xFFFFFFFFu);
        }
    }

    for (uint e = 0u; e < entries; ++e) {
        velk_lod_visit(first_entry + e * VELK_CLUSTER_ENTRY_WORDS, slot, world, sqrt(scale2), globals.cam_pos.xyz, g_lod_cam, vp,
                       globals.viewport.y, pc.lod_max_pixels);
    }
}
