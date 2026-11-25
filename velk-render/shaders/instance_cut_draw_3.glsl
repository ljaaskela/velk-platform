// Whether a sphere around `center`, relative to the camera, is outside the view.
bool velk_cut_outside(mat4 vp, vec3 center, float radius)
{
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
    uint cut = pc.cut_word;
    // A group a run of the records of partitions in view (instance_cut_visible_src).
    uint run = gl_WorkGroupID.x;
    if (run >= cull_words.data[cut + VELK_CUT_CAPACITY] / VELK_CUT_PAGE_RECORDS + cull_words.data[cut + VELK_CUT_PARTITIONS] + 1u) return;
    uint runs = cull_words.data[cut + VELK_CUT_VISIBLE_WORD] + run * 2u;
    uint place = cull_words.data[runs] + gl_LocalInvocationIndex;
    if (place >= cull_words.data[runs + 1u]) return;
    uint record = velk_cut_record_word(cut, place);
    uint entry = cull_words.data[record + VELK_RECORD_ENTRY];
    uint cluster_or_table = cull_words.data[record + VELK_RECORD_CLUSTER];
    uint slot = cull_words.data[record + VELK_RECORD_SLOT];

    // For rays only, or a patch for rays: not drawn.
    if (velk_cut_for_rays(entry) || entry == VELK_CUT_RAY_QUAD) return;

    // A quad: in view, it goes to the view's quad list, never past its room, its record in the
    // column after.
    if (entry == VELK_CUT_QUAD) {
        if (pc.quads_word == 0xFFFFFFFFu) return;
        uint w = cluster_or_table;
        GlobalData globals = velk_globals.data[pc.globals_base];
        vec3 center = vec3(uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER]),
                           uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER + 1u]),
                           uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER + 2u]))
                    - globals.cam_pos.xyz;
        float radius = uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_RADIUS]) + pc.quad_pad;
        if (pc.whole == 0u && velk_cut_outside(globals.view_projection, center, radius)) return;
        uint at = atomicAdd(cull_words.data[pc.quads_word + VELK_QUAD_LIST_COUNT], 1u);
        if (at < pc.quad_capacity) {
            cull_words.data[pc.quads_word + VELK_QUAD_LIST_QUADS + at] = w;
            cull_words.data[pc.quads_word + VELK_QUAD_LIST_QUADS + pc.quad_capacity + at] = record;
        } else {
            atomicAdd(cull_words.data[pc.quads_word + VELK_QUAD_LIST_COUNT], 0xFFFFFFFFu);
        }
        return;
    }

    // A record names its item: the instance, its slot and its table (kPlainItemWords). An
    // instance without a place in its raster region is not drawn.
    uint instance = pc.instances_base + slot;
    uint table_word = pc.table_word;
    if (pc.items_word != 0xFFFFFFFFu) {
        uint at = pc.items_word + slot * VELK_PLAIN_ITEM_WORDS;
        uint item = slot;
        instance = cull_words.data[at];
        slot = cull_words.data[at + VELK_PLAIN_ITEM_SLOT];
        table_word = cull_words.data[at + VELK_PLAIN_ITEM_TABLE];
        // Rays only: drawn by its own pass, whose pixels find their record by item.
        if (table_word == 0xFFFFFFFFu) {
            cull_words.data[pc.item_records_word + item] = record;
            return;
        }
        if (instance == 0xFFFFFFFFu) return;
    }
    CullInstance ci = cull_instances.data[instance];
    mat4 world = velk_sized_world(ci.world_matrix, ci.offset.xyz, ci.size.xyz);
    if (world[3][3] == 0.0) return;
    uint table = table_word + ci.params.y * VELK_TABLE_END;
    GlobalData globals = velk_globals.data[pc.globals_base];
    vec3 cam = globals.cam_pos.xyz;
    mat4 vp = globals.view_projection;
    float scale2 = max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)), dot(world[2].xyz, world[2].xyz));
    float scale = sqrt(scale2);

    // The whole instance: near enough.
    vec3 center = velk_rel_position(world, vec3(uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER]),
                                                uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER + 1u]),
                                                uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER + 2u])), globals);
    float radius = uintBitsToFloat(cull_words.data[table + VELK_TABLE_RADIUS]) * scale;
    if (pc.max_distance > 0.0 && length(center) - radius > pc.max_distance) return;
    g_instance = instance;
    g_table = table_word;

    if (entry == 0xFFFFFFFFu) {
        if (pc.whole == 0u && velk_cut_outside(vp, center, radius)) return;
        uint at = atomicAdd(cull_words.data[cluster_or_table + VELK_TABLE_COUNT], 1u);
        uint capacity = cull_words.data[cluster_or_table + VELK_TABLE_CAPACITY];
        if (at < capacity) {
            uint list = cull_words.data[cluster_or_table + VELK_TABLE_LIST];
            cull_words.data[list + at] = slot;
            cull_words.data[list + capacity + at] = record;
        } else {
            // No room: the count, which the draw's instance count is, gives it back.
            atomicAdd(cull_words.data[cluster_or_table + VELK_TABLE_COUNT], 0xFFFFFFFFu);
        }
        return;
    }

    uint c = cluster_or_table;
    uint cluster = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + c * VELK_LOD_CLUSTER_WORDS;
    // Its cluster's errors changed after its partition chose, as loading or dropping detail
    // changes them: the partition, its page's owner, chooses again on the next frame.
    uint changed = cull_words.data[cluster + VELK_LOD_CLUSTER_CHANGED];
    if (changed != 0u) {
        uint place = (record - cull_words.data[pc.cut_word + VELK_CUT_RECORD_POOL]) / VELK_RECORD_END;
        uint owner = cull_words.data[cull_words.data[pc.cut_word + VELK_CUT_PAGE_HEADERS]
                                     + (place / VELK_CUT_PAGE_RECORDS) * VELK_CUT_PAGE_WORDS + VELK_CUT_PAGE_OWNER];
        if (owner < cull_words.data[pc.cut_word + VELK_CUT_PARTITIONS]) {
            uint owner_state = velk_cut_state(pc.cut_word, owner);
            if (changed > cull_words.data[owner_state + VELK_PART_CHOSE]) cull_words.data[owner_state + VELK_PART_RETRY] = 1u;
        }
    }
    uint entry_flags = cull_words.data[entry + VELK_CLUSTER_ENTRY_FLAGS];
    // A deforming mesh's spheres are where it is at rest, which says nothing about where it is.
    bool deforming = (entry_flags & VELK_LOD_DEFORMING) != 0u;
    float own_radius = uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale;
    vec3 own_center = velk_lod_center(cluster + VELK_LOD_CLUSTER_OWN_SPHERE, world, cam);
    // Traced: what comes of a cluster the traced pixel's ray passes.
    bool traced = globals.trace != VELK_TRACE_NONE && velk_trace_sphere(globals, own_center, own_radius);
    if (!deforming && pc.whole == 0u) {
        if (velk_cut_outside(vp, own_center, own_radius)) {
            if (traced) velk_trace_note(globals, VELK_TRACE_DRAW, 1u, entry, c, record);
            return;
        }
        // Tested where it was last frame, against what was drawn then. A blended batch's own pass
        // draws only the first phase; its depth test hides what is behind.
        float test_radius = uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_SPHERE + 3u]) * scale;
        if (pc.pyramid_word != 0xFFFFFFFFu && (entry_flags & VELK_LOD_RAYS_ONLY) == 0u
            && velk_behind_pyramid(pc.pyramid_word, globals.prev_view_projection,
                                   velk_lod_center(cluster + VELK_LOD_CLUSTER_SPHERE, world, cam) + globals.cam_moved, test_radius)
            && velk_retest(entry, c, slot, record)) {
            if (traced) velk_trace_note(globals, VELK_TRACE_DRAW, 2u, entry, c, record);
            return;
        }
    }
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * globals.viewport.y * 0.5;
    // Chosen or the list full, as velk_lod_use says; else compute raster took it.
    if (traced) g_traced = 3u;
    velk_lod_take(entry, c, slot, record, world, scale, cam, cam, pixels_per_unit);
    if (traced) velk_trace_note(globals, VELK_TRACE_DRAW, g_traced, entry, c, record);
}
