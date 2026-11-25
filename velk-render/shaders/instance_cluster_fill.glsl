#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint chosen_word;
    uint chosen_capacity;
} pc;

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= min(cull_words.data[pc.chosen_word + VELK_CHOSEN_LIST_COUNT], pc.chosen_capacity)) return;
    uint record = pc.chosen_word + VELK_CHOSEN_LIST_RECORDS + index * VELK_CHOSEN_END;
    uint entry = cull_words.data[record + VELK_CHOSEN_ENTRY];
    uint at = atomicAdd(cull_words.data[cull_words.data[entry + VELK_CLUSTER_ENTRY_CURSORS] + cull_words.data[record + VELK_CHOSEN_CLUSTER]], 1u);
    uint capacity = cull_words.data[entry + VELK_CLUSTER_ENTRY_CAPACITY];
    if (at < capacity) {
        uint list = cull_words.data[entry + VELK_CLUSTER_ENTRY_LIST];
        cull_words.data[list + at] = cull_words.data[record + VELK_CHOSEN_SLOT];
        cull_words.data[list + capacity + at] = cull_words.data[record + VELK_CHOSEN_CUT_RECORD];
        cull_words.data[list + capacity * 2u + at] = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE]
                                                   + cull_words.data[record + VELK_CHOSEN_CLUSTER] * VELK_LOD_CLUSTER_WORDS;
    }
}
