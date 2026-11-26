#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint ids_word;
    uint output_id;
    uint width;
    uint height;
    uint mode;
    uint pyramid_word;
    uint chooser_globals;
    uint dim_outside;
    uint pool_word;       // the view's light pool (light_pool.h), for mode 15
    float light_pixels;   // kLightPixels
} pc;

#define VELK_LIGHT_WORDS velk_culling
