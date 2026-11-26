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
    uint globals_base;   // the view's, for sizes on screen
    float light_pixels;  // the light's level, as at instance_cut_choose_src
    uint mode;           // which pass: see main
} pc;
#velk_include VELK_CUT_GLSL
#velk_include VELK_CUT_BOX_GLSL
#velk_include VELK_SORT_GLSL
#velk_include VELK_RAY_ENTRY_GLSL
#velk_include VELK_CUT_RAY_RECORD_GLSL
#velk_include VELK_CUT_LARGE_QUEUE_GLSL

// Each thread's share of its block's sums, then added up by the first.
shared vec3 s_draw_lo[256];
shared vec3 s_draw_hi[256];
shared vec3 s_centre_lo[256];
shared vec3 s_centre_hi[256];
shared uint s_always[256];
shared uint s_for_rays[256];
shared uint s_quads[256];
// A bit a record of the block, set for those rays trace.
shared uint s_mask[8];

void main()
{
    uint lane = gl_LocalInvocationIndex;
    uint cut = pc.cut_word;
    velk_large_words();
    // 1: a group a list, its first thread: where each block's entries start, the list's count,
    // and what its partition's own group would add up (VELK_PART_BOX, ALWAYS, FOR_RAYS, QUADS, the
    // box around its entries' centres), each block's share added up in their order. A list of
    // kRaySortMax entries or fewer is left to instance_cut_tree_src's mode 1, as its partition's
    // own group would sort and build it.
    if (pc.mode == 1u) {
        if (lane != 0u || gl_WorkGroupID.x >= cull_words.data[g_large + VELK_CUT_LARGE_LISTS]) return;
        uint list = velk_large_list_word(gl_WorkGroupID.x);
        uint first_block = cull_words.data[list + VELK_CUT_LARGE_LIST_FIRST_BLOCK];
        uint blocks = (cull_words.data[list + VELK_CUT_LARGE_LIST_RECORDS] + VELK_CUT_LARGE_BLOCK_ENTRIES - 1u) / VELK_CUT_LARGE_BLOCK_ENTRIES;
        uint count = 0u;
        vec3 draw_lo = vec3(1e30);
        vec3 draw_hi = vec3(-1e30);
        vec3 centre_lo = vec3(1e30);
        vec3 centre_hi = vec3(-1e30);
        uint always = 0u;
        uint for_rays = 0u;
        uint quads = 0u;
        for (uint b = 0u; b < blocks; ++b) {
            uint at = g_counts_word + (first_block + b) * 256u;
            cull_words.data[at + VELK_CUT_LARGE_BLOCK_FIRST] = count;
            count += cull_words.data[at + VELK_CUT_LARGE_BLOCK_COUNT];
            draw_lo = min(draw_lo, velk_load_vec3(at + VELK_CUT_LARGE_BLOCK_DRAW_MIN));
            draw_hi = max(draw_hi, velk_load_vec3(at + VELK_CUT_LARGE_BLOCK_DRAW_MAX));
            centre_lo = min(centre_lo, velk_load_vec3(at + VELK_CUT_LARGE_BLOCK_CENTRE_MIN));
            centre_hi = max(centre_hi, velk_load_vec3(at + VELK_CUT_LARGE_BLOCK_CENTRE_MAX));
            always |= cull_words.data[at + VELK_CUT_LARGE_BLOCK_ALWAYS];
            for_rays += cull_words.data[at + VELK_CUT_LARGE_BLOCK_FOR_RAYS];
            quads += cull_words.data[at + VELK_CUT_LARGE_BLOCK_QUADS];
        }
        uint state = cull_words.data[list + VELK_CUT_LARGE_LIST_STATE];
        velk_store_box(state + VELK_PART_BOX, draw_lo, draw_hi);
        cull_words.data[state + VELK_PART_ALWAYS] = always;
        cull_words.data[state + VELK_PART_FOR_RAYS] = for_rays;
        cull_words.data[state + VELK_PART_QUADS] = quads;
        cull_words.data[state + VELK_PART_RAY_COUNT] = count;
        cull_words.data[list + VELK_CUT_LARGE_LIST_COUNT] = count;
        velk_store_box(list + VELK_CUT_LARGE_LIST_LO, centre_lo, centre_hi);
        cull_words.data[list + VELK_CUT_LARGE_LIST_SMALL] = count <= kSortMax ? 1u : 0u;
        return;
    }
    // The rest: a group a block of a list's records, a thread a record.
    uint list, first;
    if (!velk_large_block_slot(list, first)) return;
    uint records = cull_words.data[list + VELK_CUT_LARGE_LIST_RECORDS];
    uint table = cull_words.data[list + VELK_CUT_LARGE_LIST_TABLE];
    uint block = g_counts_word + gl_WorkGroupID.x * 256u;
    uint r = first + lane;
    bool here = r < records;
    // 0: whether rays trace each record, and the block's share of what the list's partition adds
    // up, as velk_scan_list's threads make theirs.
    if (pc.mode == 0u) {
        if (lane < 8u) s_mask[lane] = 0u;
        memoryBarrierShared();
        barrier();
        vec3 draw_lo = vec3(1e30);
        vec3 draw_hi = vec3(-1e30);
        vec3 centre_lo = vec3(1e30);
        vec3 centre_hi = vec3(-1e30);
        uint always = 0u;
        uint for_rays = 0u;
        uint quads = 0u;
        if (here) {
            uint state = cull_words.data[list + VELK_CUT_LARGE_LIST_STATE];
            uint place = velk_cut_place(table, r);
            uint record = velk_cut_record_word(cut, place);
            if (velk_ray_record(record, place, velk_ray_viewpoint(state), velk_ray_pixels_per_unit())) {
                atomicOr(s_mask[lane >> 5u], 1u << (lane & 31u));
                // Its centre, as velk_ray_entry_box makes it from the entry velk_write_ray_entry
                // writes: the record's box grown by the pad.
                vec3 blo, bhi;
                velk_cut_record_box(record, pc.instances_base, pc.items_word, pc.pad, blo, bhi);
                if (blo.x <= bhi.x) {
                    vec3 c = (blo + bhi) * 0.5;
                    centre_lo = c;
                    centre_hi = c;
                }
            }
            velk_draw_share(record, draw_lo, draw_hi, always, for_rays, quads);
        }
        s_draw_lo[lane] = draw_lo;
        s_draw_hi[lane] = draw_hi;
        s_centre_lo[lane] = centre_lo;
        s_centre_hi[lane] = centre_hi;
        s_always[lane] = always;
        s_for_rays[lane] = for_rays;
        s_quads[lane] = quads;
        memoryBarrierShared();
        barrier();
        if (lane != 0u) return;
        uint count = 0u;
        for (uint k = 0u; k < 8u; ++k) {
            count += bitCount(s_mask[k]);
            cull_words.data[block + VELK_CUT_LARGE_BLOCK_MASK + k] = s_mask[k];
        }
        for (uint i = 1u; i < 256u; ++i) {
            draw_lo = min(draw_lo, s_draw_lo[i]);
            draw_hi = max(draw_hi, s_draw_hi[i]);
            centre_lo = min(centre_lo, s_centre_lo[i]);
            centre_hi = max(centre_hi, s_centre_hi[i]);
            always |= s_always[i];
            for_rays += s_for_rays[i];
            quads += s_quads[i];
        }
        cull_words.data[block + VELK_CUT_LARGE_BLOCK_COUNT] = count;
        velk_store_box(block + VELK_CUT_LARGE_BLOCK_DRAW_MIN, draw_lo, draw_hi);
        velk_store_box(block + VELK_CUT_LARGE_BLOCK_CENTRE_MIN, centre_lo, centre_hi);
        cull_words.data[block + VELK_CUT_LARGE_BLOCK_ALWAYS] = always;
        cull_words.data[block + VELK_CUT_LARGE_BLOCK_FOR_RAYS] = for_rays;
        cull_words.data[block + VELK_CUT_LARGE_BLOCK_QUADS] = quads;
        return;
    }
    // 2: each record rays trace, written as its entry after the block's first (mode 1) and the
    // block's earlier ones, so that the list is in the records' order, as velk_scan_list writes it.
    if (pc.mode == 2u) {
        if (!here) return;
        uint word = lane >> 5u;
        uint bit = lane & 31u;
        uint mask = cull_words.data[block + VELK_CUT_LARGE_BLOCK_MASK + word];
        if ((mask & (1u << bit)) == 0u) return;
        uint rank = bitCount(mask & ((1u << bit) - 1u));
        for (uint k = 0u; k < word; ++k) rank += bitCount(cull_words.data[block + VELK_CUT_LARGE_BLOCK_MASK + k]);
        uint at = cull_words.data[block + VELK_CUT_LARGE_BLOCK_FIRST] + rank;
        velk_write_ray_entry(velk_cut_ray_word(cut, velk_cut_place(table, at)), velk_cut_place(table, r));
    }
}
