#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint cut_word;
    uint globals_base;
    uint whole;       // 1 to list every partition, in view or not
    uint quads_word;  // the view's quad list, for its count of quads in cuts, or 0xFFFFFFFF
} pc;
#velk_include VELK_CUT_GLSL
#velk_include VELK_TRACE_GLSL
// Whether the box from `lo` to `hi`, relative to the camera, is outside one of the view's
// planes, tested as instance_cut_draw_src tests spheres: clip z by the wider of its two
// conventions.
bool velk_box_outside(mat4 vp, vec3 lo, vec3 hi)
{
    vec3 c = (lo + hi) * 0.5;
    vec3 e = (hi - lo) * 0.5;
    vec3 r3 = vec3(vp[0][3], vp[1][3], vp[2][3]);
    for (int k = 0; k < 3; ++k) {
        vec3 rk = vec3(vp[0][k], vp[1][k], vp[2][k]);
        for (int s = 0; s < 2; ++s) {
            vec3 n = s == 0 ? r3 + rk : r3 - rk;
            float d = s == 0 ? vp[3][3] + vp[3][k] : vp[3][3] - vp[3][k];
            if (dot(n, c) + d + dot(abs(n), e) < 0.0) return true;
        }
    }
    return false;
}

void main()
{
    uint cut = pc.cut_word;
    uint index = gl_GlobalInvocationID.x;
    uint partitions = cull_words.data[cut + VELK_CUT_PARTITIONS];
    if (index >= partitions) return;
    uint state = velk_cut_state(cut, index);
    uint count = cull_words.data[state + VELK_PART_COUNT];
    if (count == 0u) return;
    uint for_rays = cull_words.data[state + VELK_PART_FOR_RAYS];
    if (for_rays != 0u) atomicAdd(cull_words.data[cut + VELK_CUT_FOR_RAYS_RECORDS], for_rays);
    uint quads = cull_words.data[state + VELK_PART_QUADS];
    if (pc.quads_word != 0xFFFFFFFFu && quads != 0u) atomicAdd(cull_words.data[pc.quads_word + VELK_QUAD_LIST_IN_CUTS], quads);

    // Traced: a partition whose box the traced pixel's ray passes, whether listed or why not.
    GlobalData g = velk_globals.data[pc.globals_base];
    vec3 lo = vec3(uintBitsToFloat(cull_words.data[state + VELK_PART_BOX]), uintBitsToFloat(cull_words.data[state + VELK_PART_BOX + 1u]),
                   uintBitsToFloat(cull_words.data[state + VELK_PART_BOX + 2u]));
    vec3 hi = vec3(uintBitsToFloat(cull_words.data[state + VELK_PART_BOX + 3u]), uintBitsToFloat(cull_words.data[state + VELK_PART_BOX + 4u]),
                   uintBitsToFloat(cull_words.data[state + VELK_PART_BOX + 5u]));
    bool boxed = all(lessThanEqual(lo, hi));
    bool traced = g.trace != VELK_TRACE_NONE && boxed && velk_trace_box(g, lo - g.cam_pos.xyz, hi - g.cam_pos.xyz);
    if (pc.whole == 0u && cull_words.data[state + VELK_PART_ALWAYS] == 0u) {
        // Nothing drawn: only records for rays only, or patches for rays.
        if (!boxed) return;
        if (velk_box_outside(g.view_projection, lo - g.cam_pos.xyz, hi - g.cam_pos.xyz)) {
            if (traced) velk_trace_note(g, VELK_TRACE_PARTITION, 1u, index, count, 0u);
            return;
        }
    }

    uint table = velk_cut_table(cut, true) + cull_words.data[state + VELK_PART_TABLE_FIRST];
    uint runs = velk_cut_pages(count);
    uint room = cull_words.data[cut + VELK_CUT_CAPACITY] / VELK_CUT_PAGE_RECORDS + partitions + 1u;
    uint at = atomicAdd(cull_words.data[cut + VELK_CUT_VISIBLE_GROUPS], runs);
    if (traced) velk_trace_note(g, VELK_TRACE_PARTITION, at + runs > room ? 2u : 0u, index, count, 0u);
    uint list = cull_words.data[cut + VELK_CUT_VISIBLE_WORD];
    for (uint r = 0u; r < runs && at + r < room; ++r) {
        uint first = cull_words.data[table + r] * VELK_CUT_PAGE_RECORDS;
        cull_words.data[list + (at + r) * 2u] = first;
        cull_words.data[list + (at + r) * 2u + 1u] = first + min(count - r * VELK_CUT_PAGE_RECORDS, VELK_CUT_PAGE_RECORDS);
    }
}
