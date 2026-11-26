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
#velk_include VELK_FRAGMENT_TRIANGLE_GLSL
vec3 velk_claim_weights() { return g_weights; }

float velk_claim_tri_size()
{
    return g_longest;
}

float velk_claim_px_per_world(GlobalData g)
{
    mat4 vp = g.view_projection;
    return length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5 / max(length(v_rel_pos), 1e-4);
}

float velk_claim_tri_px(GlobalData g)
{
    return velk_claim_tri_size() * velk_claim_px_per_world(g);
}

mat4 velk_claim_world(VelkDrawData d, uvec2 ids)
{
    ElementInstance inst = velk_draw_instance(d, velk_ids_slot(d, ids));
    return velk_sized_world(inst.world_matrix, inst.offset.xyz, inst.size.xyz);
}
#velk_include VELK_LIGHT_POOL_GLSL
#velk_include VELK_LIGHT_LIST_GLSL
#velk_include VELK_LIGHT_CLAIM_GLSL
void main()
{
    // A helper invocation's atomics give nothing back to go by.
    if (gl_HelperInvocation || velk_light_claim_idle()) return;
    VelkDrawData d = velk_draw_data.data[pc.draw_base];
    g_ids = uvec2(pc.draw_base + 1u, (v_instance << d.triangle_bits) | uint(gl_PrimitiveID));
    uint record = velk_ids_record(d, g_ids);
    if (record == VELK_NO_RECORD) return;
    velk_fragment_triangle(d, g_ids, v_rel_pos, g_weights, g_longest);
    velk_light_claim(d, g_ids, record);
}
