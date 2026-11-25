#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

struct LightInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer LightInstances { LightInstance data[]; } light_instances;
layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint ids_word;
    uint width;
    uint height;
    uint pool_word;
    float light_pixels;
    uint lights;  // the scene's lights, at most VELK_LIGHT_SLOTS
    uint again;   // after the search, for the records it laid out
    uint position_id;  // the G-buffer's positions, with each triangle's longest edge
} pc;

#define VELK_LIGHT_WORDS light_words

uvec2 g_coord;  // the thread's pixel

// As the resolve left them for the lighting: where the pixel is in its triangle, and from the
// G-buffer's positions, its distance and its triangle's longest edge.
vec3 velk_claim_weights() { return velk_pixel_weights(pc.ids_word, pc.width, ivec2(g_coord)); }

// Its triangle's longest edge in world units, and the pixels a world unit is where it is.
float velk_claim_tri_size()
{
    return texelFetch(velk_textures[nonuniformEXT(pc.position_id)], ivec2(g_coord), 0).w;
}

float velk_claim_px_per_world(GlobalData g)
{
    vec4 position = texelFetch(velk_textures[nonuniformEXT(pc.position_id)], ivec2(g_coord), 0);
    mat4 vp = g.view_projection;
    return length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5 / max(length(position.xyz), 1e-4);
}

float velk_claim_tri_px(GlobalData g)
{
    return velk_claim_tri_size() * velk_claim_px_per_world(g);
}

mat4 velk_claim_world(VelkDrawData d, uvec2 ids)
{
    LightInstance li = light_instances.data[d.instances_base + velk_ids_slot(d, ids)];
    return velk_sized_world(li.world_matrix, li.offset.xyz, li.size.xyz);
}
