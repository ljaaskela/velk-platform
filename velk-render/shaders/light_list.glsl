// What lists the points the caller lists next, for the log (VELK_POOL_SUM_LISTED_BY, every 16th
// frame): a record without pieces, at its vertex points; or a triangle in pieces less than 8
// pixels across on screen, less than 64, or more.
#define VELK_LIGHT_LISTED_VERTICES 0u
#define VELK_LIGHT_LISTED_SMALL 1u
#define VELK_LIGHT_LISTED_MID 2u
#define VELK_LIGHT_LISTED_LARGE 3u
uint g_listed_by = VELK_LIGHT_LISTED_VERTICES;

// Takes `rays` of the frame's budget in rays (pool header 106) for a light on the trace list.
// A first trace may take all of it; another takes only what the last frame's first traces did
// not ask for, so that new light comes first. A light larger than all it may take goes alone,
// when it is the frame's first. False when there is no room; it is listed again on a later
// frame.
bool velk_light_take_rays(uint pool, uint rays, bool first)
{
    uint budget = VELK_LIGHT_WORDS.data[pool + VELK_POOL_RAY_BUDGET];
    uint reserved = 0u;
    if (first) {
        atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_FIRST_RAYS_ASKED], rays);
    } else {
        reserved = min(VELK_LIGHT_WORDS.data[pool + VELK_POOL_LAST_FIRST_RAYS_ASKED], budget);
        budget -= reserved;
    }
    uint before = atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_RAYS_LISTED], rays);
    if (before + rays <= budget || (before == 0u && (first || reserved == 0u))) return true;
    // Given back, so that a smaller light after it can still fit.
    atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_RAYS_LISTED], 0u - rays);
    return false;
}

// Puts a first trace of `rays` rays of a light of kind `kind` (VELK_LIGHT_TRACE_*) on the trace
// list (kLightTraceWords); nothing when the list or the frame's budget of rays is full, and it is
// listed again on a later frame.
void velk_light_list_entry(uint pool, uint kind, uint at, uint instance, uint table, uint rays)
{
    if (!velk_light_take_rays(pool, rays, true)) return;
    uint i = atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_TRACES], 1u);
    if (i >= VELK_LIGHT_TRACE_CAPACITY) return;
    uint w = VELK_LIGHT_WORDS.data[pool + VELK_POOL_TRACE_WORD] + i * VELK_LIGHT_TRACE_WORDS;
    VELK_LIGHT_WORDS.data[w + VELK_TRACE_ENTRY_HOW] = kind;
    VELK_LIGHT_WORDS.data[w + VELK_TRACE_ENTRY_AT] = at;
    VELK_LIGHT_WORDS.data[w + VELK_TRACE_ENTRY_INSTANCE] = instance;
    VELK_LIGHT_WORDS.data[w + VELK_TRACE_ENTRY_TABLE] = table;
}

// Lists the coarse light of the own points whose first page is `page`, once a frame, when it is
// due: eight rays for each of `lights`, as a first trace, so that it comes before the points' later
// traces. The frame's low bits go into it, so another pixel finds it listed.
void velk_light_list_coarse(uint pool, uint page, uint instance, uint frame, uint lights)
{
    uint first = velk_light_page(pool, page);
    uint at = first + VELK_PAGE_COARSE;
    uint value = VELK_LIGHT_WORDS.data[at];
    if (value < VELK_LIGHT_ONE_VALUE || value >= VELK_LIGHT_PENDING || !velk_light_one_due(value)) return;
    uint listed = (value & ~(31u << VELK_LIGHT_ONE_QUEUED_SHIFT)) | ((frame & 31u) << VELK_LIGHT_ONE_QUEUED_SHIFT);
    if (listed == value || atomicCompSwap(VELK_LIGHT_WORDS.data[at], value, listed) != value) return;
    // Seeded, its trace casts no rays.
    velk_light_list_entry(pool, VELK_LIGHT_TRACE_COARSE, page, instance, VELK_LIGHT_WORDS.data[first + VELK_PAGE_CLUSTER],
                          velk_light_seeded(value) ? 1u : max(lights, 1u));
}

// Puts the point of light whose words start at `word` on a point list, as `entry_first` and
// `entry_point` (VELK_POINT_ENTRY), once a frame, when it is due: not traced, or stale since a
// change (velk_light_point_traces; `first` is the first page of its record, whose changes make it
// stale), or with a light due (its blocker word `bw` has a code VELK_LIGHT_BLOCKER_DUE). By the
// order its trace comes in (VELK_LIGHT_LIST_UNTRACED and the others): of a pixel's `coarse`
// level, never traced; of it, due again; finer, never traced or due. Its trace state takes the
// frame, and whether it is traced, so that the trace reads which lights it traces: all of them, or
// those due. The rays it is due add to its list's in the pool's header (VELK_POOL_LIST_RAYS), by
// which the budget charges its list. Past the list's room it waits for a later frame.
void velk_light_list_at(uint pool, uint word, uint bw, uint first, uint entry_first, uint entry_point, uint instance,
                        uint frame, bool coarse)
{
    uint at = word + 1u;
    uint state = VELK_LIGHT_WORDS.data[at];
    uint piece = (entry_point & VELK_LIGHT_POINT_PIECE) != 0u ? entry_first : VELK_LIGHT_NONE;
    uint traces = velk_light_point_traces(pool, first, piece, state);
    uint codes = traces != 0u ? VELK_LIGHT_WORDS.data[bw] : 0u;
    if (traces != 0u && !velk_light_any_due(codes)) return;
    uint now = frame & 0x0FFFFFFFu;
    if (state != VELK_LIGHT_NONE && (state & 0x0FFFFFFFu) == now) return;
    if (atomicCompSwap(VELK_LIGHT_WORDS.data[at], state, (traces << 28u) | now) != state) return;
    // Coarse before fine, never traced or due: a new view's coarse light comes in first, the
    // finer points' over it after.
    bool never = VELK_LIGHT_WORDS.data[at - 1u] == VELK_LIGHT_NONE;
    uint list = !coarse ? VELK_LIGHT_LIST_FINE : never ? VELK_LIGHT_LIST_UNTRACED : VELK_LIGHT_LIST_COARSE;
    // Its rays: a ray each of its record's lights it is due; every light the scene could give it
    // when its record has none chosen yet.
    uint rays = 0u;
    if (VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS] == VELK_LIGHT_NONE) {
        rays = min(VELK_LIGHT_WORDS.data[pool + VELK_POOL_LIGHTS], VELK_LIGHT_SLOTS);
    } else {
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
            uint light = VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s];
            if (light != VELK_LIGHT_NONE && ((codes >> (4u * s)) & 15u) == VELK_LIGHT_BLOCKER_DUE) ++rays;
        }
    }
    atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_LIST_RAYS + list], max(rays, 1u));
    uint i = atomicAdd(VELK_LIGHT_WORDS.data[pool + velk_light_list_count_word(list)], 1u);
    if (i >= VELK_LIGHT_POINT_CAPACITY) return;
    uint w = VELK_LIGHT_WORDS.data[pool + velk_light_list_word_word(list)] + i * VELK_LIGHT_POINT_WORDS;
    VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_FIRST] = entry_first;
    VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_POINT] = entry_point;
    VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_INSTANCE] = instance;
    if ((frame & 15u) == 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_LISTED_BY + g_listed_by], 1u);
}

// Puts own point `p` of the record whose first page starts at `first` on a point list
// (velk_light_list_at), `coarse` when it is of a pixel's coarse level.
void velk_light_list_point(uint pool, uint first, uint p, uint instance, uint frame, bool coarse)
{
    if (p >= velk_light_own_points(first)) return;
    velk_light_list_at(pool, velk_light_own_word(pool, first, p), velk_light_blocker_word(pool, first, p), first, first, p,
                       instance, frame, coarse);
}

// Puts point `q` of the piece at page `page`, whose record's first page starts at `first`, on a
// point list (velk_light_list_at); a `coarse` one, of a pixel's coarse level, sampled around it
// (VELK_LIGHT_POINT_COARSE).
void velk_light_list_piece_point(uint pool, uint page, uint q, uint first, uint instance, uint frame, bool coarse)
{
    if (q >= VELK_PIECE_POINTS) return;
    uint point = q | VELK_LIGHT_POINT_PIECE | (coarse ? VELK_LIGHT_POINT_COARSE : 0u);
    velk_light_list_at(pool, velk_light_page(pool, page) + 2u * q, velk_light_page_blocker(pool, page, 2u * q), first, page,
                       point, instance, frame, coarse);
}
