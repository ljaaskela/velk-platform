// The box around the leaves' centres: its min, then its max, as velk_float_order keeps them.
shared uint s_box[6];
// How many leaves have records for rays, which the sort puts first.
shared uint s_traced;
// Whether the tree as built no longer fits its leaves, and how many leaves its nodes name.
shared uint s_changed;
shared uint s_named;

// A float as a uint that orders as the float does, for atomicMin and atomicMax.
uint velk_float_order(float f)
{
    uint b = floatBitsToUint(f);
    return (b & 0x80000000u) != 0u ? ~b : b | 0x80000000u;
}

float velk_float_unorder(uint u)
{
    return uintBitsToFloat((u & 0x80000000u) != 0u ? u & 0x7FFFFFFFu : ~u);
}

vec3 velk_load_vec3(uint at)
{
    return vec3(uintBitsToFloat(cull_words.data[at]), uintBitsToFloat(cull_words.data[at + 1u]),
                uintBitsToFloat(cull_words.data[at + 2u]));
}

// The box of leaf `leaf` of the top tree whose leaves start at `leaf_word`: its partition's
// root's, empty for one without records for rays. False for that.
bool velk_leaf_box(uint top, uint leaf_word, uint leaf, out vec3 lo, out vec3 hi)
{
    lo = vec3(1e30);
    hi = vec3(-1e30);
    uint at = leaf_word + leaf * VELK_RAY_TOP_LEAF_WORDS;
    uint cut = cull_words.data[top + VELK_RAY_TOP_HEADER_WORDS + cull_words.data[at + VELK_RAY_TOP_LEAF_CUT] * VELK_RAY_TOP_CUT_WORDS
                               + VELK_RAY_TOP_CUT_CUT];
    uint state = velk_cut_state(cut, cull_words.data[at + VELK_RAY_TOP_LEAF_PARTITION]);
    if (cull_words.data[state + VELK_PART_RAY_COUNT] == 0u) return false;
    uint root = velk_cut_node_word(cut, cull_words.data[state + VELK_PART_ROOT]);
    lo = min(velk_load_vec3(root), velk_load_vec3(root + 6u));
    hi = max(velk_load_vec3(root + 3u), velk_load_vec3(root + 9u));
    return true;
}

void velk_store_box(uint at, vec3 lo, vec3 hi)
{
    cull_words.data[at] = floatBitsToUint(lo.x);
    cull_words.data[at + 1u] = floatBitsToUint(lo.y);
    cull_words.data[at + 2u] = floatBitsToUint(lo.z);
    cull_words.data[at + 3u] = floatBitsToUint(hi.x);
    cull_words.data[at + 4u] = floatBitsToUint(hi.y);
    cull_words.data[at + 5u] = floatBitsToUint(hi.z);
}

// The top tree being built: its word and its leaves' and nodes', and whether its leaves are
// sorted.
uint g_top = 0u;
uint g_leaf_word = 0u;
uint g_nodes = 0u;
bool g_sorted = false;

// The top tree's nodes are together, named by their index.
uint velk_lbvh_node_word(uint i)
{
    return g_nodes + i * VELK_CUT_NODE_WORDS;
}

uint velk_lbvh_node_ref(uint i)
{
    return i;
}

uint velk_lbvh_ref_word(uint ref)
{
    return g_nodes + ref * VELK_CUT_NODE_WORDS;
}

uint velk_lbvh_key(uint g)
{
    return s_order[g] >> kSortIndexBits;
}

// Leaf g in order: its partition's leaf, none for one without records for rays.
uint velk_lbvh_leaf_ref(uint g)
{
    uint leaf = g_sorted ? s_order[g] & kSortIndexMask : g;
    vec3 lo, hi;
    return velk_leaf_box(g_top, g_leaf_word, leaf, lo, hi) ? 0x80000000u | leaf : 0xFFFFFFFFu;
}

void velk_lbvh_leaf_box(uint ref, out vec3 lo, out vec3 hi)
{
    velk_leaf_box(g_top, g_leaf_word, ref & 0x7FFFFFFFu, lo, hi);
}

// Whether leaf `leaf` of the top tree whose leaves start at `leaf_word` has records for rays.
bool velk_leaf_traced(uint top, uint leaf_word, uint leaf)
{
    uint at = leaf_word + leaf * VELK_RAY_TOP_LEAF_WORDS;
    uint cut = cull_words.data[top + VELK_RAY_TOP_HEADER_WORDS + cull_words.data[at + VELK_RAY_TOP_LEAF_CUT] * VELK_RAY_TOP_CUT_WORDS
                               + VELK_RAY_TOP_CUT_CUT];
    return cull_words.data[velk_cut_state(cut, cull_words.data[at + VELK_RAY_TOP_LEAF_PARTITION]) + VELK_PART_RAY_COUNT] != 0u;
}

// Whether the tree built before still fits its leaves: it was built, no partition of its cuts
// chose again this frame (a partition's tree, and so its root's box, changes only then), and its
// nodes name exactly the leaves with records for rays. All threads of the group call it.
bool velk_top_unchanged(uint top, uint cuts, uint leaf_word, uint leaves, uint nodes, uint node_count, uint lane)
{
    if (lane == 0u) {
        s_changed = cull_words.data[top + VELK_RAY_TOP_BUILT] == 0u ? 1u : 0u;
        s_named = 0u;
        s_traced = 0u;
    }
    memoryBarrierShared();
    barrier();
    for (uint c = lane; c < cuts; c += 256u) {
        uint cut = cull_words.data[top + VELK_RAY_TOP_HEADER_WORDS + c * VELK_RAY_TOP_CUT_WORDS + VELK_RAY_TOP_CUT_CUT];
        if (cull_words.data[cut + VELK_CUT_CHOOSE_GROUPS] != 0u) atomicOr(s_changed, 1u);
    }
    for (uint i = lane; i < leaves; i += 256u) {
        if (velk_leaf_traced(top, leaf_word, i)) atomicAdd(s_traced, 1u);
    }
    for (uint i = lane; i < node_count; i += 256u) {
        for (uint k = 0u; k < 2u; ++k) {
            uint ref = cull_words.data[nodes + i * VELK_CUT_NODE_WORDS + VELK_CUT_NODE_LEFT_REF + k];
            if (ref == 0xFFFFFFFFu || (ref & 0x80000000u) == 0u) continue;
            uint leaf = ref & 0x7FFFFFFFu;
            if (leaf >= leaves || !velk_leaf_traced(top, leaf_word, leaf)) atomicOr(s_changed, 1u);
            else atomicAdd(s_named, 1u);
        }
    }
    memoryBarrierShared();
    barrier();
    bool unchanged = s_changed == 0u && s_named == s_traced;
    barrier();
    return unchanged;
}

void main()
{
    uint top = pc.top_word;
    uint cuts = cull_words.data[top + VELK_RAY_TOP_CUTS];
    uint leaves = cull_words.data[top + VELK_RAY_TOP_LEAVES];
    if (leaves == 0u) return;
    uint leaf_word = top + VELK_RAY_TOP_HEADER_WORDS + cuts * VELK_RAY_TOP_CUT_WORDS;
    uint nodes = leaf_word + leaves * 2u;
    uint lane = gl_LocalInvocationIndex;
    uint node_count = leaves > 1u ? leaves - 1u : 1u;
    // Built before over the same partition trees: kept as it is.
    if (velk_top_unchanged(top, cuts, leaf_word, leaves, nodes, node_count, lane)) return;

    // The leaves in Morton order of their boxes' centres within the box around them all, so
    // that partitions near each other share nodes; those at least a quarter as wide as that box
    // after them, together, so that they widen as few nodes as they can; those without records
    // for rays last. Past kSortMax leaves, in their order.
    bool sorted = leaves <= kSortMax;
    if (sorted) {
        if (lane < 6u) s_box[lane] = lane < 3u ? 0xFFFFFFFFu : 0u;
        if (lane == 0u) s_traced = 0u;
        memoryBarrierShared();
        barrier();
        for (uint i = lane; i < leaves; i += 256u) {
            vec3 blo, bhi;
            if (!velk_leaf_box(top, leaf_word, i, blo, bhi)) continue;
            vec3 c = (blo + bhi) * 0.5;
            for (uint k = 0u; k < 3u; ++k) {
                atomicMin(s_box[k], velk_float_order(c[k]));
                atomicMax(s_box[3u + k], velk_float_order(c[k]));
            }
        }
        memoryBarrierShared();
        barrier();
        vec3 lo = vec3(velk_float_unorder(s_box[0]), velk_float_unorder(s_box[1]), velk_float_unorder(s_box[2]));
        vec3 hi = vec3(velk_float_unorder(s_box[3]), velk_float_unorder(s_box[4]), velk_float_unorder(s_box[5]));
        float extent = max(max(hi.x - lo.x, hi.y - lo.y), hi.z - lo.z);
        uint n = 1u;
        while (n < leaves) n <<= 1u;
        for (uint i = lane; i < n; i += 256u) {
            uint key = 0xFFFFFFFFu;
            if (i < leaves) {
                vec3 blo, bhi;
                // 20 bits of Morton code under a bit for the wide ones; one without records last.
                uint code = 0x1FFFFFu;
                if (velk_leaf_box(top, leaf_word, i, blo, bhi)) {
                    bool wide = length(bhi - blo) * 0.5 >= 0.25 * extent;
                    code = ((wide ? 1u : 0u) << 20u) | min(velk_morton((blo + bhi) * 0.5, lo, hi) >> 10u, 0xFFFFEu);
                    atomicAdd(s_traced, 1u);
                }
                key = (code << kSortIndexBits) | i;
            }
            s_order[i] = key;
        }
        memoryBarrierShared();
        barrier();
        velk_sort_keys(n, lane, 256u);
    }

    // Sorted, only the leaves with records for rays, which come first; else all of them in a
    // balanced tree, those without records not there for the walk (no reference).
    g_top = top;
    g_leaf_word = leaf_word;
    g_nodes = nodes;
    g_sorted = sorted;
    velk_lbvh_build_bounded(node_count, sorted ? s_traced : leaves, lane, 256u, !sorted);
    if (lane == 0u) cull_words.data[top + VELK_RAY_TOP_BUILT] = 1u;
}
