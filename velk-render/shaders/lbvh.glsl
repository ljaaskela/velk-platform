bool g_lbvh_balanced = false;
uint velk_lbvh_key(uint g);
uint velk_lbvh_leaf_ref(uint g);
void velk_lbvh_leaf_box(uint ref, out vec3 lo, out vec3 hi);
uint velk_lbvh_node_word(uint i);
uint velk_lbvh_node_ref(uint i);
uint velk_lbvh_ref_word(uint ref);
void velk_store_box(uint at, vec3 lo, vec3 hi);
vec3 velk_load_vec3(uint at);

// Whether a reference names a node, not a leaf or nothing.
bool velk_lbvh_is_node(uint ref)
{
    return ref != 0xFFFFFFFFu && (ref & 0x80000000u) == 0u;
}

// How many leading bits the keys of leaves i and j of n share; -1 past either end. Keys alike
// go on by their places.
int velk_lbvh_delta(int i, int j, int n)
{
    if (j < 0 || j >= n) return -1;
    uint a = g_lbvh_balanced ? uint(i) : velk_lbvh_key(uint(i));
    uint b = g_lbvh_balanced ? uint(j) : velk_lbvh_key(uint(j));
    if (a != b) return 31 - findMSB(a ^ b);
    return 63 - findMSB(uint(i ^ j));
}

// The children of internal node i (0 the root) of the radix tree over n leaves, n at least 2:
// a leaf as 0x80000000 and its place, else an internal node.
void velk_lbvh_children(int i, int n, out uint left, out uint right)
{
    int d = velk_lbvh_delta(i, i + 1, n) - velk_lbvh_delta(i, i - 1, n) >= 0 ? 1 : -1;
    // The other end of its range: past it the shared prefix is no longer than toward i - d.
    int dmin = velk_lbvh_delta(i, i - d, n);
    int lmax = 2;
    for (uint k = 0u; k < 30u && velk_lbvh_delta(i, i + lmax * d, n) > dmin; ++k) lmax *= 2;
    int l = 0;
    for (int t = lmax / 2; t >= 1; t /= 2) {
        if (velk_lbvh_delta(i, i + (l + t) * d, n) > dmin) l += t;
    }
    int j = i + l * d;
    // Where the range splits: the last leaf that shares more than the range does with i.
    int dnode = velk_lbvh_delta(i, j, n);
    int s = 0;
    int div = 2;
    for (uint k = 0u; k < 32u; ++k) {
        int t = (l + div - 1) / div;
        if (velk_lbvh_delta(i, i + (s + t) * d, n) > dnode) s += t;
        if (t <= 1) break;
        div *= 2;
    }
    int split = i + s * d + min(d, 0);
    left = min(i, j) == split ? 0x80000000u | uint(split) : uint(split);
    right = max(i, j) == split + 1 ? 0x80000000u | uint(split + 1) : uint(split + 1);
}

// Builds the radix tree over the `n` leaves into the tree's `node_count` nodes
// (velk_lbvh_node_word, kCutNodeWords each): internal node i at node_count - 1 - i, the root
// last, the nodes before the tree's cleared; then each node's boxes from its children's, from
// the bottom up, a round a level, and its height (VELK_CUT_NODE_LEVEL, 1 over leaves only).
// Returns the root's height, 0 when it did not finish. All `lanes` threads of the work group
// call it.
uint velk_lbvh_build(uint node_count, uint n, uint lane, uint lanes)
{
    uint internal = n > 1u ? n - 1u : 1u;
    for (uint s = lane; s + internal < node_count; s += lanes) {
        uint node = velk_lbvh_node_word(s);
        velk_store_box(node + VELK_CUT_NODE_LEFT_MIN, vec3(1e30), vec3(-1e30));
        velk_store_box(node + VELK_CUT_NODE_RIGHT_MIN, vec3(1e30), vec3(-1e30));
        cull_words.data[node + VELK_CUT_NODE_LEFT_REF] = 0xFFFFFFFFu;
        cull_words.data[node + VELK_CUT_NODE_RIGHT_REF] = 0xFFFFFFFFu;
        cull_words.data[node + VELK_CUT_NODE_LEVEL] = 0u;
        cull_words.data[node + VELK_CUT_NODE_ROOT] = 0u;
    }
    // Each internal node's children. Until the boxes are done, ROOT holds the round a node was
    // done in, plus one.
    for (uint i = lane; i < internal; i += lanes) {
        uint node = velk_lbvh_node_word(node_count - 1u - i);
        uint left = 0xFFFFFFFFu;
        uint right = 0xFFFFFFFFu;
        if (n >= 2u) {
            uint l;
            uint r;
            velk_lbvh_children(int(i), int(n), l, r);
            left = (l & 0x80000000u) != 0u ? velk_lbvh_leaf_ref(l & 0x7FFFFFFFu) : velk_lbvh_node_ref(node_count - 1u - l);
            right = (r & 0x80000000u) != 0u ? velk_lbvh_leaf_ref(r & 0x7FFFFFFFu) : velk_lbvh_node_ref(node_count - 1u - r);
        } else if (n == 1u) {
            left = velk_lbvh_leaf_ref(0u);
        }
        cull_words.data[node + VELK_CUT_NODE_LEFT_REF] = left;
        cull_words.data[node + VELK_CUT_NODE_RIGHT_REF] = right;
        cull_words.data[node + VELK_CUT_NODE_LEVEL] = 0u;
        cull_words.data[node + VELK_CUT_NODE_ROOT] = 0u;
    }
    memoryBarrierBuffer();
    barrier();
    // A node is done once its child nodes were done in an earlier round, whose boxes the
    // barrier since has made seen. The keys all differ, so the tree is at most 64 deep.
    uint root = velk_lbvh_node_word(node_count - 1u);
    for (uint round = 1u; round <= 72u; ++round) {
        for (uint i = lane; i < internal; i += lanes) {
            uint node = velk_lbvh_node_word(node_count - 1u - i);
            if (cull_words.data[node + VELK_CUT_NODE_ROOT] != 0u) continue;
            uint height = 0u;
            bool ready = true;
            for (uint k = 0u; k < 2u; ++k) {
                uint ref = cull_words.data[node + VELK_CUT_NODE_LEFT_REF + k];
                if (!velk_lbvh_is_node(ref)) continue;
                uint below = velk_lbvh_ref_word(ref);
                uint done = cull_words.data[below + VELK_CUT_NODE_ROOT];
                if (done == 0u || done >= round) ready = false;
                height = max(height, cull_words.data[below + VELK_CUT_NODE_LEVEL]);
            }
            if (!ready) continue;
            for (uint k = 0u; k < 2u; ++k) {
                uint ref = cull_words.data[node + VELK_CUT_NODE_LEFT_REF + k];
                vec3 lo = vec3(1e30);
                vec3 hi = vec3(-1e30);
                if (ref == 0xFFFFFFFFu) {
                } else if ((ref & 0x80000000u) != 0u) {
                    velk_lbvh_leaf_box(ref, lo, hi);
                } else {
                    uint below = velk_lbvh_ref_word(ref);
                    lo = min(velk_load_vec3(below + VELK_CUT_NODE_LEFT_MIN), velk_load_vec3(below + VELK_CUT_NODE_RIGHT_MIN));
                    hi = max(velk_load_vec3(below + VELK_CUT_NODE_LEFT_MAX), velk_load_vec3(below + VELK_CUT_NODE_RIGHT_MAX));
                }
                velk_store_box(node + VELK_CUT_NODE_LEFT_MIN + k * (VELK_CUT_NODE_RIGHT_MIN - VELK_CUT_NODE_LEFT_MIN), lo, hi);
            }
            cull_words.data[node + VELK_CUT_NODE_LEVEL] = height + 1u;
            cull_words.data[node + VELK_CUT_NODE_ROOT] = round;
        }
        memoryBarrierBuffer();
        barrier();
        if (cull_words.data[root + VELK_CUT_NODE_ROOT] != 0u) break;
    }
    uint height = cull_words.data[root + VELK_CUT_NODE_ROOT] != 0u ? cull_words.data[root + VELK_CUT_NODE_LEVEL] : 0u;
    memoryBarrierBuffer();
    barrier();
    for (uint i = lane; i < internal; i += lanes) {
        cull_words.data[velk_lbvh_node_word(node_count - 1u - i) + VELK_CUT_NODE_ROOT] = i == 0u ? 1u : 0u;
    }
    memoryBarrierBuffer();
    barrier();
    return height;
}

// Builds the radix tree over `n` leaves (velk_lbvh_build), and a balanced one instead where it
// is deeper than a balanced one by more than 16 levels, so that the walk's stack holds
// (VELK_TRACE_STACK_SIZE).
uint g_lbvh_tried_height = 0u;  // the radix tree's height the last build tried, for the log
void velk_lbvh_build_bounded(uint node_count, uint n, uint lane, uint lanes, bool balanced)
{
    g_lbvh_balanced = balanced;
    uint height = velk_lbvh_build(node_count, n, lane, lanes);
    g_lbvh_tried_height = height;
    uint most = (n > 1u ? uint(findMSB(n - 1u)) + 1u : 1u) + 16u;
    if (!balanced && (height == 0u || height > most)) {
        g_lbvh_balanced = true;
        velk_lbvh_build(node_count, n, lane, lanes);
    }
}
