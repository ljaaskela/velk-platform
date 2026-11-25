#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint cut_word;
    uint globals_base;
    uint whole;       // 1 to list every partition, in view or not
    uint quads_word;  // the view's quad list, for its count of quads in cuts, or 0xFFFFFFFF
} pc;
