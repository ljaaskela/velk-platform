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
    uint cut_word;
    uint instances_base;
    uint slot_count;
    uint table_word;
    uint globals_base;
    float lod_max_pixels;
    uint write;
    uint items_word;
    uint objects_word;
    uint next_word;
    uint head_word;
    uint object_slots;
    uint partition_table;  // the cut's partition table, whose entries name their quad trees
    float quad_pixels;     // kQuadPixels
    float light_pixels;    // the light's level: lod_max_pixels times kLightPixels over kLodElementPixels
    uint descriptors_word; // the instanced visuals' item descriptors (kItemDescriptorHeaderWords)
    uint descriptor_count;
} pc;

#define VELK_LOD_LIGHT
