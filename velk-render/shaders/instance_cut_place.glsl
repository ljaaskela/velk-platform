#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint cut_word;
} pc;
#velk_include VELK_CUT_GLSL
// Takes `count` pages off the free stack; the kth is velk_cut_taken(cut, base, k). False when
// there are not that many. The count of pages taken only ever moves forward, past a take that
// fits, so that no two takes get the same pages; a swap that fails means another take moved it.
bool velk_cut_take(uint cut, uint count, out uint base)
{
    uint free_pages = cull_words.data[cut + VELK_CUT_FREE_PAGES];
    uint taken = cull_words.data[cut + VELK_CUT_PAGES_TAKEN];
    base = 0u;
    for (uint tries = 0u; tries < 4096u; ++tries) {
        if (taken + count > free_pages) return false;
        uint seen = atomicCompSwap(cull_words.data[cut + VELK_CUT_PAGES_TAKEN], taken, taken + count);
        if (seen == taken) {
            base = taken;
            return true;
        }
        taken = seen;
    }
    return false;
}

uint velk_cut_taken(uint cut, uint base, uint k)
{
    return cull_words.data[cull_words.data[cut + VELK_CUT_STACK_WORD] + cull_words.data[cut + VELK_CUT_FREE_PAGES] - 1u - base - k];
}

void main()
{
    uint cut = pc.cut_word;
    uint index = gl_GlobalInvocationID.x;
    if (index >= cull_words.data[cut + VELK_CUT_PARTITIONS]) return;
    uint state = velk_cut_state(cut, index);
    if (cull_words.data[state + VELK_PART_CHOOSING] == 0u) return;
    uint pool_pages = cull_words.data[cut + VELK_CUT_PAGES];
    uint count = cull_words.data[state + VELK_PART_COUNT];
    uint pages = velk_cut_pages(count);
    uint base = 0u;
    uint first = 0u;
    // What the frame asks of the pool, placed or not, for the sweep to judge whether it is too
    // small (instance_cut_pages_src).
    if (pages != 0u) atomicAdd(cull_words.data[cut + VELK_CUT_PAGES_DEMAND], pages);
    bool placed = pages == 0u || velk_cut_take(cut, pages, base);
    if (placed && pages != 0u) {
        first = atomicAdd(cull_words.data[cut + VELK_CUT_TABLE_USED], pages);
        // Never so, since a page is in one table at most; the pages taken go free at the sweep.
        if (first + pages > pool_pages) placed = false;
    }
    if (!placed) {
        cull_words.data[state + VELK_PART_CHOOSING] = 0u;
        uint old = cull_words.data[state + VELK_PART_OLD_COUNT];
        uint old_pages = velk_cut_pages(old);
        uint was = cull_words.data[state + VELK_PART_LAST_TABLE_FIRST];
        uint at = old != 0u && cull_words.data[state + VELK_PART_SAME] != 0u
                ? atomicAdd(cull_words.data[cut + VELK_CUT_TABLE_USED], old_pages) : pool_pages;
        if (at + old_pages <= pool_pages && was + old_pages <= pool_pages) {
            // Its old records as they were, and it chooses again on the next frame.
            uint from = velk_cut_table(cut, false) + was;
            uint to = velk_cut_table(cut, true) + at;
            for (uint p = 0u; p < old_pages; ++p) cull_words.data[to + p] = cull_words.data[from + p];
            cull_words.data[state + VELK_PART_TABLE_FIRST] = at;
            cull_words.data[state + VELK_PART_COUNT] = old;
            cull_words.data[state + VELK_PART_RAY_COUNT] = cull_words.data[state + VELK_PART_OLD_RAY_COUNT];
            cull_words.data[state + VELK_PART_RETRY] = 1u;
        } else {
            cull_words.data[state + VELK_PART_COUNT] = 0u;
            cull_words.data[state + VELK_PART_RAY_COUNT] = 0u;
        }
        cull_words.data[state + VELK_PART_OLD_COUNT] = 0u;
        return;
    }
    // Its pages in order, each page's header naming it.
    uint table = velk_cut_table(cut, true) + first;
    uint headers = cull_words.data[cut + VELK_CUT_PAGE_HEADERS];
    for (uint p = 0u; p < pages; ++p) {
        uint page = velk_cut_taken(cut, base, p);
        cull_words.data[table + p] = page;
        cull_words.data[headers + page * VELK_CUT_PAGE_WORDS + VELK_CUT_PAGE_OWNER] = index;
        cull_words.data[headers + page * VELK_CUT_PAGE_WORDS + VELK_CUT_PAGE_PLACE] = p;
    }
    cull_words.data[state + VELK_PART_TABLE_FIRST] = first;
    // Each of its chunks' first place in its page table, in the order of its chunks.
    uint counts = cull_words.data[cut + VELK_CUT_CHUNK_COUNTS_WORD] + cull_words.data[state + VELK_PART_CHUNK_START];
    uint next = 0u;
    for (uint c = 0u; c < cull_words.data[state + VELK_PART_CHUNKS]; ++c) {
        uint chunk_records = cull_words.data[counts + c];
        cull_words.data[counts + c] = next;
        next += chunk_records;
    }
}
