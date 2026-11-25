#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(early_fragment_tests) in;

VELK_VARYINGS_IN

layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant, std430) uniform VelkPC {
    uint draw_base;
    uint pool_word;
    float light_pixels;
    uint lights;
    uint again;
} pc;

#define VELK_LIGHT_WORDS light_words

uvec2 g_ids;       // the fragment's, as the visibility buffer would hold them
vec3 g_weights;    // where it is in its triangle
float g_longest;   // its triangle's longest edge, in world units
