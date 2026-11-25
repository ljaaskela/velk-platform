#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint cut_word;
    uint pairs_word;
    uint pair_count;
    uint items_word;
    uint item_count;  // the old plain items, which `items` names again one by one
    uint blocks_word; // past them, the instanced visuals' blocks: old first, how many, new first
    uint block_count;
} pc;
