#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct CullInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer CullInstances { CullInstance data[]; } cull_instances;
layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;
layout(set = 1, binding = 12, std430) readonly buffer LodNodes { uint data[]; } lod_nodes;

layout(push_constant) uniform PC {
    uint instances_base;
    uint slot_count;
    uint table_word;
    uint globals_base;
    float max_distance;
    float lod_max_pixels;
    uint items_word;
    uint chosen_word;
    uint chosen_capacity;
    uint pyramid_word;     // the previous frame's depth pyramid, or 0xFFFFFFFF for none
    uint retest_word;
    uint retest_capacity;
    uint tiny_word;        // the list of tiny clusters, see kTinyHeaderWords
    uint tiny_capacity;
    float tiny_pixels;     // 0 for none
} pc;
