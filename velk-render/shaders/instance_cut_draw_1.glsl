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
    uint table_word;
    uint globals_base;
    float max_distance;
    float lod_max_pixels;
    uint chosen_word;
    uint chosen_capacity;
    uint pyramid_word;     // the previous frame's depth pyramid, or 0xFFFFFFFF for none
    uint retest_word;
    uint retest_capacity;
    uint tiny_word;
    uint tiny_capacity;
    float tiny_pixels;
    uint whole;            // 1 to draw every record, in view or not (ICamera::choose_as)
    uint items_word;       // plain items records name in place of slots, or 0xFFFFFFFF (instance_cut_choose_src)
    uint quads_word;       // the view's quad list (kQuadListHeaderWords), or 0xFFFFFFFF
    uint quad_capacity;
    float quad_pad;        // what a quad's sphere grows by, for instances a motion function moves
    uint item_records_word;  // by plain item, the record of one its own pass draws (InstanceCullPass::together)
} pc;
