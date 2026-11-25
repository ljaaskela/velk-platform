// Puts together and stores the group's point `point` traced spread, after a barrier: each light
// a ray reached blocked or not, from what its lanes added up; the rest as its first lane kept
// them. Nothing when it had no point.
void velk_points_store_spread(uint point)
{
    uint pw = s_point_word[point];
    if (pw == VELK_LIGHT_NONE) return;
    uint value = 0u;
    uint blockers = 0u;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        uint at = point * VELK_LIGHT_SLOTS + s;
        uint rays = s_slot_rays[at];
        if (rays == 0u) {
            value |= s_point_kept[point] & (15u << (4u * s));
            blockers |= s_point_kept_blockers[point] & (15u << (4u * s));
            continue;
        }
        bool blocked = s_slot_blocked[at] != 0u;
        value |= (blocked ? 15u : 0u) << (4u * s);
        blockers |= (blocked ? VELK_LIGHT_BLOCKER_BLOCKED : VELK_LIGHT_BLOCKER_CLEAR) << (4u * s);
    }
    if (value == VELK_LIGHT_NONE) value = 0xEFFFFFFFu;
    VELK_LIGHT_WORDS.data[pw] = value;
    VELK_LIGHT_WORDS.data[s_point_blocker_word[point]] = blockers;
    VELK_LIGHT_WORDS.data[pw + 1u] = s_point_state[point];
}


shared uint s_slots[VELK_LIGHT_SLOTS];
// A one value's rays, a thread each.
shared float s_blocked[VELK_LIGHT_TRACE_THREADS];

void main()
{
    uint pool = pc.spare3;
    uint index = gl_WorkGroupID.x;
    uint lane = gl_LocalInvocationIndex;
    // The lanes a point spreads its rays over at most, a ray each: a power of two up to
    // VELK_LIGHT_RAY_LANES (pc.spare1's low byte).
    uint most_lanes = clamp(pc.spare1 & 0xFFu, 1u, VELK_LIGHT_RAY_LANES);
#if VELK_LIGHT_TRACE_MODE == 1
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
#elif VELK_LIGHT_TRACE_MODE == 2
    // The point lists, their rays spread over each point's lanes, so that no thread walks all of a
    // point's rays alone. A group's barriers are the whole group's.
    {
        uint counts[VELK_LIGHT_LISTS];
        uint takes[VELK_LIGHT_LISTS];
        for (uint k = 0u; k < VELK_LIGHT_LISTS; ++k) {
            takes[k] = VELK_LIGHT_WORDS.data[pool + velk_points_take_word(k)];
            counts[k] = VELK_LIGHT_WORDS.data[pool + velk_points_count_word(k)];
        }
        uint spread = takes[0] + takes[1] + takes[2];
        uint spread_groups = VELK_LIGHT_WORDS.data[pool + VELK_POOL_SPREAD_GROUPS];
        // As the sizing chose, a power of two up to most_lanes.
        uint lanes = clamp(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SPREAD_LANES], 1u, most_lanes);
        uint per_group = VELK_LIGHT_TRACE_THREADS / lanes;
        uint group = index;
        uint frame = VELK_GLOBALS.present_counter;
        if (group < spread_groups) {
            // Spread, the group's points' sums start empty; alone, a lane each, nothing shared.
            if (lanes > 1u) {
                if (lane < VELK_LIGHT_SPREAD_POINTS) s_point_word[lane] = VELK_LIGHT_NONE;
                for (uint u = lane; u < VELK_LIGHT_SPREAD_POINTS * VELK_LIGHT_SLOTS; u += VELK_LIGHT_TRACE_THREADS) {
                    s_slot_blocked[u] = 0u;
                    s_slot_rays[u] = 0u;
                }
                memoryBarrierShared();
                barrier();
            }
            // By ray first: side by side, the same ray of neighbouring points, which mostly goes
            // to the same light, so that a wave's walks go one way.
            uint in_group = lane % per_group;
            uint point = group * per_group + in_group;
            if (point < spread) velk_points_trace_point(pool, point, frame, counts, takes, lane / per_group, lanes, in_group);
            if (lanes > 1u) {
                memoryBarrierShared();
                barrier();
                if (lane < per_group) velk_points_store_spread(lane);
            }
        }
        velk_points_count(pool);
    }
#else
    // The trace list: a one value a group.
    if (index >= min(VELK_LIGHT_WORDS.data[pool + VELK_POOL_TRACES], VELK_LIGHT_TRACE_CAPACITY)) return;
    uint w = VELK_LIGHT_WORDS.data[pool + VELK_POOL_TRACE_WORD] + index * VELK_LIGHT_TRACE_WORDS;
    uint how = VELK_LIGHT_WORDS.data[w + VELK_TRACE_ENTRY_HOW];
    uint kind = how & VELK_LIGHT_TRACE_KIND;
    if (kind == VELK_LIGHT_TRACE_SKIP) return;
    uint at = VELK_LIGHT_WORDS.data[w + VELK_TRACE_ENTRY_AT];
    uint instance = VELK_LIGHT_WORDS.data[w + VELK_TRACE_ENTRY_INSTANCE];
    uint table = VELK_LIGHT_WORDS.data[w + VELK_TRACE_ENTRY_TABLE];
    uint frame = VELK_GLOBALS.present_counter;
    GlobalData g = VELK_GLOBALS;

    // One value: from its centre, a ray a light.
    uint word;
    vec3 center;
    float radius;
    if (kind == VELK_LIGHT_TRACE_NODE) {
        word = at + VELK_QUAD_NODE_LIGHT;
        center = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[at + VELK_QUAD_NODE_CENTER]),
                      uintBitsToFloat(VELK_LIGHT_WORDS.data[at + VELK_QUAD_NODE_CENTER + 1u]),
                      uintBitsToFloat(VELK_LIGHT_WORDS.data[at + VELK_QUAD_NODE_CENTER + 2u])) - g.cam_pos.xyz;
        radius = max(uintBitsToFloat(VELK_LIGHT_WORDS.data[at + VELK_QUAD_NODE_RADIUS]), 0.0);
    } else {
        // A record's one value, or the coarse light on the first page of its own points.
        if (kind == VELK_LIGHT_TRACE_COARSE && at >= VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES]) return;
        word =kind == VELK_LIGHT_TRACE_COARSE ? velk_light_page(pool, at) + VELK_PAGE_COARSE : at + 3u;
        ElementInstance inst = velk_instances.data[instance];
        mat3 to_world = mat3(inst.world_matrix);
        float scale = max(max(length(to_world[0] * inst.size.x), length(to_world[1] * inst.size.y)),
                          length(to_world[2] * inst.size.z));
        uint sphere = kind == VELK_LIGHT_TRACE_MESH ? table : table + VELK_LOD_CLUSTER_OWN_SPHERE;
        vec3 local = vec3(0.5);
        if (sphere == VELK_LIGHT_NONE) {
            // A shape without a mesh's table: from the middle of its instance's box, out of
            // the sphere around it; a flat one only just off itself, having nothing to clear.
            float thickness = min(min(length(to_world[0] * inst.size.x), length(to_world[1] * inst.size.y)),
                                  length(to_world[2] * inst.size.z));
            radius = thickness <= scale * 1e-4 ? scale * 1e-3 : 0.5 * length(to_world * inst.size.xyz);
        } else {
            local = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere]), uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere + 1u]),
                         uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere + 2u]));
            radius = uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere + 3u]) * scale;
        }
        center = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g);
    }
    // Read by all threads before the first writes it, at the end.
    uint old = VELK_LIGHT_WORDS.data[word];
    if (old < VELK_LIGHT_ONE_VALUE || old >= VELK_LIGHT_PENDING) return;
    if (lane == 0u) {
        // A coarse light's are its points' lights, so that a pixel reads both by the same slots:
        // chosen here, as the points' first trace chooses them, when that has not happened yet.
        uint coarse_first = kind == VELK_LIGHT_TRACE_COARSE ? velk_light_page(pool, at) : VELK_LIGHT_NONE;
        if (coarse_first != VELK_LIGHT_NONE && VELK_LIGHT_WORDS.data[coarse_first + VELK_PAGE_SLOTS] != VELK_LIGHT_NONE) {
            for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) s_slots[s] = VELK_LIGHT_WORDS.data[coarse_first + VELK_PAGE_SLOTS + s];
        } else {
            uint chosen[VELK_LIGHT_SLOTS];
            velk_light_slots_at(center, chosen);
            for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
                s_slots[s] = chosen[s];
                if (coarse_first != VELK_LIGHT_NONE) VELK_LIGHT_WORDS.data[coarse_first + VELK_PAGE_SLOTS + s] = chosen[s];
            }
        }
        // The light the sun pass shades, for a cluster of an opaque draw or its points' coarse light:
        // its rays cast nothing (velk_points_leave_sun), its choice kept as stored. A quad node or a
        // whole mesh, whose draw this does not know, traces it.
        uint sun_kind = VELK_LIGHT_BLENDED;
        if (coarse_first != VELK_LIGHT_NONE) {
            sun_kind = VELK_LIGHT_WORDS.data[coarse_first + VELK_PAGE_KIND];
        } else if (kind == VELK_LIGHT_TRACE_CLUSTER) {
            uint entry = VELK_LIGHT_WORDS.data[at + VELK_RECORD_ENTRY];
            if (velk_cut_for_rays(entry)) entry &= ~VELK_CUT_FOR_RAYS;
            if (entry < VELK_CUT_RAY_QUAD) {
                sun_kind = (VELK_LIGHT_WORDS.data[entry + VELK_CLUSTER_ENTRY_FLAGS] & VELK_LOD_RAYS_ONLY) != 0u ? VELK_LIGHT_BLENDED : 0u;
            }
        }
        uint traced[VELK_LIGHT_SLOTS];
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) traced[s] = s_slots[s];
        if (velk_points_leave_sun(pool, sun_kind, traced)) {
            for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) s_slots[s] = traced[s];
        }
    }
    barrier();
    // Seeded from its record's one value (velk_light_seeded), whose lights these are: traced.
    if (kind == VELK_LIGHT_TRACE_COARSE && velk_light_seeded(old)) {
        if (lane == 0u) VELK_LIGHT_WORDS.data[word] = old | VELK_LIGHT_ONE_TRACED;
        return;
    }
    // Out of its own sphere toward each light, facing it, a ray a light: the first lane of each
    // eight. Traced once, and again only when something changes it.
    uint s = lane / 8u;
    uint light = s_slots[s];
    float blocked = 0.0;
    if (light != VELK_LIGHT_NONE && (lane & 7u) == 0u) {
        Light l = velk_light_at(pc.lights_base + light);
        vec3 toward = l.flags.x == 0u ? -l.direction.xyz : normalize(l.position.xyz - center);
        ++g_first_rays;
        blocked = velk_points_blocked(l, center, toward, radius * 1.01 + length(center) * 1e-4, false);
    }
    s_blocked[lane] = blocked;
    barrier();
    if (lane == 0u) {
        uint shades = 0u;
        for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
            shades |= (s_blocked[k * 8u] > 0.0 ? 7u : 0u) << (3u * k);
        }
        VELK_LIGHT_WORDS.data[word] = VELK_LIGHT_ONE_VALUE | VELK_LIGHT_ONE_TRACED | (old & (31u << VELK_LIGHT_ONE_QUEUED_SHIFT)) | shades;
    }
    velk_points_count(pool);
#endif
}
