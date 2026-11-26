// The partition the tree is built over: the word of its page table in the current one, and how
// many of its ray list's entries its leaves of four hold.
uint g_table = 0u;
uint g_leaf_records = 0u;

// Leaf g: up to four entries from the ray list's place 4g, which never cross a page, as the first
// one's place in the pool times four plus how many less one.
uint velk_lbvh_leaf_ref(uint g)
{
    uint first = g * 4u;
    return 0x80000000u | (velk_cut_place(g_table, first) * 4u + min(4u, g_leaf_records - first) - 1u);
}

void velk_lbvh_leaf_box(uint ref, out vec3 lo, out vec3 hi)
{
    lo = vec3(1e30);
    hi = vec3(-1e30);
    uint first = (ref & 0x7FFFFFFFu) >> 2u;
    uint n = (ref & 3u) + 1u;
    for (uint r = 0u; r < n; ++r) {
        uint at = velk_cut_ray_word(pc.cut_word, first + r);
        lo = min(lo, velk_load_vec3(at + VELK_CUT_RAY_MIN));
        hi = max(hi, velk_load_vec3(at + VELK_CUT_RAY_MAX));
    }
}

// The tree's node i is on the partition's pages, named by its place in the pool.
uint velk_lbvh_node_word(uint i)
{
    return velk_cut_node_word(pc.cut_word, velk_cut_node_place(g_table, i));
}

uint velk_lbvh_node_ref(uint i)
{
    return velk_cut_node_place(g_table, i);
}

uint velk_lbvh_ref_word(uint ref)
{
    return velk_cut_node_word(pc.cut_word, ref);
}
