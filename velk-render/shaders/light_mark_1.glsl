#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint cut_word;
    uint pool_word;
    uint instances_base;  // the cut's, as instance_cut_choose_src has them
    uint items_word;
    uint globals_base;    // the view's
} pc;

#define VELK_LIGHT_WORDS light_words
