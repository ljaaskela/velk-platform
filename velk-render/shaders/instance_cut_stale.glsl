#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint cut_word;
} pc;
#velk_include VELK_CUT_GLSL
shared uint s_stale;

void main()
{
    uint cut = pc.cut_word;
    uint index = gl_WorkGroupID.x;
    if (index >= cull_words.data[cut + VELK_CUT_PARTITIONS]) return;
    uint state = velk_cut_state(cut, index);
    uint count = cull_words.data[state + VELK_PART_COUNT];
    if (count == 0u) return;
    if (gl_LocalInvocationIndex == 0u) s_stale = 0u;
    barrier();
    uint first = cull_words.data[state + VELK_PART_TABLE_FIRST];
    uint pages = cull_words.data[cut + VELK_CUT_PAGES];
    // The table that was current last frame, which the begin pass made the last one.
    uint table = velk_cut_table(cut, false) + first;
    if (first + velk_cut_pages(count) <= pages) {
        for (uint r = gl_LocalInvocationIndex; r < count; r += 64u) {
            uint place = velk_cut_place(table, r);
            if (place < cull_words.data[cut + VELK_CUT_CAPACITY]
                && cull_words.data[velk_cut_record_word(cut, place) + VELK_RECORD_ENTRY] == VELK_CUT_STALE) {
                s_stale = 1u;
            }
        }
    }
    barrier();
    if (gl_LocalInvocationIndex == 0u && s_stale != 0u) cull_words.data[state + VELK_PART_CHUNK_FIRST] = 0xFFFFFFFFu;
}
