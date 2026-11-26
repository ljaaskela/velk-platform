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
layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;
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
    uint source;  // the scratch buffer the pass reads, 0 or 1; it writes the other
} pc;
#velk_include VELK_CUT_GLSL
#velk_include VELK_CUT_BOX_GLSL
#velk_include VELK_SORT_GLSL
#velk_include VELK_RAY_ENTRY_GLSL

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

void main()
{
    uint lane = gl_LocalInvocationIndex;
    velk_large_words();
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
