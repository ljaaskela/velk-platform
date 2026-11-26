#define VELK_LIGHT_WORDS velk_light_cell_words
#velk_include VELK_LIGHT_POOL_GLSL
#velk_include VELK_LIGHT_SLOTS_GLSL
#define VELK_CHANGES_MAX 128u
// kLightChangeSmall.
#define VELK_CHANGE_SMALL 0x40000000u

vec3 velk_change_vec3(uint at)
{
    return vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[at]), uintBitsToFloat(VELK_LIGHT_WORDS.data[at + 1u]),
                uintBitsToFloat(VELK_LIGHT_WORDS.data[at + 2u]));
}

// Whether the way from a sphere at `center` of `radius` to `light` passes through the box from
// `lo` to `hi`, all relative to the camera: a ray toward a light without a place, else the
// segment to it.
bool velk_change_in_way(vec3 center, float radius, Light light, vec3 lo, vec3 hi)
{
    lo -= vec3(radius);
    hi += vec3(radius);
    vec3 dir;
    float t_max;
    if (light.flags.x == 0u) {
        dir = -light.direction.xyz;
        t_max = 1e30;
    } else {
        vec3 to = light.position.xyz - center;
        t_max = length(to);
        dir = to / max(t_max, 1e-6);
    }
    vec3 inv = 1.0 / (abs(dir) + vec3(1e-12)) * sign(dir + vec3(1e-30));
    vec3 t0 = (lo - center) * inv;
    vec3 t1 = (hi - center) * inv;
    vec3 near = min(t0, t1);
    vec3 far = max(t0, t1);
    float enter = max(max(near.x, near.y), near.z);
    float leave = min(min(far.x, far.y), far.z);
    return leave >= max(enter, 0.0) && enter <= t_max;
}

// A change's light (VELK_CHANGE_LIGHT) without its VELK_CHANGE_SMALL, VELK_LIGHT_NONE for a box.
uint velk_change_light(uint word)
{
    return word == VELK_LIGHT_NONE ? word : word & ~VELK_CHANGE_SMALL;
}

// The slots of `slots` whose light a change this frame could alter at a sphere at `center`
// (relative to the camera) of `radius`, a bit each: a change of that light, or a box in the way
// to it. `hard` those of them a change other than a small move of their light alters
// (VELK_CHANGE_SMALL): where those are all of a record's lights, its light is traced again from
// none.
uint velk_change_slots(uint changes, vec3 center, float radius, uint slots[VELK_LIGHT_SLOTS], out uint hard)
{
    vec3 cam = VELK_GLOBALS.cam_pos.xyz;
    uint lit = 0u;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        if (slots[s] != VELK_LIGHT_NONE) lit |= 1u << s;
    }
    uint altered = 0u;
    hard = 0u;
    uint count = min(VELK_LIGHT_WORDS.data[changes + VELK_CHANGE_LIST_COUNT], VELK_CHANGES_MAX);
    for (uint k = 0u; k < count && hard != lit; ++k) {
        uint w = changes + VELK_CHANGE_LIST_CHANGES + k * VELK_CHANGE_WORDS;
        uint word = VELK_LIGHT_WORDS.data[w + VELK_CHANGE_LIGHT];
        uint light = velk_change_light(word);
        bool small = light != VELK_LIGHT_NONE && (word & VELK_CHANGE_SMALL) != 0u;
        vec3 lo = velk_change_vec3(w + VELK_CHANGE_MIN) - cam;
        vec3 hi = velk_change_vec3(w + VELK_CHANGE_MAX) - cam;
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
            if (slots[s] == VELK_LIGHT_NONE || (hard & (1u << s)) != 0u) continue;
            bool hit = light != VELK_LIGHT_NONE
                         ? slots[s] == light
                         : velk_change_in_way(center, radius, velk_light_at(pc.lights_base + slots[s]), lo, hi);
            if (!hit) continue;
            altered |= 1u << s;
            if (!small) hard |= 1u << s;
        }
    }
    return altered;
}

// Whether a change this frame could alter the light of a sphere at `center` (relative to the
// camera) of `radius`, which uses the lights `slots`.
bool velk_change_alters(uint changes, vec3 center, float radius, uint slots[VELK_LIGHT_SLOTS])
{
    uint hard;
    return velk_change_slots(changes, center, radius, slots, hard) != 0u;
}

// The slots of `slots` with a light, a bit each.
uint velk_change_lit(uint slots[VELK_LIGHT_SLOTS])
{
    uint lit = 0u;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        if (slots[s] != VELK_LIGHT_NONE) lit |= 1u << s;
    }
    return lit;
}

// Blocker codes `codes` with the slots of `bits` made due (VELK_LIGHT_BLOCKER_DUE): what the
// point has of them stays until their next ray replaces it.
uint velk_change_stale(uint codes, uint bits)
{
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        if ((bits & (1u << s)) != 0u) codes &= ~(15u << (4u * s));
    }
    return codes;
}

// How much `light` could bring some place of a sphere at `center` (relative to the camera) of
// `radius`, as velk_light_strength judges a place: from the sphere's nearest point, and for a spot
// light within its cone widened by the sphere, or anywhere when the light is inside it.
float velk_change_strength(Light light, vec3 center, float radius)
{
    float bright = dot(light.color_intensity.rgb, vec3(0.2126, 0.7152, 0.0722)) * light.color_intensity.a;
    if (light.flags.x == 0u) return bright;
    vec3 to = light.position.xyz - center;
    float d = length(to);
    float dist = max(d - radius, 0.0);
    float range = light.params.x;
    if (range > 0.0 && dist >= range) return 0.0;
    if (light.flags.x == 2u && d > radius && dot(-to / d, light.direction.xyz) <= light.params.z - radius / d) return 0.0;
    float t = range > 0.0 ? 1.0 - dist / range : 1.0 / (1.0 + dist * dist);
    return bright * t * t;
}

// Puts each light a change this frame names that could now reach a sphere at `center` of `radius`
// and is not among `slots` (a record's PAGE_SLOTS, all chosen) into them: a free slot, else in
// place of the weakest when it is stronger, all judged by velk_change_strength. One among them
// that now brings nothing (gone dark, or out of reach) leaves its slot free, so that its points
// cast it no more rays. True when a slot changed.
bool velk_change_enters(uint changes, vec3 center, float radius, inout uint slots[VELK_LIGHT_SLOTS])
{
    if (slots[0] == VELK_LIGHT_NONE) return false;
    bool entered = false;
    uint count = min(VELK_LIGHT_WORDS.data[changes + VELK_CHANGE_LIST_COUNT], VELK_CHANGES_MAX);
    for (uint k = 0u; k < count; ++k) {
        uint light = velk_change_light(VELK_LIGHT_WORDS.data[changes + VELK_CHANGE_LIST_CHANGES + k * VELK_CHANGE_WORDS + VELK_CHANGE_LIGHT]);
        if (light == VELK_LIGHT_NONE) continue;
        float strength = velk_change_strength(velk_light_at(pc.lights_base + light), center, radius);
        bool kept = false;
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
            if (slots[s] != light) continue;
            kept = true;
            if (strength <= 0.0) {
                slots[s] = VELK_LIGHT_NONE;
                entered = true;
            }
        }
        if (kept || strength <= 0.0) continue;
        uint weakest = 0u;
        float least = 3.4e38;
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
            float st = slots[s] == VELK_LIGHT_NONE ? -1.0 : velk_change_strength(velk_light_at(pc.lights_base + slots[s]), center, radius);
            if (st < least) {
                least = st;
                weakest = s;
            }
        }
        if (strength <= least) continue;
        slots[weakest] = light;
        entered = true;
    }
    // The lights it keeps first, so that a free first slot still means none chosen yet.
    if (entered) {
        uint kept = 0u;
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
            uint light = slots[s];
            slots[s] = VELK_LIGHT_NONE;
            if (light != VELK_LIGHT_NONE) slots[kept++] = light;
        }
    }
    return entered;
}


// A one value at `word` traced again on its next listing.
void velk_change_one(uint word)
{
    uint value = VELK_LIGHT_WORDS.data[word];
    if (value >= VELK_LIGHT_ONE_VALUE && value < VELK_LIGHT_PENDING) atomicAnd(VELK_LIGHT_WORDS.data[word], ~VELK_LIGHT_ONE_TRACED);
}

// A coarse light (VELK_PAGE_COARSE) traced again on its next listing, from nothing: no pixel
// reads it until it is traced, and light left in it would read as seeded (velk_light_seeded).
void velk_change_coarse(uint word)
{
    uint value = VELK_LIGHT_WORDS.data[word];
    if (value >= VELK_LIGHT_ONE_VALUE && value < VELK_LIGHT_PENDING) {
        atomicAnd(VELK_LIGHT_WORDS.data[word], ~(VELK_LIGHT_ONE_TRACED | VELK_LIGHT_ONE_SHADE_BITS));
    }
}

// The pieces pass (pc.mode 1), after the records': a thread a page of the pool. A piece of a
// record a change altered this frame is stale only where the change is in its own way: its
// triangle's part, placed by the record's instance, in a sphere a little wider than its points
// reach, tested as the record was.
void velk_change_pieces(uint pool, uint changes, uint page)
{
    if (page >= VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES] || !velk_light_is_piece(pool, page)) return;
    uint at = velk_light_page(pool, page);
    uint owner = VELK_LIGHT_WORDS.data[at + VELK_PIECE_OWNER];
    if ((owner & 0xFFFFFFu) >= VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES]) return;
    uint first = velk_light_page(pool, owner & 0xFFFFFFu);
    GlobalData g = VELK_GLOBALS;
    uint now = g.present_counter & 0x0FFFFFFFu;
    if (VELK_LIGHT_WORDS.data[first + VELK_PAGE_TOUCHED] != now) return;
    uint instance = VELK_LIGHT_WORDS.data[first + VELK_PAGE_INSTANCE];
    uint slot = owner >> 24u;
    if (instance == VELK_LIGHT_NONE || slot >= velk_light_pieced(first)) return;
    ElementInstance inst = velk_instances.data[instance];
    uint cluster = VELK_LIGHT_WORDS.data[first + VELK_PAGE_CLUSTER];
    uint place = VELK_LIGHT_WORDS.data[at + VELK_PIECE_PLACE];
    uint n = velk_piece_level(place);
    uint triangle = slot;
    if (triangle >= VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_TRIANGLES]) return;
    uint vertex_word = VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_VERTEX];
    uint first_index = VELK_LIGHT_WORDS.data[first + VELK_PAGE_INDEX] + VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_INDEX];
    vec3 v[3];
    for (uint k = 0u; k < 3u; ++k) {
        uint vw = vertex_word + velk_mesh_words.data[first_index + triangle * 3u + k] * VELK_VERTEX3D_WORDS;
        vec3 local = vec3(uintBitsToFloat(velk_mesh_words.data[vw]), uintBitsToFloat(velk_mesh_words.data[vw + 1u]),
                          uintBitsToFloat(velk_mesh_words.data[vw + 2u]));
        v[k] = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g);
    }
    vec3 b0 = velk_piece_weights(place, uvec2(0u, 0u));
    vec3 b1 = velk_piece_weights(place, uvec2(n, 0u));
    vec3 b2 = velk_piece_weights(place, uvec2(0u, n));
    vec3 p0 = b0.x * v[0] + b0.y * v[1] + b0.z * v[2];
    vec3 p1 = b1.x * v[0] + b1.y * v[1] + b1.z * v[2];
    vec3 p2 = b2.x * v[0] + b2.y * v[1] + b2.z * v[2];
    vec3 center = (p0 + p1 + p2) / 3.0;
    float radius = max(max(length(p0 - center), length(p1 - center)), length(p2 - center)) * (1.0 + 0.5 / float(n));
    uint slots[VELK_LIGHT_SLOTS];
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) slots[s] = VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s];
    uint hard;
    uint altered = velk_change_slots(changes, center, radius, slots, hard);
    if (altered == 0u) return;
    // All its lights, not all by small moves, or all its record's: its points stale, traced from
    // none again. Else those lights due at its points (velk_change_stale), which keep the rest.
    if ((altered == velk_change_lit(slots) && hard != 0u) || VELK_LIGHT_WORDS.data[first + VELK_PAGE_CHANGED] == now) {
        VELK_LIGHT_WORDS.data[velk_piece_changed_word(pool, page)] = now;
        return;
    }
    for (uint q = 0u; q < VELK_PIECE_POINTS; ++q) {
        uint bw = velk_light_page_blocker(pool, page, 2u * q);
        VELK_LIGHT_WORDS.data[bw] = velk_change_stale(VELK_LIGHT_WORDS.data[bw], altered);
    }
}

void main()
{
    uint pool = pc.spare3;
    uint changes = pc.spare4;
    if (pc.mode == 1u) {
        velk_change_pieces(pool, changes, gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex);
        return;
    }
    uint cut_entry = pc.spare1 + VELK_RAY_TOP_HEADER_WORDS + pc.spare2 * VELK_RAY_TOP_CUT_WORDS;
    uint cut = VELK_LIGHT_WORDS.data[cut_entry + VELK_RAY_TOP_CUT_CUT];
    uint instances_base = VELK_LIGHT_WORDS.data[cut_entry + VELK_RAY_TOP_CUT_INSTANCES_BASE];
    uint items_word = VELK_LIGHT_WORDS.data[cut_entry + VELK_RAY_TOP_CUT_ITEMS];
    // A record of the pool a thread, a page a group (64 records): the prelude's groups are 8 by
    // 8. Only those a partition holds.
    uint record;
    uint state;
    uint at;
    if (!velk_cut_live_record(cut, gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex, record, state, at)) return;
    uint entry = VELK_LIGHT_WORDS.data[record + VELK_RECORD_ENTRY];
    uint cluster = VELK_LIGHT_WORDS.data[record + VELK_RECORD_CLUSTER];
    uint slot = VELK_LIGHT_WORDS.data[record + VELK_RECORD_SLOT];
    uint value = VELK_LIGHT_WORDS.data[record + VELK_RECORD_LIGHT];
    bool all = VELK_LIGHT_WORDS.data[changes + VELK_CHANGE_LIST_ALL] != 0u;
    vec3 cam = VELK_GLOBALS.cam_pos.xyz;
    uint slots[VELK_LIGHT_SLOTS];

    // A quad: its light is on the node above it as wide as the light spacing, wherever the
    // camera is, so its node and all above it.
    if (entry == VELK_CUT_QUAD) {
        vec3 center = velk_change_vec3(cluster + VELK_QUAD_NODE_CENTER) - cam;
        float radius = max(uintBitsToFloat(VELK_LIGHT_WORDS.data[cluster + VELK_QUAD_NODE_RADIUS]), 0.0);
        velk_light_slots_at(center, slots);
        if (!all && !velk_change_alters(changes, center, radius, slots)) return;
        uint tree = slot;
        uint i = (cluster - tree) / VELK_QUAD_NODE_WORDS;
        for (uint step = 0u; step < 32u; ++step) {
            velk_change_one(tree + i * VELK_QUAD_NODE_WORDS + VELK_QUAD_NODE_LIGHT);
            if (i == 0u) break;
            i = (i - 1u) / 2u;
        }
        return;
    }

    // Light of its own: points on pages, or a one value.
    bool own_points = false;
    uint first = 0u;
    if (value < VELK_LIGHT_ONE_VALUE) {
        if (value >= VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES]) return;
        first = velk_light_page(pool, value);
        own_points = true;
    } else if (value >= VELK_LIGHT_PENDING) {
        return;
    }

    // Where it is: its cluster's own sphere, or a whole mesh's, or an item's instance box.
    uint instance = instances_base + slot;
    if (items_word != VELK_LIGHT_NONE) instance = VELK_LIGHT_WORDS.data[items_word + slot * VELK_PLAIN_ITEM_WORDS];
    if (instance == VELK_LIGHT_NONE) return;
    ElementInstance inst = velk_instances.data[instance];
    mat3 to_world = mat3(inst.world_matrix);
    float scale = max(max(length(to_world[0] * inst.size.x), length(to_world[1] * inst.size.y)), length(to_world[2] * inst.size.z));
    vec3 local;
    float radius;
    if (entry == VELK_LIGHT_NONE && cluster == VELK_LIGHT_NONE) {
        local = vec3(0.5);
        radius = 0.5 * length(to_world * inst.size.xyz);
    } else {
        uint sphere = entry != VELK_LIGHT_NONE
                        ? VELK_LIGHT_WORDS.data[(entry & ~VELK_CUT_FOR_RAYS) + VELK_CLUSTER_ENTRY_TABLE] + cluster * VELK_LOD_CLUSTER_WORDS + VELK_LOD_CLUSTER_OWN_SPHERE
                        : cluster;
        local = velk_change_vec3(sphere);
        radius = uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere + 3u]) * scale;
    }
    vec3 center = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, VELK_GLOBALS);

    if (own_points) {
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) slots[s] = VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s];
    } else {
        velk_light_slots_at(center, slots);
    }
    // Own points keep the lights chosen at their first trace: a light that moved into reach
    // takes a slot, and all its points are traced again. One values choose theirs as they are read.
    bool entered = own_points && !all && velk_change_enters(changes, center, radius, slots);
    if (entered) {
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s] = slots[s];
        // For the log, as the trace's choice counts: the lights it could have at its centre.
        uint could[VELK_LIGHT_SLOTS];
        velk_light_slots_at(center, could);
        atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_SLOT_CHOICES], 1u);
        if (g_slots_left_out >= 0.1 * g_slots_strongest && g_slots_left_out > 0.0) {
            atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_LEFT_OUT_TENTH], 1u);
        }
        if (g_slots_left_out >= 0.25 * g_slots_strongest && g_slots_left_out > 0.0) {
            atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_LEFT_OUT_QUARTER], 1u);
        }
    }
    uint hard = 0u;
    uint altered = all || entered ? velk_change_lit(slots) : velk_change_slots(changes, center, radius, slots, hard);
    if (!all && !entered && altered == 0u) return;
    atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_CHANGED], 1u);
    uint lit = velk_change_lit(slots);
    if (own_points && !all && !entered && (altered != lit || hard == 0u)) {
        // Some of its lights, or all only by small moves: those due at its vertices' points
        // (velk_change_stale), which keep what they have until their rays; its pieces likewise as
        // far as the change is in their way (the pieces pass, pc.mode 1). Its coarse light stays:
        // its points are trusted, and a coarse light's rays every frame a light moves would take
        // the budget from them.
        uint now = VELK_GLOBALS.present_counter & 0x0FFFFFFFu;
        VELK_LIGHT_WORDS.data[first + VELK_PAGE_INSTANCE] = instance;
        VELK_LIGHT_WORDS.data[first + VELK_PAGE_TOUCHED] = now;
        uint points = velk_light_own_points(first);
        for (uint p = 0u; p < points; ++p) {
            uint bw = velk_light_blocker_word(pool, first, p);
            VELK_LIGHT_WORDS.data[bw] = velk_change_stale(VELK_LIGHT_WORDS.data[bw], altered);
        }
        return;
    }
    if (own_points) {
        // Its coarse light traced again at once, and its points and its pieces' stale
        // (velk_light_point_traces): pixels read the light under them, its coarse pieces' points
        // traced again this frame as pixels read them, the rest from none as the budget allows.
        // When everything changed, their lights chosen again too: what changed may be one of them.
        // A light that came into reach is new to every point too. Else its vertices' points are
        // stale, and its pieces' as far as the change is in their way (the pieces pass, pc.mode 1,
        // which finds its instance here).
        velk_change_coarse(first + VELK_PAGE_COARSE);
        uint now = VELK_GLOBALS.present_counter & 0x0FFFFFFFu;
        if (all || entered) VELK_LIGHT_WORDS.data[first + VELK_PAGE_RESET] = now;
        if (all) {
            for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s] = VELK_LIGHT_NONE;
        } else if (!entered) {
            VELK_LIGHT_WORDS.data[first + VELK_PAGE_INSTANCE] = instance;
            VELK_LIGHT_WORDS.data[first + VELK_PAGE_CHANGED] = now;
            VELK_LIGHT_WORDS.data[first + VELK_PAGE_TOUCHED] = now;
        }
    } else {
        velk_change_one(record + VELK_RECORD_LIGHT);
    }
}
