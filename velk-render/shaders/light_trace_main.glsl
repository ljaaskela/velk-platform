#define VELK_LIGHT_WORDS velk_light_cell_words
#velk_include VELK_LIGHT_POOL_GLSL
#velk_include VELK_LIGHT_SLOTS_GLSL
uint velk_points_hash(uint x)
{
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

uint g_rays = 0u;        // the rays this thread cast
uint g_rays_charged = 0u; // the rays its point was charged against the budget, cast or not
uint g_first_rays = 0u;  // of those, for lights traced for the first time
// The blocker code (velk_light_blocker_code) of the last ray velk_points_blocked found blocked.
uint g_points_code = VELK_LIGHT_BLOCKER_CLEAR;

// Whether the light of `light` is kept from `p` (facing `n`, `lift` off its surface): 1 where
// something is in the way of a ray to its centre (along its direction for a directional light),
// patches of small members on the way (VELK_CUT_RAY_QUAD) counting when they block more than
// half; 0 where nothing is. Where 1, g_points_code says how far away what was in the way is.
float velk_points_blocked(Light light, vec3 p, vec3 n, float lift, bool two_sided)
{
    velk_ray_blocker_t = 0.0;
    g_points_code = VELK_LIGHT_BLOCKER_NEAREST;
    vec3 dir;
    float t_max = 1e30;
    float dist = 0.0;
    if (light.flags.x == 0u) {
        dir = normalize(-light.direction.xyz);
    } else {
        vec3 to = light.position.xyz - p;
        dist = length(to);
        dir = to / max(dist, 1e-6);
        t_max = dist * 0.999;
    }
    // Facing away, the surface is in its own way. A stand-in for small pieces (`n` of no length,
    // velk_point_place) faces every way: its ray starts off it toward the light. One seen from both
    // sides (`two_sided`) is lit on the side the light is on, from that side.
    bool faces = dot(n, n) > 0.0;
    if (two_sided && dot(n, dir) < 0.0) n = -n;
    if (faces && dot(n, dir) <= 0.0) return 1.0;
    Ray ray;
    ray.origin = p + (faces ? n : dir) * lift;
    ray.dir = dir;
    ++g_rays;
    if (trace_visibility(ray, max(t_max - lift, 0.0)) > 0.5) {
        g_points_code = VELK_LIGHT_BLOCKER_CLEAR;
        return 0.0;
    }
    g_points_code = velk_light_blocker_code(velk_ray_blocker_t + lift, dist);
    return 1.0;
}

// This thread's rays and their walks' steps, into the frame's counts (pool header 86 and 87).
void velk_points_count(uint pool)
{
    if (g_rays != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_RAYS_CAST], g_rays);
    if (g_rays_charged != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_POINT_RAYS_CHARGED], g_rays_charged);
    if (velk_ray_steps != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_STEPS], velk_ray_steps);
    if (velk_ray_steps != 0u) atomicMax(VELK_LIGHT_WORDS.data[pool + VELK_POOL_LONGEST_WALK], velk_ray_steps);
    if (g_first_rays != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_FIRST_RAYS], g_first_rays);
    // Added up until the log reads them: triangles tested, records, rays and steps.
    if (velk_ray_tris != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_TRIANGLES], (velk_ray_tris + 15u) / 16u);
    if (velk_ray_records != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_RECORDS], velk_ray_records);
    if (velk_ray_records_hit != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_RECORDS_HIT], velk_ray_records_hit);
    if (velk_ray_hit_patches != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_HIT_BY], velk_ray_hit_patches);
    if (velk_ray_hit_quads != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_HIT_BY + 1u], velk_ray_hit_quads);
    if (velk_ray_hit_none != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_HIT_BY + 2u], velk_ray_hit_none);
    if (velk_ray_hit_clusters != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_HIT_BY + 3u], velk_ray_hit_clusters);
    if (velk_ray_hit_meshes != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_HIT_BY + 4u], velk_ray_hit_meshes);
    if (velk_ray_hit_empty != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_HIT_BY + 5u], velk_ray_hit_empty);
    for (uint b = 0u; b < 4u; ++b) {
        uint wide = ((b < 2u ? velk_ray_mesh_wide[0] : velk_ray_mesh_wide[1]) >> (16u * (b & 1u))) & 0xFFFFu;
        uint tris = ((b < 2u ? velk_ray_mesh_tris[0] : velk_ray_mesh_tris[1]) >> (16u * (b & 1u))) & 0xFFFFu;
        if (wide != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_MESH_WIDE + b], wide);
        if (tris != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_MESH_TRIS + b], tris);
    }
    if (g_rays != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_RAYS], g_rays);
    if (velk_ray_steps != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_STEPS], (velk_ray_steps + 15u) / 16u);
    if (velk_ray_deepest > 0) atomicMax(VELK_LIGHT_WORDS.data[pool + VELK_POOL_DEEPEST_TRACE], uint(velk_ray_deepest));
    // Where the walks' steps went: top tree nodes, partition tree nodes, mesh tree nodes (in
    // sixteens) and partitions entered.
    if (velk_ray_top_nodes != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_TOP_NODES], (velk_ray_top_nodes + 15u) / 16u);
    if (velk_ray_partition_nodes != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_PARTITION_NODES], (velk_ray_partition_nodes + 15u) / 16u);
    if (velk_ray_mesh_nodes != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_MESH_NODES], (velk_ray_mesh_nodes + 15u) / 16u);
    if (velk_ray_partitions != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_PARTITIONS], velk_ray_partitions);
}


// The blocker codes of the slots of `slots` without a light: VELK_LIGHT_BLOCKER_CLEAR, so that a
// code VELK_LIGHT_BLOCKER_DUE always means a light still to trace (velk_light_list_at).
uint velk_points_none_clear(uint slots[VELK_LIGHT_SLOTS])
{
    uint codes = 0u;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        if (slots[s] == VELK_LIGHT_NONE) codes |= VELK_LIGHT_BLOCKER_CLEAR << (4u * s);
    }
    return codes;
}

// Where a point of light is: a vertex of a cluster (`vertex`), or weights `b` of the corners of
// triangle `triangle`.
struct VelkPointPlace
{
    uint instance;
    uint vertex_word;
    uint first_index;
    uint triangle;
    uint vertex;
    vec3 b;
};

// The place of `at`, relative to the camera, its normal, and how far off its surface its rays
// start: a share of the light spacing where it is, so that it does not shadow itself and still
// sees what stands on it.
void velk_point_place(VelkPointPlace at, out vec3 p, out vec3 n, out float lift)
{
    GlobalData g = VELK_GLOBALS;
    ElementInstance inst = velk_instances.data[at.instance];
    vec3 local = vec3(0.0);
    vec3 normal = vec3(0.0);
    // The length its normals were authored with: shorter than unit for a stand-in for small
    // pieces (velk_stand_in_flatness), which faces every way: `n` then has no length.
    float authored = 1.0;
    if (at.vertex != VELK_LIGHT_NONE) {
        uint vw = at.vertex_word + at.vertex * VELK_VERTEX3D_WORDS;
        local = vec3(uintBitsToFloat(velk_mesh_words.data[vw]), uintBitsToFloat(velk_mesh_words.data[vw + 1u]),
                     uintBitsToFloat(velk_mesh_words.data[vw + 2u]));
        normal = vec3(uintBitsToFloat(velk_mesh_words.data[vw + 3u]), uintBitsToFloat(velk_mesh_words.data[vw + 4u]),
                      uintBitsToFloat(velk_mesh_words.data[vw + 5u]));
        authored = length(normal);
    } else {
        vec3 b = at.b;
        authored = 0.0;
        for (uint k = 0u; k < 3u; ++k) {
            uint vw = at.vertex_word + velk_mesh_words.data[at.first_index + at.triangle * 3u + k] * VELK_VERTEX3D_WORDS;
            local += b[k] * vec3(uintBitsToFloat(velk_mesh_words.data[vw]), uintBitsToFloat(velk_mesh_words.data[vw + 1u]),
                                 uintBitsToFloat(velk_mesh_words.data[vw + 2u]));
            vec3 corner_normal = vec3(uintBitsToFloat(velk_mesh_words.data[vw + 3u]), uintBitsToFloat(velk_mesh_words.data[vw + 4u]),
                                      uintBitsToFloat(velk_mesh_words.data[vw + 5u]));
            normal += b[k] * corner_normal;
            authored += b[k] * length(corner_normal);
        }
    }
    p = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g);
    n = velk_stand_in_flatness(authored) >= 0.0 ? vec3(0.0)
      : normalize(mat3(inst.world_matrix) * (normal / max(abs(inst.size.xyz), vec3(1e-6))));
    mat4 vp = g.view_projection;
    float spacing = VELK_LIGHT_PIXELS / max(length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5, 1e-6);
    lift = max(0.05 * spacing * length(p), length(p) * 1e-4);
}

#velk_include VELK_LIGHT_TRACE_COMMON_GLSL
// The most points a work group traces spread, two lanes or more each.
#define VELK_LIGHT_SPREAD_POINTS (VELK_LIGHT_TRACE_THREADS / 2u)

// What a point traced with its rays spread over its lanes leaves for the first of them to store
// (velk_points_store_spread), by its place among the group's points: where its words are
// (VELK_LIGHT_NONE for no point) and its state; and by its light slot, what its lanes' rays to
// that light found: the rays something blocked, and the light's rays (0 for no ray).
shared uint s_point_word[VELK_LIGHT_SPREAD_POINTS];
shared uint s_point_blocker_word[VELK_LIGHT_SPREAD_POINTS];
shared uint s_point_state[VELK_LIGHT_SPREAD_POINTS];
// And what its lights no ray reached keep: their light and blockers, a nibble a slot.
shared uint s_point_kept[VELK_LIGHT_SPREAD_POINTS];
shared uint s_point_kept_blockers[VELK_LIGHT_SPREAD_POINTS];
shared uint s_slot_blocked[VELK_LIGHT_SPREAD_POINTS * VELK_LIGHT_SLOTS];
// And of those, the nearest blocker code (velk_light_blocker_code), VELK_LIGHT_BLOCKER_CLEAR for none.
shared uint s_slot_code[VELK_LIGHT_SPREAD_POINTS * VELK_LIGHT_SLOTS];
shared uint s_slot_rays[VELK_LIGHT_SPREAD_POINTS * VELK_LIGHT_SLOTS];


// A point's trace state with it traced: its top 4 bits 1 (velk_light_point_traces).
#define VELK_LIGHT_STATE_TRACED 0x10000000u
// The trace state of a traced point listed in the frame `state` holds, or in `frame` when it
// was never listed.
uint velk_points_traced_state(uint state, uint frame)
{
    return VELK_LIGHT_STATE_TRACED | (state == VELK_LIGHT_NONE ? frame & 0x0FFFFFFFu : state & 0x0FFFFFFFu);
}

// Stores a point's light `value`, its `blockers` and its trace state `state`, whose words start
// at `pw` and `bw`.
void velk_points_store(uint pw, uint bw, uint value, uint blockers, uint state)
{
    // Never the word for untraced: all eight slots fully dark reads one step lighter.
    if (value == VELK_LIGHT_NONE) value = 0xEFFFFFFFu;
    VELK_LIGHT_WORDS.data[pw] = value;
    VELK_LIGHT_WORDS.data[bw] = blockers;
    VELK_LIGHT_WORDS.data[pw + 1u] = state;
}

// Traces the point of light at place `i` of the frame's points to trace (velk_points_takes'
// `counts` and `takes`), on lane `lane` of `lanes` the point has: a ray to each of its lights that
// is due, all of them when it is not traced (or stale since a change), the rest keeping what they
// have. Alone, a lane casts all its rays and stores it; spread, each lane casts the rays whose
// place among its due lights is its lane modulo `lanes`, and adds what they found into the group's
// s_slot arrays for its place among the group's points, `point_in_group`, which
// velk_points_store_spread puts together.
void velk_points_trace_point(uint pool, uint i, uint frame, uint counts[VELK_LIGHT_LISTS], uint takes[VELK_LIGHT_LISTS], uint lane,
                             uint lanes, uint point_in_group)
{
    GlobalData g = VELK_GLOBALS;
    uint list = VELK_LIGHT_NONE;
    uint listed_points = 0u;
    uint place = i;
    for (uint k = 0u; k < VELK_LIGHT_LISTS; ++k) {
        if (place < takes[k]) {
            list = velk_points_list(k);
            listed_points = counts[k];
            break;
        }
        place -= takes[k];
    }
    if (list == VELK_LIGHT_NONE) {
        return;
    }
    place = (place + velk_points_hash(frame * 2654435761u + list) % listed_points) % listed_points;
    uint w = VELK_LIGHT_WORDS.data[pool + velk_light_list_word_word(list)] + place * VELK_LIGHT_POINT_WORDS;
    uint first = VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_FIRST];
    uint q = VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_POINT];
    uint instance = VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_INSTANCE];
    uint pages = VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES];
    // A piece's point: its page, and its record's first page and light root slot from its owner word.
    uint piece = VELK_LIGHT_NONE;
    uint piece_slot = 0u;
    q &= ~VELK_LIGHT_POINT_COARSE;
    if ((q & VELK_LIGHT_POINT_PIECE) != 0u) {
        piece = first;
        q &= ~VELK_LIGHT_POINT_PIECE;
        if (piece >= pages || q >= VELK_PIECE_POINTS) return;
        uint owner = VELK_LIGHT_WORDS.data[velk_light_page(pool, piece) + VELK_PIECE_OWNER];
        if ((owner & 0xFFFFFFu) >= pages) return;
        first = velk_light_page(pool, owner & 0xFFFFFFu);
        piece_slot = owner >> 24u;
        if (piece_slot >= velk_light_pieced(first)) return;
    }
    uint kind = VELK_LIGHT_WORDS.data[first + VELK_PAGE_KIND];
    if (piece == VELK_LIGHT_NONE && q >= velk_light_own_points(first)) return;
    uint cluster = VELK_LIGHT_WORDS.data[first + VELK_PAGE_CLUSTER];
    uint vertices = min(VELK_LIGHT_WORDS.data[first + VELK_PAGE_VERTICES], 256u);
    ElementInstance inst = velk_instances.data[instance];
    mat3 to_world = mat3(inst.world_matrix);

    // Its lights: chosen at the cluster's centre when nothing has chosen them yet, the same inputs
    // giving the same lights wherever they are chosen.
    uint slots[VELK_LIGHT_SLOTS];
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) slots[s] = VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s];
    if (slots[0] == VELK_LIGHT_NONE) {
        vec3 own = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE]),
                        uintBitsToFloat(VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 1u]),
                        uintBitsToFloat(VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 2u]));
        velk_light_slots_at(velk_rel_position(inst.world_matrix, inst.offset.xyz + own * inst.size.xyz, g), slots);
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s] = slots[s];
        // For the log: how often the lights a record could have outnumber its slots, by how strong
        // the strongest it left out is against the strongest it took.
        if (lane == 0u) {
            atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_SLOT_CHOICES], 1u);
            if (g_slots_left_out >= 0.1 * g_slots_strongest && g_slots_left_out > 0.0) {
                atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_LEFT_OUT_TENTH], 1u);
            }
            if (g_slots_left_out >= 0.25 * g_slots_strongest && g_slots_left_out > 0.0) {
                atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_LEFT_OUT_QUARTER], 1u);
            }
        }
    }

    // A vertex, or a piece's point in a triangle at weights of its corners.
    uint vertex_word = VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_VERTEX];
    uint first_index = VELK_LIGHT_WORDS.data[first + VELK_PAGE_INDEX] + VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_INDEX];
    uint pw = piece == VELK_LIGHT_NONE ? velk_light_own_word(pool, first, q) : velk_light_page(pool, piece) + 2u * q;
    uint old = VELK_LIGHT_WORDS.data[pw];
    uint state = VELK_LIGHT_WORDS.data[pw + 1u];
    // Whether it was traced, as its listing found it.
    uint traces = state == VELK_LIGHT_NONE ? 0u : min(state >> 28u, 1u);
    VelkPointPlace at;
    at.instance = instance;
    at.vertex_word = vertex_word;
    at.first_index = first_index;
    at.triangle = 0u;
    at.vertex = VELK_LIGHT_NONE;
    at.b = vec3(0.0);
    if (piece == VELK_LIGHT_NONE) {
        if (q >= vertices) return;
        at.vertex = q;
    } else {
        at.triangle = piece_slot;
        if (at.triangle >= VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_TRIANGLES]) return;
        uint place = VELK_LIGHT_WORDS.data[velk_light_page(pool, piece) + VELK_PIECE_PLACE];
        uvec2 ab = velk_piece_place(q);
        if (ab.x + ab.y > velk_piece_level(place)) return;
        at.b = velk_piece_weights(place, ab);
    }

    vec3 p;
    vec3 n;
    float lift;
    velk_point_place(at, p, n, lift);
    uint bw = piece == VELK_LIGHT_NONE ? velk_light_blocker_word(pool, first, q) : velk_light_page_blocker(pool, piece, 2u * q);
    // From none (not traced, or stale since a change): all its lights due, what it had before left
    // out. Else those its blocker codes have due (VELK_LIGHT_BLOCKER_DUE), the rest keeping theirs.
    bool from_none = traces == 0u || old == VELK_LIGHT_NONE;
    bool restart = from_none && old != VELK_LIGHT_NONE;
    uint had = from_none ? 0u : old;
    uint had_blockers = from_none ? 0u : VELK_LIGHT_WORDS.data[bw];
    // Its due lights, a bit a slot, and what the rest keep: a light that brings nothing here needs
    // no ray, nothing in its way; a slot without a light, nothing to trace, which its code says.
    uint due = 0u;
    uint kept = 0u;
    uint kept_blockers = 0u;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        uint code = (had_blockers >> (4u * s)) & 15u;
        if (slots[s] == VELK_LIGHT_NONE) {
            kept_blockers |= VELK_LIGHT_BLOCKER_CLEAR << (4u * s);
        } else if (code != VELK_LIGHT_BLOCKER_DUE) {
            kept |= had & (15u << (4u * s));
            kept_blockers |= code << (4u * s);
        } else if (velk_light_strength(velk_light_at(pc.lights_base + slots[s]), p) <= 0.0) {
            kept_blockers |= VELK_LIGHT_BLOCKER_CLEAR << (4u * s);
        } else {
            due |= 1u << s;
        }
    }
    // A ray each due light, numbered by its place among them; spread, each lane casts those whose
    // number is its lane modulo `lanes`.
    bool spread = lanes > 1u;
    uint slot_base = point_in_group * VELK_LIGHT_SLOTS;
    uint value = kept;
    uint blockers = kept_blockers;
    uint r = 0u;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        if ((due & (1u << s)) == 0u) continue;
        bool mine = !spread || r % lanes == lane;
        ++r;
        if (!mine) continue;
        if (!restart && from_none) ++g_first_rays;
        bool blocked = velk_points_blocked(velk_light_at(pc.lights_base + slots[s]), p, n, lift, (kind & VELK_LIGHT_TWO_SIDED) != 0u) > 0.0;
        if (spread) {
            atomicAdd(s_slot_rays[slot_base + s], 1u);
            if (blocked) {
                atomicAdd(s_slot_blocked[slot_base + s], 1u);
                atomicMin(s_slot_code[slot_base + s], g_points_code);
            }
        } else {
            value |= (blocked ? 15u : 0u) << (4u * s);
            blockers |= (blocked ? g_points_code : VELK_LIGHT_BLOCKER_CLEAR) << (4u * s);
        }
    }
    if (lane == 0u) {
        atomicAdd(VELK_LIGHT_WORDS.data[pool + (!from_none ? VELK_POOL_SUM_LATER_TRACES
                                                           : restart ? VELK_POOL_SUM_RETRACES : VELK_POOL_SUM_FIRST_TRACES)], 1u);
        // What the budget charged it: its due lights (velk_light_list_at counted them so).
        g_rays_charged = r;
    }
    // Traced: every light it is due now has its ray.
    uint traced = velk_points_traced_state(state, frame);
    if (spread) {
        if (lane != 0u) return;
        s_point_word[point_in_group] = pw;
        s_point_blocker_word[point_in_group] = bw;
        s_point_state[point_in_group] = traced;
        s_point_kept[point_in_group] = kept;
        s_point_kept_blockers[point_in_group] = kept_blockers;
        return;
    }
    velk_points_store(pw, bw, value, blockers, traced);
}

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
        blockers |= (blocked ? clamp(s_slot_code[at], VELK_LIGHT_BLOCKER_NEAREST, VELK_LIGHT_BLOCKER_FARTHEST)
                             : VELK_LIGHT_BLOCKER_CLEAR) << (4u * s);
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
#if VELK_LIGHT_TRACE_MODE == 2
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
                    s_slot_code[u] = VELK_LIGHT_BLOCKER_CLEAR;
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
