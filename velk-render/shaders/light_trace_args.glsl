#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer VelkLightCellWords { uint data[]; } velk_light_cell_words;
#velk_include VELK_DEFERRED_PUSH_CONSTANTS_GLSL
#define VELK_LIGHT_WORDS velk_light_cell_words
#velk_include VELK_LIGHT_POOL_GLSL
#velk_include VELK_LIGHT_TRACE_COMMON_GLSL

// The points of each list (velk_points_list's order) the frame has, and those it traces: each
// list taking what it can of what the budget in rays has left after the trace list's lights
// (pool header 85 of 106) and the lists before it, a point charged the rays its list's points are
// due on average (VELK_POOL_LIST_RAYS): a ray each light it is due.
void velk_points_takes(uint pool, out uint counts[VELK_LIGHT_LISTS], out uint takes[VELK_LIGHT_LISTS])
{
    uint budget = VELK_LIGHT_WORDS.data[pool + VELK_POOL_RAY_BUDGET];
    uint left = budget - min(VELK_LIGHT_WORDS.data[pool + VELK_POOL_RAYS_LISTED], budget);
    uint taken = 0u;
    uint costs[VELK_LIGHT_LISTS];
    for (uint k = 0u; k < VELK_LIGHT_LISTS; ++k) {
        uint list = velk_points_list(k);
        uint listed = VELK_LIGHT_WORDS.data[pool + velk_light_list_count_word(list)];
        counts[k] = min(listed, VELK_LIGHT_POINT_CAPACITY);
        uint rays = VELK_LIGHT_WORDS.data[pool + VELK_POOL_LIST_RAYS + list];
        costs[k] = clamp((rays + max(listed, 1u) - 1u) / max(listed, 1u), 1u, VELK_LIGHT_SLOTS);
        takes[k] = min(counts[k], left / costs[k]);
        left -= takes[k] * costs[k];
        taken += takes[k];
    }
    // Some every frame, so that light always moves on: the first that has any, a few hundred rays'
    // worth of its points.
    if (taken == 0u) {
        for (uint k = 0u; k < VELK_LIGHT_LISTS; ++k) {
            if (counts[k] != 0u) {
                takes[k] = min(counts[k], clamp(256u / costs[k], 8u, 64u));
                break;
            }
        }
    }
}

void main()
{
    uint pool = pc.spare3;
    uint lane = gl_LocalInvocationIndex;
    // The lanes a point spreads its rays over at most, a ray each: a power of two up to
    // VELK_LIGHT_RAY_LANES (pc.spare1's low byte).
    uint most_lanes = clamp(pc.spare1 & 0xFFu, 1u, VELK_LIGHT_RAY_LANES);
    // The dispatches' sizes (a group): the trace list's, a group a one value, and the point
    // lists' from the points each traces this frame (velk_points_takes), kept for them.
    {
        if (lane != 0u) return;
        uint counts[VELK_LIGHT_LISTS];
        uint takes[VELK_LIGHT_LISTS];
        velk_points_takes(pool, counts, takes);
        // Points spread their rays over as many lanes as keep the threads near what fills the
        // device (a power of two up to most_lanes): fewer threads would leave it waiting on the
        // longest of them walking its rays in a row, more only repeat each point's setup.
        uint points = takes[0] + takes[1] + takes[2];
        uint fill = VELK_LIGHT_WORDS.data[pool + VELK_POOL_FILL_THREADS];
        uint lanes = 1u;
        while (lanes < most_lanes && points * lanes * 2u <= fill) lanes *= 2u;
        uint per_group = VELK_LIGHT_TRACE_THREADS / lanes;
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_SPREAD_LANES] = lanes;
        // No more points than a dispatch's groups hold: those past it wait, the latest lists'.
        uint room = 65535u * per_group;
        for (uint k = 0u; k < VELK_LIGHT_LISTS; ++k) {
            takes[k] = min(takes[k], room);
            room -= takes[k];
        }
        for (uint k = 0u; k < VELK_LIGHT_LISTS; ++k) {
            VELK_LIGHT_WORDS.data[pool + velk_points_take_word(k)] = takes[k];
            VELK_LIGHT_WORDS.data[pool + velk_points_count_word(k)] = counts[k];
        }
        uint spread = takes[0] + takes[1] + takes[2];
        uint spread_groups = (spread + per_group - 1u) / per_group;
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_SPREAD_GROUPS] = spread_groups;
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_TRACE_ARGS] = min(VELK_LIGHT_WORDS.data[pool + VELK_POOL_TRACES], VELK_LIGHT_TRACE_CAPACITY);
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_TRACE_ARGS + 1u] = 1u;
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_TRACE_ARGS + 2u] = 1u;
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_POINT_ARGS] = spread_groups;
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_POINT_ARGS + 1u] = 1u;
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_POINT_ARGS + 2u] = 1u;
    }
}
