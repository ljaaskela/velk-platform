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
// The clusters' triangle trees (VelkMeshNode, 16 words each) and their ray headers, for a
// cluster's box (velk_cut_record_box).
layout(set = 1, binding = 12, std430) readonly buffer LodNodes { uint data[]; } lod_nodes;
layout(set = 1, binding = 13, std430) readonly buffer ClusterWords { uint data[]; } cluster_words;

layout(push_constant) uniform PC {
    uint cut_word;
    uint instances_base;
    uint items_word;
    float pad;
    uint globals_base;   // the view's, for sizes on screen
    float light_pixels;  // the light's level, as at instance_cut_choose_src
} pc;
