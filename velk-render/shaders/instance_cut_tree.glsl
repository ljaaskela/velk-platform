#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct CullInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer CullInstances { CullInstance data[]; } cull_instances;
layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;
// The clusters' triangle trees (VelkMeshNode, 16 words each) and their ray headers, for a
// cluster's box (velk_cut_record_box).
layout(set = 1, binding = 12, std430) readonly buffer LodNodes { uint data[]; } lod_nodes;
layout(set = 1, binding = 13, std430) readonly buffer ClusterWords { uint data[]; } cluster_words;

layout(push_constant) uniform PC {
    uint cut_word;
    uint instances_base;
    uint items_word;
    float pad;
    uint globals_base;   // the view's, for sizes on screen
    float light_pixels;  // the light's level, as at instance_cut_choose_src
    uint mode;           // 0 a group a partition, 1 a group a list of the long lists' queue
} pc;
#velk_include VELK_CUT_GLSL
#velk_include VELK_CUT_BOX_GLSL
#velk_include VELK_SORT_GLSL
#velk_include VELK_LBVH_GLSL
#velk_include VELK_RAY_ENTRY_GLSL
shared uint s_counts[64];
// After the sort: the key of each leaf of four records, that of its first, and how many records
// have a box, which the sort puts first.
shared uint s_leaf_key[kSortMax / 4u];
shared uint s_bounded;
// The box around what the partition draws and its counts, each thread's share, then added up.
shared vec3 s_lo[64];
shared vec3 s_hi[64];
shared uint s_always[64];
shared uint s_for_rays[64];
shared uint s_quads[64];
// A long list's place in the queue of the long lists' sort, kCullNoSlot when its own group sorts
// it; and the first block it took.
shared uint s_large_slot;
shared uint s_large_first;

// Adds `record`'s share to the partition's box and counts: none for one for rays only
// (kCutForRays) or a patch for rays, which are not drawn; a deforming mesh's or an item drawn
// by its own pass makes the partition always visited.
void velk_draw_share(uint record, inout vec3 lo, inout vec3 hi, inout uint always, inout uint for_rays, inout uint quads)
{
    uint entry = cull_words.data[record + VELK_RECORD_ENTRY];
    if (velk_cut_for_rays(entry)) {
        ++for_rays;
        return;
    }
    if (entry == VELK_CUT_RAY_QUAD) return;
    if (entry == VELK_CUT_QUAD) {
        ++quads;
    } else if (entry != 0xFFFFFFFFu) {
        uint flags = cull_words.data[entry + VELK_CLUSTER_ENTRY_FLAGS];
        if ((flags & VELK_LOD_DEFORMING) != 0u) always = 1u;
    } else if (pc.items_word != 0xFFFFFFFFu) {
        uint item = pc.items_word + cull_words.data[record + VELK_RECORD_SLOT] * VELK_PLAIN_ITEM_WORDS;
        if (cull_words.data[item + VELK_PLAIN_ITEM_TABLE] == 0xFFFFFFFFu) always = 1u;
    }
    vec3 rlo, rhi;
    velk_cut_record_box(record, pc.instances_base, pc.items_word, pc.pad, rlo, rhi);
    lo = min(lo, rlo);
    hi = max(hi, rhi);
}

// How wide on screen, from `from`, the error of the sphere at `word` (centre, radius, error) is.
float velk_ray_pixels(uint word, mat4 world, float scale, vec3 from, float pixels_per_unit)
{
    vec3 center = mat3(world) * vec3(uintBitsToFloat(cull_words.data[word]), uintBitsToFloat(cull_words.data[word + 1u]),
                                     uintBitsToFloat(cull_words.data[word + 2u])) + world[3].xyz - from;
    float away = length(center) - uintBitsToFloat(cull_words.data[word + 3u]) * scale;
    return uintBitsToFloat(cull_words.data[word + 4u]) * scale * pixels_per_unit / max(away, 1e-3);
}

// Whether rays trace `record`, the record at `place` of the pool: at the light's level, seen
// from `viewpoint` where its partition chose. A record for rays only is, and a patch for rays;
// a record of a member in a patch is not, the patch standing for it; a cluster drawn finer than
// the light, whose parent is not coarser than the light, is not, since a record for rays only
// covers it.
bool velk_ray_record(uint record, uint place, vec3 viewpoint, float pixels_per_unit)
{
    uint marks = cull_words.data[pc.cut_word + VELK_CUT_RAY_MARKS_WORD];
    if ((cull_words.data[marks + (place >> 5u)] & (1u << (place & 31u))) != 0u) return false;
    uint entry = cull_words.data[record + VELK_RECORD_ENTRY];
    if (entry >= VELK_CUT_RAY_QUAD || velk_cut_for_rays(entry)) return true;
    uint slot = cull_words.data[record + VELK_RECORD_SLOT];
    uint instance = pc.instances_base + slot;
    if (pc.items_word != 0xFFFFFFFFu) instance = cull_words.data[pc.items_word + slot * VELK_PLAIN_ITEM_WORDS];
    if (instance == 0xFFFFFFFFu) return false;
    CullInstance ci = cull_instances.data[instance];
    mat4 world = velk_sized_world(ci.world_matrix, ci.offset.xyz, ci.size.xyz);
    float scale = sqrt(max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)), dot(world[2].xyz, world[2].xyz)));
    uint cluster = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + cull_words.data[record + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS;
    return velk_ray_pixels(cluster + VELK_LOD_CLUSTER_PARENT_SPHERE, world, scale, viewpoint, pixels_per_unit) > pc.light_pixels;
}

// The partition the tree is built over: the word of its page table in the current one, and how
// many of its ray list's entries its leaves of four hold.
uint g_table = 0u;
uint g_leaf_records = 0u;

// The word of entry `i` of the partition's ray list.
uint velk_tree_ray_word(uint i)
{
    return velk_cut_ray_word(pc.cut_word, velk_cut_place(g_table, i));
}

// The box around the centres of the `count` entries of the partition's ray list that have a box,
// the same in every thread. All threads of the group call it.
void velk_ray_list_box(uint count, uint lane, out vec3 lo, out vec3 hi)
{
    lo = vec3(1e30);
    hi = vec3(-1e30);
    for (uint i = lane; i < count; i += 64u) {
        vec3 blo, bhi, c;
        float radius;
        if (!velk_ray_entry_box(velk_tree_ray_word(i), blo, bhi, c, radius)) continue;
        lo = min(lo, c);
        hi = max(hi, c);
    }
    s_lo[lane] = lo;
    s_hi[lane] = hi;
    memoryBarrierShared();
    barrier();
    for (uint i = 0u; i < 64u; ++i) {
        lo = min(lo, s_lo[i]);
        hi = max(hi, s_hi[i]);
    }
    memoryBarrierShared();
    barrier();
}

// Puts the `count` entries of the partition's ray list in Morton order of their spheres' centres
// within the box around them, so that the tree's leaves of four hold records near each other;
// records at least a quarter as wide as that box go after them, together, so that they widen as
// few nodes as they can; records without a box last. Leaves s_leaf_key and s_bounded. All
// threads of the group call it.
void velk_sort_ray_list(uint count, uint lane)
{
    vec3 lo = vec3(1e30);
    vec3 hi = vec3(-1e30);
    for (uint i = lane; i < count; i += 64u) {
        vec3 blo, bhi, c;
        float radius;
        if (!velk_ray_entry_box(velk_tree_ray_word(i), blo, bhi, c, radius)) continue;
        lo = min(lo, c);
        hi = max(hi, c);
    }
    s_lo[lane] = lo;
    s_hi[lane] = hi;
    if (lane == 0u) s_bounded = 0u;
    memoryBarrierShared();
    barrier();
    for (uint i = 0u; i < 64u; ++i) {
        lo = min(lo, s_lo[i]);
        hi = max(hi, s_hi[i]);
    }
    float extent = max(max(hi.x - lo.x, hi.y - lo.y), hi.z - lo.z);
    uint n = 1u;
    while (n < count) n <<= 1u;
    for (uint i = lane; i < n; i += 64u) {
        uint key = 0xFFFFFFFFu;
        if (i < count) {
            uint at = velk_tree_ray_word(i);
            vec3 blo, bhi, c;
            float radius;
            // 20 bits of Morton code under a bit for the wide ones; a record without a box last,
            // its code above every other.
            uint code = 0x1FFFFFu;
            if (velk_ray_entry_box(at, blo, bhi, c, radius)) {
                code = ((radius >= 0.25 * extent ? 1u : 0u) << 20u) | min(velk_morton(c, lo, hi) >> 10u, 0xFFFFEu);
                atomicAdd(s_bounded, 1u);
            }
            key = (code << kSortIndexBits) | i;
        }
        s_order[i] = key;
    }
    memoryBarrierShared();
    barrier();
    velk_sort_keys(n, lane, 64u);
    // Each leaf's key, its last record with a box's, so that a leaf holding a wide record goes
    // with the wide ones; before the keys give way to the records.
    for (uint g = lane; g * 4u < count; g += 64u) s_leaf_key[g] = s_order[min(g * 4u + 3u, max(s_bounded, 1u) - 1u)];
    memoryBarrierShared();
    barrier();
    // Each place's record in the new order, read before any entry is written again.
    for (uint i = lane; i < count; i += 64u) {
        s_order[i] = cull_words.data[velk_tree_ray_word(s_order[i] & kSortIndexMask) + VELK_CUT_RAY_INDEX];
    }
    memoryBarrierShared();
    barrier();
    for (uint i = lane; i < count; i += 64u) velk_write_ray_entry(velk_tree_ray_word(i), s_order[i]);
    memoryBarrierBuffer();
    barrier();
}


// Past kSortMax entries, the sort is a radix sort in the partition's node pages, before the tree
// is built on them: a page's kCutPageNodes nodes are four words for each of its kCutPageRecords
// entries, two buffers of (code, place in the list) pairs. A thread's run of entries is counted
// by digit, the counts summed in order of digit then thread, and the run moved in its order, so
// that each pass keeps the order of the one before. A list the long lists' sort has room for is
// sorted by that instead, the same way over many work groups (instance_cut_large_src).
shared uint s_radix[16u * 64u];
shared uint s_radix_sum[64];
// Whether the radix sort made the order: a leaf's key is then its code, made again from its entry
// (velk_lbvh_key); the box its codes are within.
bool g_large_sort = false;
vec3 g_sort_lo = vec3(0.0);
vec3 g_sort_hi = vec3(0.0);
float g_sort_extent = 0.0;

// Word `w` of the radix sort's scratch, in the partition's node pages.
uint velk_sort_scratch(uint w)
{
    return velk_lbvh_node_word(w / VELK_CUT_NODE_WORDS) + w % VELK_CUT_NODE_WORDS;
}

// The radix sort's code of the entry at word `at`: as velk_sort_ray_list's, all 30 bits of its
// Morton code under the bit for the wide ones, and above every other for one without a box.
uint velk_ray_code_large(uint at)
{
    return velk_ray_entry_code(at, g_sort_lo, g_sort_hi, g_sort_extent);
}

// Sets the box the codes of a list past kSortMax entries are within (velk_ray_code_large).
void velk_large_sort_box(vec3 lo, vec3 hi)
{
    g_sort_lo = lo;
    g_sort_hi = hi;
    g_sort_extent = max(max(hi.x - lo.x, hi.y - lo.y), hi.z - lo.z);
    g_large_sort = true;
}

// velk_sort_ray_list for more than kSortMax entries. Leaves s_bounded. All threads of the group
// call it.
void velk_sort_ray_list_large(uint count, uint lane)
{
    vec3 lo, hi;
    velk_ray_list_box(count, lane, lo, hi);
    if (lane == 0u) s_bounded = 0u;
    memoryBarrierShared();
    barrier();
    velk_large_sort_box(lo, hi);
    for (uint i = lane; i < count; i += 64u) {
        uint code = velk_ray_code_large(velk_tree_ray_word(i));
        if (code != 0xFFFFFFFFu) atomicAdd(s_bounded, 1u);
        cull_words.data[velk_sort_scratch(2u * i)] = code;
        cull_words.data[velk_sort_scratch(2u * i + 1u)] = i;
    }
    memoryBarrierBuffer();
    memoryBarrierShared();
    barrier();
    uint run = (count + 63u) / 64u;
    uint begin = min(lane * run, count);
    uint end = min(begin + run, count);
    uint from = 0u;
    uint to = 2u * count;
    for (uint pass = 0u; pass < 8u; ++pass) {
        uint shift = 4u * pass;
        for (uint d = 0u; d < 16u; ++d) s_radix[d * 64u + lane] = 0u;
        memoryBarrierShared();
        barrier();
        for (uint i = begin; i < end; ++i) {
            uint d = (cull_words.data[velk_sort_scratch(from + 2u * i)] >> shift) & 15u;
            s_radix[d * 64u + lane] += 1u;
        }
        memoryBarrierShared();
        barrier();
        // Where each thread's entries of each digit go: the counts before it, digit then thread.
        uint sum = 0u;
        for (uint k = 0u; k < 16u; ++k) sum += s_radix[lane * 16u + k];
        s_radix_sum[lane] = sum;
        memoryBarrierShared();
        barrier();
        uint before = 0u;
        for (uint t = 0u; t < lane; ++t) before += s_radix_sum[t];
        for (uint k = 0u; k < 16u; ++k) {
            uint n = s_radix[lane * 16u + k];
            s_radix[lane * 16u + k] = before;
            before += n;
        }
        memoryBarrierShared();
        barrier();
        for (uint i = begin; i < end; ++i) {
            uint code = cull_words.data[velk_sort_scratch(from + 2u * i)];
            uint place = cull_words.data[velk_sort_scratch(from + 2u * i + 1u)];
            uint d = (code >> shift) & 15u;
            uint dst = s_radix[d * 64u + lane];
            s_radix[d * 64u + lane] = dst + 1u;
            cull_words.data[velk_sort_scratch(to + 2u * dst)] = code;
            cull_words.data[velk_sort_scratch(to + 2u * dst + 1u)] = place;
        }
        memoryBarrierBuffer();
        barrier();
        uint swap = from;
        from = to;
        to = swap;
    }
    // Each place's record in the new order, into the other buffer before any entry is written again.
    for (uint i = lane; i < count; i += 64u) {
        uint place = cull_words.data[velk_sort_scratch(from + 2u * i + 1u)];
        cull_words.data[velk_sort_scratch(to + i)] = cull_words.data[velk_tree_ray_word(place) + VELK_CUT_RAY_INDEX];
    }
    memoryBarrierBuffer();
    barrier();
    for (uint i = lane; i < count; i += 64u) velk_write_ray_entry(velk_tree_ray_word(i), cull_words.data[velk_sort_scratch(to + i)]);
    memoryBarrierBuffer();
    barrier();
}

uint velk_lbvh_key(uint g)
{
    if (g_large_sort) return velk_ray_code_large(velk_tree_ray_word(min(g * 4u + 3u, max(s_bounded, 1u) - 1u)));
    return s_leaf_key[g] >> kSortIndexBits;
}

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

// The tree over the partition at state word `state` with `count` entries in its ray list, the
// first `leaf_records` of them in leaves of four, in their order when `sorted`, else balanced; its
// root and whether it fell back to a balanced tree. All threads of the group call it.
void velk_build_tree(uint state, uint count, uint leaf_records, bool sorted, uint lane)
{
    g_leaf_records = leaf_records;
    uint node_count = velk_cut_tree_nodes(count);
    velk_lbvh_build_bounded(node_count, (leaf_records + 3u) / 4u, lane, 64u, !sorted);
    if (lane == 0u) {
        cull_words.data[state + VELK_PART_ROOT] = velk_lbvh_node_ref(node_count - 1u);
        cull_words.data[state + VELK_PART_BALANCED] = sorted && g_lbvh_balanced ? max(g_lbvh_tried_height, 1u) : 0u;
    }
}

// The word of the long lists' sort's queue place `slot` of the cut.
uint velk_large_list_word(uint large, uint slot)
{
    return large + VELK_CUT_LARGE_END + slot * VELK_CUT_LARGE_LIST_END;
}

// Whether the long lists' sort takes the partition's ray list of `count` entries, past kSortMax:
// its blocks if there is room for them all, then a place in the queue, which has one for every
// partition. The blocks of a list there was no room for are named by none. When taken, the box
// its codes are within and the rest the sort needs are in its place. All threads of the group
// call it.
bool velk_large_take(uint state, uint count, uint lane)
{
    uint cut = pc.cut_word;
    uint large = cull_words.data[cut + VELK_CUT_LARGE_WORD];
    uint blocks = (count + VELK_CUT_LARGE_BLOCK_ENTRIES - 1u) / VELK_CUT_LARGE_BLOCK_ENTRIES;
    uint cap = cull_words.data[large + VELK_CUT_LARGE_CAP];
    if (lane == 0u) {
        uint first = atomicAdd(cull_words.data[large + VELK_CUT_LARGE_BLOCKS], blocks);
        s_large_first = first;
        s_large_slot = first + blocks <= cap ? atomicAdd(cull_words.data[large + VELK_CUT_LARGE_LISTS], 1u) : 0xFFFFFFFFu;
    }
    memoryBarrierShared();
    barrier();
    uint first = s_large_first;
    uint slot = s_large_slot;
    // Each block it took that is in the table: of its place, or of none.
    uint table = large + VELK_CUT_LARGE_END + cull_words.data[cut + VELK_CUT_PARTITIONS] * VELK_CUT_LARGE_LIST_END;
    for (uint b = first + lane; b < first + blocks && b < cap; b += 64u) cull_words.data[table + b] = slot;
    if (slot == 0xFFFFFFFFu) return false;
    vec3 lo, hi;
    velk_ray_list_box(count, lane, lo, hi);
    if (lane == 0u) {
        uint at = velk_large_list_word(large, slot);
        cull_words.data[at + VELK_CUT_LARGE_LIST_STATE] = state;
        cull_words.data[at + VELK_CUT_LARGE_LIST_TABLE] = g_table;
        cull_words.data[at + VELK_CUT_LARGE_LIST_COUNT] = count;
        cull_words.data[at + VELK_CUT_LARGE_LIST_FIRST_BLOCK] = first;
        cull_words.data[at + VELK_CUT_LARGE_LIST_BOUNDED] = 0u;
        velk_store_box(at + VELK_CUT_LARGE_LIST_LO, lo, hi);
    }
    return true;
}

void main()
{
    uint cut = pc.cut_word;
    uint lane = gl_LocalInvocationIndex;
    // A list of the long lists' queue, sorted by now: its tree.
    if (pc.mode != 0u) {
        uint large = cull_words.data[cut + VELK_CUT_LARGE_WORD];
        if (gl_WorkGroupID.x >= cull_words.data[large + VELK_CUT_LARGE_LISTS]) return;
        uint at = velk_large_list_word(large, gl_WorkGroupID.x);
        uint state = cull_words.data[at + VELK_CUT_LARGE_LIST_STATE];
        g_table = cull_words.data[at + VELK_CUT_LARGE_LIST_TABLE];
        uint count = cull_words.data[at + VELK_CUT_LARGE_LIST_COUNT];
        if (lane == 0u) s_bounded = cull_words.data[at + VELK_CUT_LARGE_LIST_BOUNDED];
        memoryBarrierShared();
        barrier();
        velk_large_sort_box(velk_load_vec3(at + VELK_CUT_LARGE_LIST_LO), velk_load_vec3(at + VELK_CUT_LARGE_LIST_HI));
        velk_build_tree(state, count, s_bounded, true, lane);
        return;
    }
    if (gl_WorkGroupID.x >= cull_words.data[cut + VELK_CUT_PARTITIONS]) return;
    uint state = velk_cut_state(cut, gl_WorkGroupID.x);
    // The same for the whole group.
    if (cull_words.data[state + VELK_PART_CHOOSING] == 0u) return;
    uint records_count = cull_words.data[state + VELK_PART_COUNT];
    g_table = velk_cut_table(cut, true) + cull_words.data[state + VELK_PART_TABLE_FIRST];

    // The records rays trace, in their order: each thread a run of them, counted, then placed
    // after the runs before.
    GlobalData g = velk_globals.data[pc.globals_base];
    mat4 vp = g.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
    vec3 viewpoint = vec3(uintBitsToFloat(cull_words.data[state + VELK_PART_FROM]), uintBitsToFloat(cull_words.data[state + VELK_PART_FROM + 1u]),
                          uintBitsToFloat(cull_words.data[state + VELK_PART_FROM + 2u]));
    uint per = (records_count + 63u) / 64u;
    uint from = min(lane * per, records_count);
    uint to = min(from + per, records_count);
    uint mine = 0u;
    vec3 draw_lo = vec3(1e30);
    vec3 draw_hi = vec3(-1e30);
    uint always = 0u;
    uint for_rays = 0u;
    uint quads = 0u;
    for (uint r = from; r < to; ++r) {
        uint place = velk_cut_place(g_table, r);
        uint record = velk_cut_record_word(cut, place);
        if (velk_ray_record(record, place, viewpoint, pixels_per_unit)) ++mine;
        velk_draw_share(record, draw_lo, draw_hi, always, for_rays, quads);
    }
    s_counts[lane] = mine;
    s_lo[lane] = draw_lo;
    s_hi[lane] = draw_hi;
    s_always[lane] = always;
    s_for_rays[lane] = for_rays;
    s_quads[lane] = quads;
    barrier();
    if (lane == 0u) {
        for (uint i = 1u; i < 64u; ++i) {
            draw_lo = min(draw_lo, s_lo[i]);
            draw_hi = max(draw_hi, s_hi[i]);
            always |= s_always[i];
            for_rays += s_for_rays[i];
            quads += s_quads[i];
        }
        for (uint k = 0u; k < 3u; ++k) {
            cull_words.data[state + VELK_PART_BOX + k] = floatBitsToUint(draw_lo[k]);
            cull_words.data[state + VELK_PART_BOX + 3u + k] = floatBitsToUint(draw_hi[k]);
        }
        cull_words.data[state + VELK_PART_ALWAYS] = always;
        cull_words.data[state + VELK_PART_FOR_RAYS] = for_rays;
        cull_words.data[state + VELK_PART_QUADS] = quads;
    }
    uint at = 0u;
    for (uint i = 0u; i < lane; ++i) at += s_counts[i];
    for (uint r = from; r < to; ++r) {
        uint place = velk_cut_place(g_table, r);
        if (!velk_ray_record(velk_cut_record_word(cut, place), place, viewpoint, pixels_per_unit)) continue;
        velk_write_ray_entry(velk_tree_ray_word(at), place);
        ++at;
    }
    uint count = 0u;
    for (uint i = 0u; i < 64u; ++i) count += s_counts[i];
    if (lane == 0u) cull_words.data[state + VELK_PART_RAY_COUNT] = count;
    memoryBarrierBuffer();
    barrier();
    if (count == 0u) return;
    // Where they are, those without a box last and left out of the leaves: sorted in the group's
    // memory as far as it holds them; past that by the long lists' sort when it has room, which
    // then builds the tree, else in the partition's node pages.
    bool sorted = count > 4u;
    uint leaf_records = count;
    if (count > kSortMax) {
        if (velk_large_take(state, count, lane)) return;
        velk_sort_ray_list_large(count, lane);
        leaf_records = s_bounded;
    } else if (sorted) {
        velk_sort_ray_list(count, lane);
        leaf_records = s_bounded;
    }
    velk_build_tree(state, count, leaf_records, sorted, lane);
}
