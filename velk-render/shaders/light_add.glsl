#if defined(VELK_LIGHT_CODES) || defined(VELK_LIGHT_BLOCKERS)
// What the points a pixel read say of its record's lights, set by velk_light_add_points in the
// shaders that define VELK_LIGHT_CODES (the lighting while VELK_LIGHT_READ is set) or
// VELK_LIGHT_BLOCKERS (the shadow pack): the record's first page, VELK_LIGHT_NONE where the pixel
// read no pieces' points; a bit a slot whose light one of the points has not traced yet, or a
// change made stale; and 4 bits a slot, how far away what kept its light from the points that
// found it blocked is (velk_light_blocker_code, the nearest of theirs), VELK_LIGHT_BLOCKER_CLEAR
// where none did.
uint g_light_codes_first = 0xFFFFFFFFu;
uint g_light_pending_slots = 0u;
uint g_light_slot_codes = 0xFFFFFFFFu;

// The slot of the record velk_light_add_points read (g_light_codes_first) that holds light `i`,
// VELK_LIGHT_SLOTS for none.
uint velk_light_codes_slot(uint i)
{
    if (g_light_codes_first == VELK_LIGHT_NONE) return VELK_LIGHT_SLOTS;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        if (VELK_LIGHT_WORDS.data[g_light_codes_first + VELK_PAGE_SLOTS + s] == i) return s;
    }
    return VELK_LIGHT_SLOTS;
}
#endif

// A pixel's or a hit's lights: up to VELK_LIGHT_SLOTS of them, filled in order, the rest
// VELK_LIGHT_NONE, each with its weight and its weight times what of its light gets through.
// Every loop over them goes over all the places, so that the compiler keeps them in registers
// rather than in scratch memory.

// Adds `weight` of `light` getting `through` of its light: in its place if it has one, else the
// first free one; past VELK_LIGHT_SLOTS lights, not at all.
void velk_light_add(uint light, float weight, float through, inout uint lights[VELK_LIGHT_SLOTS],
                    inout vec2 sums[VELK_LIGHT_SLOTS])
{
    if (light == VELK_LIGHT_NONE || weight <= 0.0) return;
    bool placed = false;
    for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
        if (!placed && (lights[k] == light || lights[k] == VELK_LIGHT_NONE)) {
            lights[k] = light;
            sums[k] += vec2(weight, weight * through);
            placed = true;
        }
    }
}

// One value's lights and what of each it gets, at `center`, relative to the camera.
void velk_light_add_one(uint value, vec3 center, inout uint lights[VELK_LIGHT_SLOTS], inout vec2 sums[VELK_LIGHT_SLOTS])
{
    uint slots[VELK_LIGHT_SLOTS];
    velk_light_slots_at(center, slots);
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        float blocked = float((value >> (3u * s)) & 7u) / 7.0;
        velk_light_add(slots[s], 1.0, 1.0 - blocked, lights, sums);
    }
}

// A point of light's lights, from its word `word` in the record whose first page is `owner`,
// with `weight`: its own once it is traced, else the record's coarse light. False when neither is
// traced yet.
bool velk_light_add_point(uint word, uint owner, float weight, inout uint lights[VELK_LIGHT_SLOTS],
                          inout vec2 sums[VELK_LIGHT_SLOTS])
{
    uint shade = VELK_LIGHT_WORDS.data[word];
    uint coarse = VELK_LIGHT_WORDS.data[owner + VELK_PAGE_COARSE];
    bool has_coarse = coarse >= VELK_LIGHT_ONE_VALUE && coarse < VELK_LIGHT_PENDING && (coarse & VELK_LIGHT_ONE_TRACED) != 0u;
    if (shade == VELK_LIGHT_NONE && !has_coarse) return false;
    uint traces = velk_light_point_traces(0u, owner, VELK_LIGHT_NONE, VELK_LIGHT_WORDS.data[word + 1u]);
    float trust = shade == VELK_LIGHT_NONE ? 0.0 : float(traces);
    if (!has_coarse) trust = 1.0;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        uint light = VELK_LIGHT_WORDS.data[owner + VELK_PAGE_SLOTS + s];
        float through = 1.0 - float((coarse >> (3u * s)) & 7u) / 7.0;
        if (trust > 0.0) through = mix(through, 1.0 - float((shade >> (4u * s)) & 15u) / 15.0, trust);
        velk_light_add(light, weight, through, lights, sums);
    }
    return true;
}

// The light of the points around weights `l` of level `page` of a pixel's way down (a piece, or
// VELK_PIECE_VERTICES for the vertex points of the triangle with corners `corners`) in the record
// whose first page starts at `first`, over `under`, which holds light when `known`: each point by
// its trust, once traced fully, a point not traced yet, or stale since a change, showing what is
// under it, or left out when nothing is. True when every point it reads counts fully, so that
// what was under does not show.
bool velk_piece_light(uint pool, uint page, uint first, uvec3 corners, vec3 l, inout bool known,
                      inout float under[VELK_LIGHT_SLOTS])
{
    uint words[3];
    vec3 w;
    velk_way_around(pool, first, corners, page, l, words, w);
    float sum[VELK_LIGHT_SLOTS];
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) sum[s] = 0.0;
    float weight = 0.0;
    bool full = true;
    for (uint k = 0u; k < 3u; ++k) {
        if (w[k] <= 0.0) continue;
        uint shade = VELK_LIGHT_WORDS.data[words[k]];
        if (shade == VELK_LIGHT_NONE) {
            full = false;
            if (!known) continue;
        }
        float trust = 0.0;
        if (shade != VELK_LIGHT_NONE) {
            uint traces = velk_light_point_traces(pool, first, page == VELK_PIECE_VERTICES ? VELK_LIGHT_NONE : page,
                                                  VELK_LIGHT_WORDS.data[words[k] + 1u]);
            trust = !known ? 1.0 : min(float(traces), 1.0);
            if (trust < 1.0) full = false;
        }
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
            sum[s] += w[k] * mix(under[s], 1.0 - float((shade >> (4u * s)) & 15u) / 15.0, trust);
        }
        weight += w[k];
    }
    if (weight > 0.0) {
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) under[s] = sum[s] / weight;
        known = true;
    }
    return full;
}

// Whether the three points around weights `l` of level `page` (velk_way_around) are all traced
// and none stale since a change.
bool velk_way_fresh(uint pool, uint first, uvec3 corners, uint page, vec3 l)
{
    uint words[3];
    vec3 w;
    velk_way_around(pool, first, corners, page, l, words, w);
    for (uint k = 0u; k < 3u; ++k) {
        if (w[k] <= 0.0) continue;
        if (VELK_LIGHT_WORDS.data[words[k]] == VELK_LIGHT_NONE) return false;
        if (velk_light_point_traces(pool, first, page == VELK_PIECE_VERTICES ? VELK_LIGHT_NONE : page,
                                    VELK_LIGHT_WORDS.data[words[k] + 1u]) == 0u) {
            return false;
        }
    }
    return true;
}

// The lights of the points a pixel or hit at weights `b` of triangle `t` (corners `corners`) of
// the record whose first page starts at `first` reads, added one at a time. False when none of
// them is traced yet. `at` is relative to the camera, `px_per_world` the pixels a world unit is
// there and `tri_size` the triangle's longest edge; a hit passes 0 for both.
//
// In pieces: the level the pixel's way down reaches (velk_piece_way), over the record's coarse
// light; its coarse level where that level's cell is exact (velk_way_cell, its points traced and
// not stale). Where a point of it is not traced yet, or stale since a change, the levels above
// show through: its coarse level, then the one above it, then it. A hit, which has no size on
// screen, reads the deepest level down its triangle's pieces whose points around it are all
// traced and fresh (velk_way_fresh), over the record's coarse light; a hit whose triangle is not
// known, its triangle's corners.
bool velk_light_add_points(uint pool, uint first, uint t, uvec3 corners, vec3 b, vec3 at, float px_per_world,
                           float tri_size, inout uint lights[VELK_LIGHT_SLOTS], inout vec2 sums[VELK_LIGHT_SLOTS])
{
    bool traced = false;
    VelkLightRoot r = velk_light_root_of(first, t, b, tri_size);
    if (px_per_world <= 0.0 && r.slot != VELK_LIGHT_NONE) {
        uint pages = VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES];
        uint best = VELK_PIECE_VERTICES;
        vec3 best_at = b;
        uint page = VELK_LIGHT_WORDS.data[velk_light_root_word(pool, first, r.slot)];
        vec3 l = r.at;
        for (uint depth = 0u; depth <= VELK_PIECE_DEEPEST && page < pages; ++depth) {
            if (velk_way_fresh(pool, first, corners, page, l)) {
                best = page;
                best_at = l;
            }
            vec3 down = l;
            uint k = velk_piece_step(down);
            page = VELK_LIGHT_WORDS.data[velk_light_page(pool, page) + VELK_PIECE_CHILDREN + k];
            l = down;
        }
        uint coarse = VELK_LIGHT_WORDS.data[first + VELK_PAGE_COARSE];
        bool known = coarse >= VELK_LIGHT_ONE_VALUE && coarse < VELK_LIGHT_PENDING && (coarse & VELK_LIGHT_ONE_TRACED) != 0u;
        float under[VELK_LIGHT_SLOTS];
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) under[s] = 1.0 - float((coarse >> (3u * s)) & 7u) / 7.0;
        velk_piece_light(pool, best, first, corners, best_at, known, under);
        if (!known) return false;
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
            velk_light_add(VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s], 1.0, under[s], lights, sums);
        }
        return true;
    }
    if (px_per_world > 0.0 && r.slot != VELK_LIGHT_NONE) {
        uint root = VELK_LIGHT_WORDS.data[velk_light_root_word(pool, first, r.slot)];
        float tri_px = tri_size * px_per_world;
        VelkPieceWay way = velk_piece_way(pool, root, b, tri_px, r, r.size * px_per_world, VELK_LIGHT_PIXELS);
        if (velk_way_cell(pool, first, corners, way, true) == 1u) velk_way_stop_at_coarse(way);
        uint coarse = VELK_LIGHT_WORDS.data[first + VELK_PAGE_COARSE];
        bool coarse_known = coarse >= VELK_LIGHT_ONE_VALUE && coarse < VELK_LIGHT_PENDING && (coarse & VELK_LIGHT_ONE_TRACED) != 0u;
        float base[VELK_LIGHT_SLOTS];
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) base[s] = 1.0 - float((coarse >> (3u * s)) & 7u) / 7.0;
        float under[VELK_LIGHT_SLOTS];
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) under[s] = base[s];
        bool known = coarse_known;
        if (!velk_piece_light(pool, way.piece, first, corners, way.at, known, under)) {
            for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) under[s] = base[s];
            known = coarse_known;
            if (way.coarse != way.piece && way.coarse != way.parent) {
                velk_piece_light(pool, way.coarse, first, corners, way.coarse_at, known, under);
            }
            if (way.parent != VELK_LIGHT_NONE) {
                velk_piece_light(pool, way.parent, first, corners, way.parent_at, known, under);
            }
            velk_piece_light(pool, way.piece, first, corners, way.at, known, under);
        }
        if (!known) return false;
#if defined(VELK_LIGHT_CODES) || defined(VELK_LIGHT_BLOCKERS)
        // What the points of the level read say of each light: which have not traced it yet, and
        // how far away what kept it from those that found it blocked is.
        g_light_codes_first = first;
        g_light_pending_slots = 0u;
        g_light_slot_codes = 0xFFFFFFFFu;
        {
            uint read_words[3];
            vec3 read_w;
            velk_way_around(pool, first, corners, way.piece, way.at, read_words, read_w);
            for (uint k = 0u; k < 3u; ++k) {
                if (read_w[k] <= 0.0) continue;
                if (VELK_LIGHT_WORDS.data[read_words[k]] == VELK_LIGHT_NONE) {
                    g_light_pending_slots = (1u << VELK_LIGHT_SLOTS) - 1u;
                    continue;
                }
                uint codes = VELK_LIGHT_WORDS.data[velk_way_blocker(pool, first, corners, way.piece, k, read_words[k])];
                for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
                    uint c = (codes >> (4u * s)) & 15u;
                    if (c == VELK_LIGHT_BLOCKER_DUE) g_light_pending_slots |= 1u << s;
                    uint had = (g_light_slot_codes >> (4u * s)) & 15u;
                    if (velk_light_code_blocked(c) && c < had) {
                        g_light_slot_codes = (g_light_slot_codes & ~(15u << (4u * s))) | (c << (4u * s));
                    }
                }
            }
        }
#endif
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
            velk_light_add(VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s], 1.0, under[s], lights, sums);
        }
        return true;
    }
    for (uint k = 0u; k < 3u; ++k) {
        uint words[3];
        uint ids[3];
        vec3 w;
        uint owner;
        velk_light_points(pool, first, corners[k], words, ids, w, owner);
        if (owner == VELK_LIGHT_NONE) continue;
        for (uint j = 0u; j < 3u; ++j) {
            if (words[j] == VELK_LIGHT_NONE) continue;
            traced = velk_light_add_point(words[j], owner, b[k] * w[j], lights, sums) || traced;
        }
    }
    return traced;
}
