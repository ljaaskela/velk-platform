// A point's state: its top 4 bits 1 once it is traced (every one of its lights then has its ray),
// the rest the frame it was last listed.

// The word of the piece at page `page` that holds the frame something last changed what its
// points could get (light_changes_main_src), VELK_LIGHT_NONE for never: the blocker word of the
// point past its last, which no point uses.
uint velk_piece_changed_word(uint pool, uint page)
{
    return velk_light_page_blocker(pool, page, 2u * VELK_PIECE_POINTS);
}

// Whether the own point of the record whose first page starts at `first` with trace state
// `state`, a point of the piece at page `piece` or VELK_LIGHT_NONE for a vertex's, is traced: 1,
// else 0, also when its record's lights were chosen again, or something changed what it could get
// (its piece's, or its record's), since it was last listed. Such a point keeps its light word
// until it is traced again, but its light is stale: pixels read what is under it, and its next
// trace starts over.
uint velk_light_point_traces(uint pool, uint first, uint piece, uint state)
{
    if (state == VELK_LIGHT_NONE) return 0u;
    uint listed = state & 0x0FFFFFFFu;
    uint reset = VELK_LIGHT_WORDS.data[first + VELK_PAGE_RESET];
    if (reset != VELK_LIGHT_NONE && listed < reset) return 0u;
    // A piece's point: what changed in the way of that piece (velk_piece_changed_word), not of
    // its whole record.
    uint changed = piece == VELK_LIGHT_NONE ? VELK_LIGHT_WORDS.data[first + VELK_PAGE_CHANGED]
                                            : VELK_LIGHT_WORDS.data[velk_piece_changed_word(pool, piece)];
    if (changed != VELK_LIGHT_NONE && listed < changed) return 0u;
    return min(state >> 28u, 1u);
}

// The word naming triangle `t`'s root piece in the record whose first page starts at `first`,
// own points in pieces: its page, VELK_LIGHT_NONE for none yet, VELK_LIGHT_PENDING while it is
// being made.
uint velk_light_root_word(uint pool, uint first, uint t)
{
    return velk_light_word(pool, first, VELK_PAGE_ROOTS + t);
}

// Whether page `page` is a piece, as the pool's piece bits say.
bool velk_light_is_piece(uint pool, uint page)
{
    return (VELK_LIGHT_WORDS.data[VELK_LIGHT_WORDS.data[pool + VELK_POOL_PIECES_WORD] + (page >> 5u)] & (1u << (page & 31u))) != 0u;
}

// The points of light vertex `vertex` of the record whose first page starts at `first` reads,
// as the words they are in, their numbers there and their weights: its own point, the first with
// all the weight. A word is VELK_LIGHT_NONE where there is no point. `owner` gets the first page
// of the record the points are in, whose lights their slots are, VELK_LIGHT_NONE for none.
void velk_light_points(uint pool, uint first, uint vertex, out uint words[3], out uint ids[3], out vec3 weights, out uint owner)
{
    words[0] = VELK_LIGHT_NONE;
    words[1] = VELK_LIGHT_NONE;
    words[2] = VELK_LIGHT_NONE;
    ids[0] = vertex;
    ids[1] = 0u;
    ids[2] = 0u;
    weights = vec3(1.0, 0.0, 0.0);
    owner = VELK_LIGHT_NONE;
    if (vertex >= VELK_LIGHT_WORDS.data[first + VELK_PAGE_VERTICES]) return;
    words[0] = velk_light_own_word(pool, first, vertex);
    owner = first;
}

// A trace list entry's first word: what it is (VELK_LIGHT_TRACE_KIND bits: a VELK_LIGHT_TRACE_
// kind, or VELK_LIGHT_TRACE_SKIP for nothing).
#define VELK_LIGHT_TRACE_KIND 15u
#define VELK_LIGHT_TRACE_SKIP 15u
