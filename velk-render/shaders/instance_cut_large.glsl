#version 450
#include "velk.glsl"

layout(local_size_x = 256) in;

struct CullInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer CullInstances { CullInstance data[]; } cull_instances;
// Coherent: a tree's bottom-up build hands each node's boxes from one work group to another
// through its visit count (mode 9).
layout(set = 1, binding = 16, std430) coherent buffer CullWords { uint data[]; } cull_words;
// The clusters' triangle trees and their ray headers, for a cluster's box (velk_cut_record_box).
layout(set = 1, binding = 12, std430) readonly buffer LodNodes { uint data[]; } lod_nodes;
layout(set = 1, binding = 13, std430) readonly buffer ClusterWords { uint data[]; } cluster_words;

layout(push_constant) uniform PC {
    uint cut_word;
    uint instances_base;
    uint items_word;
    float pad;
    uint mode;    // which pass: see main
    uint shift;   // the digit's lowest bit, in the passes over digits
    uint source;  // the scratch buffer the pass reads, 0 or 1; it writes the other. In the tree's
                  // passes, the try: 0, or 1 for the balanced tree built again
} pc;
#velk_include VELK_CUT_GLSL
#velk_include VELK_CUT_BOX_GLSL
#velk_include VELK_SORT_GLSL
#velk_include VELK_RAY_ENTRY_GLSL
#velk_include VELK_LBVH_GLSL
#velk_include VELK_CUT_TREE_NODES_GLSL

shared uint s_digits[256];
shared uint s_bounded_here;

// The long lists' sort's words of the cut, and where its queue, block table and digit counts are.
uint g_large = 0u;
uint g_table_word = 0u;
uint g_counts_word = 0u;

void velk_large_words()
{
    g_large = cull_words.data[pc.cut_word + VELK_CUT_LARGE_WORD];
    g_table_word = g_large + VELK_CUT_LARGE_END + cull_words.data[pc.cut_word + VELK_CUT_PARTITIONS] * VELK_CUT_LARGE_LIST_END;
    g_counts_word = g_table_word + cull_words.data[g_large + VELK_CUT_LARGE_CAP];
}

uint velk_large_list_word(uint slot)
{
    return g_large + VELK_CUT_LARGE_END + slot * VELK_CUT_LARGE_LIST_END;
}

// The list this work group's block is of: its queue place's word, the block's first entry in the
// list, and the list's count; false for a block of no list.
bool velk_large_block(out uint list, out uint first, out uint count)
{
    list = 0u;
    first = 0u;
    count = 0u;
    uint block = gl_WorkGroupID.x;
    if (block >= min(cull_words.data[g_large + VELK_CUT_LARGE_BLOCKS], cull_words.data[g_large + VELK_CUT_LARGE_CAP])) return false;
    uint slot = cull_words.data[g_table_word + block];
    if (slot == 0xFFFFFFFFu) return false;
    list = velk_large_list_word(slot);
    first = (block - cull_words.data[list + VELK_CUT_LARGE_LIST_FIRST_BLOCK]) * VELK_CUT_LARGE_BLOCK_ENTRIES;
    count = cull_words.data[list + VELK_CUT_LARGE_LIST_COUNT];
    return true;
}

// The word of entry `i` of the list whose page table is at word `table`.
uint velk_large_entry_word(uint table, uint i)
{
    return velk_cut_ray_word(pc.cut_word, velk_cut_place(table, i));
}

// Word `w` of the list's scratch, in its partition's node pages: two buffers of (code, place in
// the list) pairs, as velk_sort_ray_list_large has them.
uint velk_large_scratch(uint table, uint w)
{
    return velk_cut_node_word(pc.cut_word, velk_cut_node_place(table, w / VELK_CUT_NODE_WORDS)) + w % VELK_CUT_NODE_WORDS;
}

// The tree of a queued list, built once it is sorted, the same tree velk_lbvh_build_bounded
// builds: its leaves of four, its internal nodes and all its nodes; and, in the digit counts of
// its blocks, which the sort is done with, each leaf's key, each leaf's and internal node's
// parent (its index times two, plus one for a right child) and each internal node's visits.
uint g_n = 0u;
uint g_internal = 0u;
uint g_node_count = 0u;
uint g_keys = 0u;
uint g_parent_leaf = 0u;
uint g_parent_node = 0u;
uint g_visit = 0u;

void velk_large_tree(uint list, uint count)
{
    g_table = cull_words.data[list + VELK_CUT_LARGE_LIST_TABLE];
    g_leaf_records = cull_words.data[list + VELK_CUT_LARGE_LIST_BOUNDED];
    g_n = (g_leaf_records + 3u) / 4u;
    g_internal = g_n > 1u ? g_n - 1u : 1u;
    g_node_count = velk_cut_tree_nodes(count);
    g_keys = g_counts_word + cull_words.data[list + VELK_CUT_LARGE_LIST_FIRST_BLOCK] * 256u;
    g_parent_leaf = g_keys + g_n;
    g_parent_node = g_parent_leaf + g_n;
    g_visit = g_parent_node + g_internal;
    g_lbvh_balanced = cull_words.data[list + VELK_CUT_LARGE_LIST_RETRY] != 0u;
}

uint velk_lbvh_key(uint g)
{
    return cull_words.data[g_keys + g];
}

void main()
{
    uint lane = gl_LocalInvocationIndex;
    velk_large_words();
    // 10: a group a list, one thread: the tree's root, or the balanced tree to build again when
    // the sorted one is deeper than velk_lbvh_build_bounded allows; a tree of one node whole.
    if (pc.mode == 10u) {
        if (lane != 0u || gl_WorkGroupID.x >= cull_words.data[g_large + VELK_CUT_LARGE_LISTS]) return;
        uint list = velk_large_list_word(gl_WorkGroupID.x);
        velk_large_tree(list, cull_words.data[list + VELK_CUT_LARGE_LIST_COUNT]);
        uint state = cull_words.data[list + VELK_CUT_LARGE_LIST_STATE];
        uint root = velk_lbvh_node_word(g_node_count - 1u);
        if (g_n < 2u) {
            if (pc.source != 0u) return;
            vec3 lo = vec3(1e30);
            vec3 hi = vec3(-1e30);
            uint left = 0xFFFFFFFFu;
            if (g_n == 1u) {
                left = velk_lbvh_leaf_ref(0u);
                velk_lbvh_leaf_box(left, lo, hi);
            }
            velk_store_box(root + VELK_CUT_NODE_LEFT_MIN, lo, hi);
            velk_store_box(root + VELK_CUT_NODE_RIGHT_MIN, vec3(1e30), vec3(-1e30));
            cull_words.data[root + VELK_CUT_NODE_LEFT_REF] = left;
            cull_words.data[root + VELK_CUT_NODE_RIGHT_REF] = 0xFFFFFFFFu;
            cull_words.data[root + VELK_CUT_NODE_LEVEL] = 1u;
            cull_words.data[root + VELK_CUT_NODE_ROOT] = 1u;
            cull_words.data[state + VELK_PART_ROOT] = velk_lbvh_node_ref(g_node_count - 1u);
            cull_words.data[state + VELK_PART_BALANCED] = 0u;
            return;
        }
        if (pc.source != 0u && !g_lbvh_balanced) return;
        if (pc.source == 0u) {
            // As velk_lbvh_build's rounds: a root not done in 72 of them has no height.
            uint height = cull_words.data[root + VELK_CUT_NODE_LEVEL];
            uint tried = height > 72u ? 0u : height;
            uint most = uint(findMSB(g_n - 1u)) + 1u + 16u;
            if (tried == 0u || tried > most) {
                cull_words.data[list + VELK_CUT_LARGE_LIST_RETRY] = 1u;
                cull_words.data[list + VELK_CUT_LARGE_LIST_TRIED] = tried;
                return;
            }
        }
        cull_words.data[root + VELK_CUT_NODE_ROOT] = 1u;
        cull_words.data[state + VELK_PART_ROOT] = velk_lbvh_node_ref(g_node_count - 1u);
        cull_words.data[state + VELK_PART_BALANCED] = g_lbvh_balanced ? max(cull_words.data[list + VELK_CUT_LARGE_LIST_TRIED], 1u) : 0u;
        return;
    }
    // 0: the group counts of the passes, from what was queued.
    if (pc.mode == 0u) {
        if (lane != 0u) return;
        uint lists = cull_words.data[g_large + VELK_CUT_LARGE_LISTS];
        uint blocks = min(cull_words.data[g_large + VELK_CUT_LARGE_BLOCKS], cull_words.data[g_large + VELK_CUT_LARGE_CAP]);
        cull_words.data[g_large + VELK_CUT_LARGE_LIST_GROUPS] = lists;
        cull_words.data[g_large + VELK_CUT_LARGE_BLOCK_GROUPS] = blocks;
        return;
    }
    // 3: a group a list, a thread a digit: where each block's entries of each digit go, the
    // counts before them, digit then block.
    if (pc.mode == 3u) {
        if (gl_WorkGroupID.x >= cull_words.data[g_large + VELK_CUT_LARGE_LISTS]) return;
        uint list = velk_large_list_word(gl_WorkGroupID.x);
        uint count = cull_words.data[list + VELK_CUT_LARGE_LIST_COUNT];
        uint first_block = cull_words.data[list + VELK_CUT_LARGE_LIST_FIRST_BLOCK];
        uint blocks = (count + VELK_CUT_LARGE_BLOCK_ENTRIES - 1u) / VELK_CUT_LARGE_BLOCK_ENTRIES;
        uint total = 0u;
        for (uint b = 0u; b < blocks; ++b) total += cull_words.data[g_counts_word + (first_block + b) * 256u + lane];
        s_digits[lane] = total;
        memoryBarrierShared();
        barrier();
        uint before = 0u;
        for (uint d = 0u; d < lane; ++d) before += s_digits[d];
        for (uint b = 0u; b < blocks; ++b) {
            uint at = g_counts_word + (first_block + b) * 256u + lane;
            uint n = cull_words.data[at];
            cull_words.data[at] = before;
            before += n;
        }
        return;
    }
    // The rest: a group a block, a thread an entry of it.
    uint list, first, count;
    if (!velk_large_block(list, first, count)) return;
    uint table = cull_words.data[list + VELK_CUT_LARGE_LIST_TABLE];
    uint i = first + lane;
    // The list's tree, as velk_lbvh_build_bounded builds it, a thread a leaf or a node of it, of
    // which no list has more than entries; with `source` 1 only for those whose sorted tree was
    // too deep, built again balanced.
    if (pc.mode >= 7u && pc.mode <= 9u) {
        velk_large_tree(list, count);
        if (pc.source != 0u && !g_lbvh_balanced) return;
        // 7: each leaf's key, the code of its last entry with a box, as velk_lbvh_key's.
        if (pc.mode == 7u) {
            if (g_n < 2u || i >= g_n) return;
            vec3 lo = velk_load_vec3(list + VELK_CUT_LARGE_LIST_LO);
            vec3 hi = velk_load_vec3(list + VELK_CUT_LARGE_LIST_HI);
            float extent = max(max(hi.x - lo.x, hi.y - lo.y), hi.z - lo.z);
            uint entry = velk_large_entry_word(table, min(i * 4u + 3u, max(g_leaf_records, 1u) - 1u));
            cull_words.data[g_keys + i] = velk_ray_entry_code(entry, lo, hi, extent);
            return;
        }
        // 8: the nodes before the tree's cleared; each internal node's children, its height and
        // root mark cleared; each child's parent; each internal node's visits none.
        if (pc.mode == 8u) {
            if (i < g_node_count - g_internal) {
                uint node = velk_lbvh_node_word(i);
                velk_store_box(node + VELK_CUT_NODE_LEFT_MIN, vec3(1e30), vec3(-1e30));
                velk_store_box(node + VELK_CUT_NODE_RIGHT_MIN, vec3(1e30), vec3(-1e30));
                cull_words.data[node + VELK_CUT_NODE_LEFT_REF] = 0xFFFFFFFFu;
                cull_words.data[node + VELK_CUT_NODE_RIGHT_REF] = 0xFFFFFFFFu;
                cull_words.data[node + VELK_CUT_NODE_LEVEL] = 0u;
                cull_words.data[node + VELK_CUT_NODE_ROOT] = 0u;
            }
            if (g_n < 2u || i >= g_internal) return;
            uint node = velk_lbvh_node_word(g_node_count - 1u - i);
            uint l;
            uint r;
            velk_lbvh_children(int(i), int(g_n), l, r);
            bool left_leaf = (l & 0x80000000u) != 0u;
            bool right_leaf = (r & 0x80000000u) != 0u;
            cull_words.data[node + VELK_CUT_NODE_LEFT_REF] = left_leaf ? velk_lbvh_leaf_ref(l & 0x7FFFFFFFu) : velk_lbvh_node_ref(g_node_count - 1u - l);
            cull_words.data[node + VELK_CUT_NODE_RIGHT_REF] = right_leaf ? velk_lbvh_leaf_ref(r & 0x7FFFFFFFu) : velk_lbvh_node_ref(g_node_count - 1u - r);
            cull_words.data[node + VELK_CUT_NODE_LEVEL] = 0u;
            cull_words.data[node + VELK_CUT_NODE_ROOT] = 0u;
            cull_words.data[(left_leaf ? g_parent_leaf : g_parent_node) + (l & 0x7FFFFFFFu)] = i << 1u;
            cull_words.data[(right_leaf ? g_parent_leaf : g_parent_node) + (r & 0x7FFFFFFFu)] = (i << 1u) | 1u;
            cull_words.data[g_visit + i] = 0u;
            return;
        }
        // 9: from each leaf up: its box into its parent's side, made seen at the device's scope
        // before its parent's visit count is added to; the second to come to a node goes on with
        // both sides, which the first's count makes seen to it, its height and its box. No thread
        // waits for another, and a tree has fewer than 128 levels below its root.
        if (g_n < 2u || i >= g_n) return;
        vec3 lo, hi;
        velk_lbvh_leaf_box(velk_lbvh_leaf_ref(i), lo, hi);
        uint link = cull_words.data[g_parent_leaf + i];
        for (uint up = 0u; up < 128u; ++up) {
            uint p = link >> 1u;
            if (p >= g_internal) return;
            uint node = velk_lbvh_node_word(g_node_count - 1u - p);
            velk_store_box(node + VELK_CUT_NODE_LEFT_MIN + (link & 1u) * (VELK_CUT_NODE_RIGHT_MIN - VELK_CUT_NODE_LEFT_MIN), lo, hi);
            memoryBarrierBuffer();
            if (atomicAdd(cull_words.data[g_visit + p], 1u) == 0u) return;
            memoryBarrierBuffer();
            uint height = 0u;
            for (uint k = 0u; k < 2u; ++k) {
                uint ref = cull_words.data[node + VELK_CUT_NODE_LEFT_REF + k];
                if (velk_lbvh_is_node(ref)) height = max(height, cull_words.data[velk_lbvh_ref_word(ref) + VELK_CUT_NODE_LEVEL]);
            }
            cull_words.data[node + VELK_CUT_NODE_LEVEL] = height + 1u;
            lo = min(velk_load_vec3(node + VELK_CUT_NODE_LEFT_MIN), velk_load_vec3(node + VELK_CUT_NODE_RIGHT_MIN));
            hi = max(velk_load_vec3(node + VELK_CUT_NODE_LEFT_MAX), velk_load_vec3(node + VELK_CUT_NODE_RIGHT_MAX));
            if (p == 0u) return;
            link = cull_words.data[g_parent_node + p];
        }
        return;
    }
    bool here = i < count;
    // 1: each entry's code and place, in buffer 0, and the count of those with a box.
    if (pc.mode == 1u) {
        if (lane == 0u) s_bounded_here = 0u;
        memoryBarrierShared();
        barrier();
        if (here) {
            vec3 lo = velk_load_vec3(list + VELK_CUT_LARGE_LIST_LO);
            vec3 hi = velk_load_vec3(list + VELK_CUT_LARGE_LIST_HI);
            float extent = max(max(hi.x - lo.x, hi.y - lo.y), hi.z - lo.z);
            uint code = velk_ray_entry_code(velk_large_entry_word(table, i), lo, hi, extent);
            if (code != 0xFFFFFFFFu) atomicAdd(s_bounded_here, 1u);
            cull_words.data[velk_large_scratch(table, 2u * i)] = code;
            cull_words.data[velk_large_scratch(table, 2u * i + 1u)] = i;
        }
        memoryBarrierShared();
        barrier();
        if (lane == 0u && s_bounded_here != 0u) atomicAdd(cull_words.data[list + VELK_CUT_LARGE_LIST_BOUNDED], s_bounded_here);
        return;
    }
    uint from = pc.source * 2u * count;
    uint to = (1u - pc.source) * 2u * count;
    uint block_counts = g_counts_word + gl_WorkGroupID.x * 256u;
    // 2: the block's count of each digit.
    if (pc.mode == 2u) {
        s_digits[lane] = 0u;
        memoryBarrierShared();
        barrier();
        if (here) atomicAdd(s_digits[(cull_words.data[velk_large_scratch(table, from + 2u * i)] >> pc.shift) & 255u], 1u);
        memoryBarrierShared();
        barrier();
        cull_words.data[block_counts + lane] = s_digits[lane];
        return;
    }
    // 4: each entry to its place in the other buffer: its digit's place for its block (mode 3),
    // and after the block's entries of that digit before it, which keeps their order.
    if (pc.mode == 4u) {
        uint code = here ? cull_words.data[velk_large_scratch(table, from + 2u * i)] : 0u;
        uint digit = (code >> pc.shift) & 255u;
        s_digits[lane] = here ? digit : 0xFFFFFFFFu;
        memoryBarrierShared();
        barrier();
        if (!here) return;
        uint rank = 0u;
        for (uint j = 0u; j < lane; ++j) rank += s_digits[j] == digit ? 1u : 0u;
        uint dst = cull_words.data[block_counts + digit] + rank;
        cull_words.data[velk_large_scratch(table, to + 2u * dst)] = code;
        cull_words.data[velk_large_scratch(table, to + 2u * dst + 1u)] = cull_words.data[velk_large_scratch(table, from + 2u * i + 1u)];
        return;
    }
    if (!here) return;
    // 5: each place's record in the new order, into the other buffer before any entry is written
    // again.
    if (pc.mode == 5u) {
        uint place = cull_words.data[velk_large_scratch(table, from + 2u * i + 1u)];
        cull_words.data[velk_large_scratch(table, to + i)] = cull_words.data[velk_large_entry_word(table, place) + VELK_CUT_RAY_INDEX];
        return;
    }
    // 6: the entries in their new order.
    if (pc.mode == 6u) {
        velk_write_ray_entry(velk_large_entry_word(table, i), cull_words.data[velk_large_scratch(table, to + i)]);
    }
}
