#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint entry;
    uint entries_word;
    uint entry_count;
    uint chosen_word;
    uint second_phase;
} pc;

void main()
{
    uint invocation = gl_GlobalInvocationID.x;
    if (invocation >= pc.entry_count) return;
    uint entry = pc.entry;
    if (pc.entries_word != 0xFFFFFFFFu) {
        entry = cull_words.data[pc.entries_word + invocation];
    }
    if (invocation == 0u) {
        cull_words.data[pc.chosen_word + VELK_CHOSEN_LIST_GROUPS] = (cull_words.data[pc.chosen_word + VELK_CHOSEN_LIST_COUNT] + 63u) / 64u;
        cull_words.data[pc.chosen_word + VELK_CHOSEN_LIST_GROUPS + 1u] = 1u;
        cull_words.data[pc.chosen_word + VELK_CHOSEN_LIST_GROUPS + 2u] = 1u;
    }

    uint capacity = cull_words.data[entry + VELK_CLUSTER_ENTRY_CAPACITY];
    uint table = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE];
    uint clusters = cull_words.data[entry + VELK_CLUSTER_ENTRY_CLUSTERS];
    uint counts = cull_words.data[entry + VELK_CLUSTER_ENTRY_COUNTS];
    uint cursors = cull_words.data[entry + VELK_CLUSTER_ENTRY_CURSORS];
    uint commands = cull_words.data[entry + (pc.second_phase != 0u ? 11u : 6u)];

    uint first = pc.second_phase != 0u ? cull_words.data[entry + VELK_CLUSTER_ENTRY_FIRST_POSITIONS] : 0u;
    uint draws = 0u;
    for (uint c = 0u; c < clusters; ++c) {
        uint count = min(cull_words.data[counts + c], capacity - first);
        cull_words.data[counts + c] = 0u;
        cull_words.data[cursors + c] = first;
        if (count != 0u) {
            uint cluster = table + c * VELK_LOD_CLUSTER_WORDS;
            uint command = commands + draws * 5u;
            cull_words.data[command + 0u] = cull_words.data[cluster + VELK_LOD_CLUSTER_TRIANGLES] * 3u;
            cull_words.data[command + 1u] = count;
            cull_words.data[command + 2u] = cull_words.data[cluster + VELK_LOD_CLUSTER_INDEX];
            cull_words.data[command + 3u] = 0u;
            cull_words.data[command + 4u] = first;
            ++draws;
        }
        first += count;
    }
    if (pc.second_phase != 0u) {
        cull_words.data[entry + VELK_CLUSTER_ENTRY_SECOND_DRAWS] = draws;
    } else {
        cull_words.data[entry + VELK_CLUSTER_ENTRY_DRAWS] = draws;
        cull_words.data[entry + VELK_CLUSTER_ENTRY_FIRST_POSITIONS] = first;
    }
}
