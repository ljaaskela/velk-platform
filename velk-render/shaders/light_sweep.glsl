#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint pool_word;
    uint begin;
} pc;
#velk_include VELK_LIGHT_POOL_HEADER_GLSL
void main()
{
    uint pool = pc.pool_word;
    uint word = gl_GlobalInvocationID.x;
    if (pc.begin != 0u) {
        if (word == 0u) {
            light_words.data[pool + VELK_POOL_LAST_RECORDS_PAGED] = light_words.data[pool + VELK_POOL_RECORDS_PAGED];
            light_words.data[pool + VELK_POOL_LAST_RECORDS_ONE_VALUE] = light_words.data[pool + VELK_POOL_RECORDS_ONE_VALUE];
            light_words.data[pool + VELK_POOL_LAST_SEARCHED] = light_words.data[pool + VELK_POOL_SEARCHED];
            light_words.data[pool + VELK_POOL_LAST_TRACES] = light_words.data[pool + VELK_POOL_TRACES];
            light_words.data[pool + VELK_POOL_TRACES] = 0u;
            light_words.data[pool + VELK_POOL_LAST_UNTRACED_PIXELS] = light_words.data[pool + VELK_POOL_UNTRACED_PIXELS];
            light_words.data[pool + VELK_POOL_UNTRACED_PIXELS] = 0u;
            // While VELK_LIGHT_READ names pixels, its debug block: the last frame's rays of the points
            // and its pending lights, then those cleared for this one; and the frame they are of,
            // which the lighting writes last in a frame, so that this begin of the next one finds it
            // still that frame's.
            const uint light_debug = light_words.data[pool + VELK_POOL_DEBUG];
            if (light_debug != 0xFFFFFFFFu) {
                light_words.data[light_debug + VELK_DEBUG_LAST_RAYS_CAST] = light_words.data[pool + VELK_POOL_RAYS_CAST];
                light_words.data[light_debug + VELK_DEBUG_LAST_RAYS_CAST + 1u] = light_words.data[light_debug + VELK_DEBUG_PENDING];
                light_words.data[light_debug + VELK_DEBUG_LAST_RAYS_CAST + 2u] = light_words.data[light_debug + VELK_DEBUG_READ_STAMP];
                light_words.data[light_debug + VELK_DEBUG_PENDING] = 0u;
            }
            // What the frame's work was charged: the lights listed for tracing and the points.
            const uint charged = light_words.data[pool + VELK_POOL_RAYS_LISTED] + light_words.data[pool + VELK_POOL_POINT_RAYS_CHARGED];
            light_words.data[pool + VELK_POOL_RAYS_LISTED] = 0u;
            light_words.data[pool + VELK_POOL_POINT_RAYS_CHARGED] = 0u;
            light_words.data[pool + VELK_POOL_LAST_UNTRACED_POINTS] = light_words.data[pool + VELK_POOL_UNTRACED_POINTS];
            light_words.data[pool + VELK_POOL_LAST_COARSE_POINTS] = light_words.data[pool + VELK_POOL_COARSE_POINTS];
            light_words.data[pool + VELK_POOL_LAST_FINE_POINTS] = light_words.data[pool + VELK_POOL_FINE_POINTS];
            light_words.data[pool + VELK_POOL_LAST_MARK_PIECES] = light_words.data[pool + VELK_POOL_MARK_PIECES];
            light_words.data[pool + VELK_POOL_LAST_MARK_DEEPEST] = light_words.data[pool + VELK_POOL_MARK_DEEPEST];
            light_words.data[pool + VELK_POOL_MARK_PIECES] = 0u;
            light_words.data[pool + VELK_POOL_MARK_DEEPEST] = 0u;
            light_words.data[pool + VELK_POOL_LAST_SPLITS] = light_words.data[pool + VELK_POOL_SPLITS];
            light_words.data[pool + VELK_POOL_SPLITS] = 0u;
            light_words.data[pool + VELK_POOL_FRAME] = light_words.data[pool + VELK_POOL_FRAME] + 1u;
            light_words.data[pool + VELK_POOL_UNTRACED_POINTS] = 0u;
            light_words.data[pool + VELK_POOL_COARSE_POINTS] = 0u;
            light_words.data[pool + VELK_POOL_FINE_POINTS] = 0u;
            light_words.data[pool + VELK_POOL_LIST_RAYS] = 0u;
            light_words.data[pool + VELK_POOL_LIST_RAYS + 1u] = 0u;
            light_words.data[pool + VELK_POOL_LIST_RAYS + 2u] = 0u;
            // For the budget in rays, which the CPU sets from the trace's time (adjust_light_rays):
            // whether the frame's work used it.
            light_words.data[pool + VELK_POOL_LAST_RAYS_CHARGED] = charged;
            light_words.data[pool + VELK_POOL_LAST_FIRST_RAYS_ASKED] = light_words.data[pool + VELK_POOL_FIRST_RAYS_ASKED];
            light_words.data[pool + VELK_POOL_FIRST_RAYS_ASKED] = 0u;
            // The frame whose walks took the most steps since the log last read it.
            if (light_words.data[pool + VELK_POOL_STEPS] > light_words.data[pool + VELK_POOL_WORST_STEPS]) {
                light_words.data[pool + VELK_POOL_WORST_RAYS] = light_words.data[pool + VELK_POOL_RAYS_CAST];
                light_words.data[pool + VELK_POOL_WORST_STEPS] = light_words.data[pool + VELK_POOL_STEPS];
                light_words.data[pool + VELK_POOL_WORST_FIRST_RAYS] = light_words.data[pool + VELK_POOL_FIRST_RAYS];
            }
            light_words.data[pool + VELK_POOL_RAYS_CAST] = 0u;
            light_words.data[pool + VELK_POOL_STEPS] = 0u;
            light_words.data[pool + VELK_POOL_FIRST_RAYS] = 0u;
            light_words.data[pool + VELK_POOL_LAST_FREE_PAGES] = light_words.data[pool + VELK_POOL_FREE_PAGES]
                - min(light_words.data[pool + VELK_POOL_PAGES_TAKEN], light_words.data[pool + VELK_POOL_FREE_PAGES]);
            light_words.data[pool + VELK_POOL_FREE_PAGES] = 0u;
            // Kept for the pool's growth, which looks only now and then.
            light_words.data[pool + VELK_POOL_MOST_PAGES_WANTED] = max(light_words.data[pool + VELK_POOL_MOST_PAGES_WANTED],
                                                                       light_words.data[pool + VELK_POOL_PAGES_WANTED]);
            light_words.data[pool + VELK_POOL_DEMAND_FRAMES] = light_words.data[pool + VELK_POOL_PAGES_WANTED] > 0u
                ? min(light_words.data[pool + VELK_POOL_DEMAND_FRAMES] + 1u, 3u) : 0u;
            light_words.data[pool + VELK_POOL_PAGES_WANTED] = 0u;
            light_words.data[pool + VELK_POOL_RECORDS_PAGED] = 0u;
            light_words.data[pool + VELK_POOL_RECORDS_ONE_VALUE] = 0u;
            light_words.data[pool + VELK_POOL_PAGES_TAKEN] = 0u;
            light_words.data[pool + VELK_POOL_SEARCHED] = 0u;
        }
        return;
    }
    uint pages = light_words.data[pool + VELK_POOL_PAGES];
    if (word * 32u >= pages) return;
    uint marks = light_words.data[pool + VELK_POOL_MARKS_WORD] + word;
    uint used = light_words.data[marks];
    uint free_pages = 0u;
    for (uint b = 0u; b < 32u && word * 32u + b < pages; ++b) {
        if ((used & (1u << b)) == 0u) ++free_pages;
    }
    uint at = light_words.data[pool + VELK_POOL_STACK_WORD] + atomicAdd(light_words.data[pool + VELK_POOL_FREE_PAGES], free_pages);
    for (uint b = 0u; b < 32u && word * 32u + b < pages; ++b) {
        if ((used & (1u << b)) == 0u) light_words.data[at++] = word * 32u + b;
    }
    light_words.data[marks] = 0u;
    // A free page is no piece.
    light_words.data[light_words.data[pool + VELK_POOL_PIECES_WORD] + word] &= used;
}
