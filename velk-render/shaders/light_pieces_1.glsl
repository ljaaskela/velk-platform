#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint pool_word;
    uint phase;
} pc;

#define VELK_LIGHT_WORDS light_words
