void velk_cut_record_box(uint record, uint instances_base, uint items_word, float pad, out vec3 lo, out vec3 hi)
{
    lo = vec3(1e30);
    hi = vec3(-1e30);
    uint entry = cull_words.data[record + VELK_RECORD_ENTRY]
               & ~(velk_cut_for_rays(cull_words.data[record + VELK_RECORD_ENTRY]) ? VELK_CUT_FOR_RAYS : 0u);
    uint cluster = cull_words.data[record + VELK_RECORD_CLUSTER];
    uint slot = cull_words.data[record + VELK_RECORD_SLOT];
    if (entry == VELK_CUT_RAY_QUAD) {
        // A patch: its node's sphere, which holds all its members, in the world already.
        vec3 c = vec3(uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_CENTER]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_CENTER + 1u]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_CENTER + 2u]));
        float r = max(uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_RADIUS]), 0.0) + pad;
        lo = c - vec3(r);
        hi = c + vec3(r);
        return;
    }
    if (entry == VELK_CUT_QUAD) {
        // A quad: around its corners, in the world already.
        vec3 c = vec3(uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_CENTER]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_CENTER + 1u]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_CENTER + 2u]));
        vec3 u = vec3(uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_U]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_U + 1u]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_U + 2u]));
        vec3 v = vec3(uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_V]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_V + 1u]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_V + 2u]));
        vec3 reach = abs(u) + abs(v) + vec3(pad);
        lo = c - reach;
        hi = c + reach;
        return;
    }
    uint instance = instances_base + slot;
    float motion = 0.0;
    uint rest = 0xFFFFFFFFu;
    if (items_word != 0xFFFFFFFFu) {
        uint item = items_word + slot * VELK_PLAIN_ITEM_WORDS;
        instance = cull_words.data[item];
        motion = uintBitsToFloat(cull_words.data[item + VELK_PLAIN_ITEM_MOTION]);
        rest = cull_words.data[item + VELK_PLAIN_ITEM_REST];
    }
    if (instance == 0xFFFFFFFFu) return;
    mat4 world = cull_instances.data[instance].world_matrix;
    if (world[3][3] == 0.0) return;
    if (motion > 0.0) {
        // A motion function moves no point further than `motion` from where it rests: around its
        // resting pose by that much, every pose it can take is held; without one, around the pose
        // it has now by twice that.
        bool rested = false;
        if (rest != 0xFFFFFFFFu) {
            vec4 r0 = vec4(uintBitsToFloat(cull_words.data[rest]), uintBitsToFloat(cull_words.data[rest + 1u]),
                           uintBitsToFloat(cull_words.data[rest + 2u]), uintBitsToFloat(cull_words.data[rest + 3u]));
            vec4 r1 = vec4(uintBitsToFloat(cull_words.data[rest + 4u]), uintBitsToFloat(cull_words.data[rest + 5u]),
                           uintBitsToFloat(cull_words.data[rest + 6u]), uintBitsToFloat(cull_words.data[rest + 7u]));
            vec4 r2 = vec4(uintBitsToFloat(cull_words.data[rest + 8u]), uintBitsToFloat(cull_words.data[rest + 9u]),
                           uintBitsToFloat(cull_words.data[rest + 10u]), uintBitsToFloat(cull_words.data[rest + 11u]));
            // A slot not placed yet rests nowhere, as the motion pass finds it.
            if (r0.xyz != vec3(0.0) || r1.xyz != vec3(0.0) || r2.xyz != vec3(0.0)) {
                world = transpose(mat4(r0, r1, r2, vec4(0.0, 0.0, 0.0, 1.0)));
                rested = true;
            }
        }
        pad += rested ? motion : 2.0 * motion;
    }
    if (entry == 0xFFFFFFFFu && cluster == 0xFFFFFFFFu) {
        // An item rays see that its own pass draws: its instance's box.
        vec3 offset = cull_instances.data[instance].offset.xyz;
        vec3 size = cull_instances.data[instance].size.xyz;
        for (uint k = 0u; k < 8u; ++k) {
            vec3 corner = offset + size * vec3(float(k & 1u), float((k >> 1u) & 1u), float((k >> 2u) & 1u));
            vec3 at = mat3(world) * corner + world[3].xyz;
            lo = min(lo, at);
            hi = max(hi, at);
        }
        lo -= vec3(pad);
        hi += vec3(pad);
        return;
    }
    world = velk_sized_world(world, cull_instances.data[instance].offset.xyz, cull_instances.data[instance].size.xyz);
    // A cluster that does not deform: the box of its triangle tree's root, as the walk finds the
    // tree (its ray header), which holds every triangle rays test of it.
    if (entry != 0xFFFFFFFFu && (cull_words.data[entry + VELK_CLUSTER_ENTRY_FLAGS] & VELK_LOD_DEFORMING) == 0u) {
        uint rays = cull_words.data[entry + VELK_CLUSTER_ENTRY_RAY];
        uint root = rays != 0xFFFFFFFFu ? cluster_words.data[rays + cluster * 20u + 10u] : 0xFFFFFFFFu;
        if (root != 0xFFFFFFFFu && (root & (VELK_BLAS_LEAF | VELK_BLAS_CLUSTER | VELK_BLAS_UPPER)) == 0u) {
            uint node = (cluster_words.data[rays + cluster * 20u + 13u] + root) * 16u;
            vec3 llo = vec3(uintBitsToFloat(lod_nodes.data[node]), uintBitsToFloat(lod_nodes.data[node + 1u]),
                            uintBitsToFloat(lod_nodes.data[node + 2u]));
            vec3 lhi = vec3(uintBitsToFloat(lod_nodes.data[node + 3u]), uintBitsToFloat(lod_nodes.data[node + 4u]),
                            uintBitsToFloat(lod_nodes.data[node + 5u]));
            vec3 rlo = vec3(uintBitsToFloat(lod_nodes.data[node + 6u]), uintBitsToFloat(lod_nodes.data[node + 7u]),
                            uintBitsToFloat(lod_nodes.data[node + 8u]));
            vec3 rhi = vec3(uintBitsToFloat(lod_nodes.data[node + 9u]), uintBitsToFloat(lod_nodes.data[node + 10u]),
                            uintBitsToFloat(lod_nodes.data[node + 11u]));
            bool has_l = all(lessThanEqual(llo, lhi));
            bool has_r = all(lessThanEqual(rlo, rhi));
            if (has_l || has_r) {
                vec3 blo = has_l && has_r ? min(llo, rlo) : has_l ? llo : rlo;
                vec3 bhi = has_l && has_r ? max(lhi, rhi) : has_l ? lhi : rhi;
                // Into the world, the box around the box (Arvo).
                vec3 c = mat3(world) * ((blo + bhi) * 0.5) + world[3].xyz;
                vec3 e = (bhi - blo) * 0.5;
                vec3 reach = abs(world[0].xyz) * e.x + abs(world[1].xyz) * e.y + abs(world[2].xyz) * e.z + vec3(pad);
                lo = c - reach;
                hi = c + reach;
                return;
            }
        }
    }
    // Otherwise its own sphere, or a whole mesh's: its table's, in the mesh's own coordinates.
    uint sphere = entry != 0xFFFFFFFFu ? cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + cluster * VELK_LOD_CLUSTER_WORDS + VELK_LOD_CLUSTER_OWN_SPHERE : cluster;
    float scale = sqrt(max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)),
                           dot(world[2].xyz, world[2].xyz)));
    vec3 center = mat3(world) * vec3(uintBitsToFloat(cull_words.data[sphere]), uintBitsToFloat(cull_words.data[sphere + 1u]),
                                     uintBitsToFloat(cull_words.data[sphere + 2u]))
                + world[3].xyz;
    float radius = uintBitsToFloat(cull_words.data[sphere + 3u]) * scale + pad;
    lo = center - radius;
    hi = center + radius;
}
