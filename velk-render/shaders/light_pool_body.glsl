#define VELK_LIGHT_NONE 0xFFFFFFFFu
#define VELK_LIGHT_PENDING 0xFFFFFFFEu
#define VELK_LIGHT_ONE_VALUE 0x80000000u
#define VELK_LIGHT_OWN 1u
#define VELK_LIGHT_PAGE_WORDS 64u
#define VELK_LIGHT_SEGMENT_PAGE_WORDS 1048576u
#define VELK_LIGHT_DIRECT_PAGES 15u
#define VELK_LIGHT_PIECED 0x40000000u
#define VELK_LIGHT_TWO_SIDED 0x20000000u
// A page's kind: its record's draw is blended, which the sun pass does not shade
// (VELK_POOL_SUN_SKIP), so its points trace the sun as every other light.
#define VELK_LIGHT_BLENDED 0x10000000u
#define VELK_LIGHT_SLOTS 8u
#define VELK_LIGHT_ONE_TRACED 0x40000000u
#define VELK_LIGHT_ONE_ASKED 0x20000000u
#define VELK_LIGHT_ONE_SHADE_BITS 0x00FFFFFFu
#define VELK_LIGHT_ONE_QUEUED_SHIFT 24u
#define VELK_LIGHT_INHERIT 0x40000000u
// Whether a record's light `value` is kLightInherit with the page it inherits from.
bool velk_light_inherits(uint value)
{
    return value < VELK_LIGHT_ONE_VALUE && (value & VELK_LIGHT_INHERIT) != 0u;
}
// kLightPieceLevel, kLightPiecePoints, kLightPieceDeepest, kLightPieceAsked, kLightCoarsePieces,
// kLightPieceLevelsAtOnce, kLightPieceKeepFrames.
#define VELK_PIECE_LEVEL 6u
#define VELK_PIECE_POINTS 28u
// kLightPieceUnits: a multiple of every level a grid may have, 2, 3, 4 and 6 for a root.
#define VELK_PIECE_UNITS 12u
#define VELK_PIECE_DEEPEST 12u
#define VELK_PIECE_ASKED 0x10000000u
#define VELK_PIECE_FRAME_BITS 0x0FFFFFFFu
#define VELK_LIGHT_COARSE_PIECES 2.0
#define VELK_PIECE_LEVELS_AT_ONCE 4u
#define VELK_PIECE_KEEP_FRAMES 32u
// kLightPieceSameFrames, and where a piece's READ word counts them.
#define VELK_PIECE_SAME_FRAMES 7u
#define VELK_PIECE_SAME_SHIFT 29u
#define VELK_PIECE_SAME_BITS 0xE0000000u
// The level above a triangle's root in a pixel's way down (VelkPieceWay): its three vertex
// points, as far apart as the triangle is wide.
#define VELK_PIECE_VERTICES 0xFFFFFFFDu
// A point list entry's point (VELK_POINT_ENTRY_POINT) with this is a piece's point, and its
// first (VELK_POINT_ENTRY_FIRST) the piece's page.
#define VELK_LIGHT_POINT_PIECE 0x80000000u
// A piece's point listed as its pixel's coarse level (velk_light_list_at).
#define VELK_LIGHT_POINT_COARSE 0x40000000u
// A split list entry's how (VELK_SPLIT_ENTRY_HOW) with this asks for a triangle's root.
#define VELK_LIGHT_SPLIT_ROOT 0x80000000u
// With this in a split list entry's how, an ask where light differs (an edge or a penumbra), not
// one made only to bring the levels to the light spacing: its pieces have VELK_PIECE_PLACE_EDGE.
#define VELK_LIGHT_SPLIT_EDGE 0x40000000u
// From this bit of a split list entry's how, 2 bits a level from the first made under the top: the
// child the asking pixel is in, the one whose children are made next (velk_split).
#define VELK_SPLIT_PATH_SHIFT 20u
// kLightSearchCapacity, kLightSplitCapacity, kLightTraceCapacity, kLightTraceWords.
#define VELK_LIGHT_SEARCH_CAPACITY 1024u
#define VELK_LIGHT_SPLIT_CAPACITY 2048u
#define VELK_LIGHT_TRACE_CAPACITY 8192u
#define VELK_LIGHT_TRACE_WORDS VELK_TRACE_ENTRY_WORDS
// kLightPointCapacity, kLightPointWords.
#define VELK_LIGHT_POINT_CAPACITY 65536u
#define VELK_LIGHT_POINT_WORDS VELK_POINT_ENTRY_WORDS
// The point lists (velk_light_list_at), traced in this order of priority from one budget:
// points of a pixel's coarse level never traced; its points due again (the coarse level judges
// where pieces split and join, and the lighting falls back to it); points finer than it, and
// vertices only bounces read, never traced or due. Their counts and words in the pool's header.
#define VELK_LIGHT_LIST_UNTRACED 0u
#define VELK_LIGHT_LIST_COARSE 1u
#define VELK_LIGHT_LIST_FINE 2u
// The point lists there are.
#define VELK_LIGHT_LISTS 3u
// What a light on the trace list is in: a record of one value whose sphere is its cluster's, one
// whose sphere is its whole mesh's, a quad tree node.
#define VELK_LIGHT_TRACE_CLUSTER 1u
#define VELK_LIGHT_TRACE_MESH 2u
#define VELK_LIGHT_TRACE_NODE 3u
// A record's coarse light (VELK_PAGE_COARSE of the first page the entry names).
#define VELK_LIGHT_TRACE_COARSE 4u
// kLightPixels.
#define VELK_LIGHT_PIXELS 4.0

// A read pixel's terms (VELK_DEBUG_READ_TERMS), floats as their bits: its albedo, direct light,
// indirect light, the environment's diffuse light, specular light in all, area lights' diffuse
// light and the colour before tonemapping, three words each; then its metallic, whether its points
// of light were traced and probes covered it (bits 0 and 1) with the frame that wrote them (the
// bits from 8 on, its present counter), and how well the probes matched.
#define VELK_LIGHT_READ_WORDS 80u
// Then its first light: its index (VELK_LIGHT_NONE for none), how much of it gets through and the
// cosine of its way to the normal, floats as their bits; and the pixel's ids' first word. Then the
// lights' highlights alone, of all its specular light, three words; then the normal it is lit
// with, three words.
#define VELK_LIGHT_READ_FIRST_LIGHT 70u
#define VELK_LIGHT_READ_HIGHLIGHTS 74u
#define VELK_LIGHT_READ_NORMAL 77u
// Then what it reads of its triangle's pieces (velk_light_explain), from word 24: its record's
// light word, its triangle, its record's triangles in pieces, its record's coarse light, its
// triangle's root word, the level it reads (a page, or VELK_PIECE_VERTICES), that level's depth
// with its coarse cell (velk_way_cell) from bit 8 and its triangle's size on screen in pixels from
// bit 16; then for its vertex points and for the level it reads, the three points around it: their
// light words, their traces and their weights in 255ths, a byte each; then the frame each of those
// six points was last listed (VELK_PIECE_FRAME_BITS), vertex points first, the pool's frame, and
// the frame something last changed their light, the vertex points' (VELK_PAGE_CHANGED) then the
// level's (velk_piece_changed_word).
// Then, floats as their bits: its triangle's three corners in the world, the pixel's place in the
// world and the way its first light shines.
#define VELK_LIGHT_READ_EXPLAIN 24u
#define VELK_LIGHT_EXPLAIN_WORDS 46u
// And the depth of the deepest piece there is on the pixel's way down, before a coarse cell's stop.
#define VELK_LIGHT_EXPLAIN_DEEPEST 45u
#define VELK_LIGHT_EXPLAIN_CORNERS 30u
#define VELK_LIGHT_EXPLAIN_AT 39u
#define VELK_LIGHT_EXPLAIN_LIGHT 42u
#define VELK_LIGHT_EXPLAIN_LISTED 21u
#define VELK_LIGHT_EXPLAIN_FRAME 27u
#define VELK_LIGHT_EXPLAIN_CHANGED 28u
#define VELK_LIGHT_EXPLAIN_RECORD 0u
#define VELK_LIGHT_EXPLAIN_TRIANGLE 1u
#define VELK_LIGHT_EXPLAIN_PIECED 2u
#define VELK_LIGHT_EXPLAIN_COARSE 3u
#define VELK_LIGHT_EXPLAIN_ROOT 4u
#define VELK_LIGHT_EXPLAIN_PIECE 5u
#define VELK_LIGHT_EXPLAIN_WAY 6u
#define VELK_LIGHT_EXPLAIN_VERTICES 7u
#define VELK_LIGHT_EXPLAIN_LEVEL 14u
#define VELK_LIGHT_READ_ALBEDO 0u
#define VELK_LIGHT_READ_DIRECT 3u
#define VELK_LIGHT_READ_INDIRECT 6u
#define VELK_LIGHT_READ_ENV 9u
#define VELK_LIGHT_READ_SPECULAR 12u
#define VELK_LIGHT_READ_AREA 15u
#define VELK_LIGHT_READ_COLOR 18u
#define VELK_LIGHT_READ_METALLIC 21u
#define VELK_LIGHT_READ_FLAGS 22u
#define VELK_LIGHT_READ_MATCH 23u


// An RGB9E5 colour (rgb9e5 in gpu_data.h).
vec3 velk_rgb9e5(uint w)
{
    return vec3(float(w & 511u), float((w >> 9u) & 511u), float((w >> 18u) & 511u)) * exp2(float(int(w >> 27u) - 24));
}

// `c` as RGB9E5, as rgb9e5 in gpu_data.h packs it, each colour below 65000 so that it never
// packs to VELK_LIGHT_NONE.
uint velk_pack_rgb9e5(vec3 c)
{
    c = clamp(c, vec3(0.0), vec3(65000.0));
    float most = max(max(c.r, c.g), c.b);
    int e = most > 0.0 ? clamp(int(floor(log2(most))) + 1, -15, 16) : 0;
    float scale = exp2(float(9 - e));
    // Rounding can carry the largest mantissa to 512: one exponent more.
    if (uint(most * scale + 0.5) >= 512u && e < 16) {
        ++e;
        scale *= 0.5;
    }
    uvec3 m = min(uvec3(c * scale + 0.5), uvec3(511u));
    return m.r | (m.g << 9u) | (m.b << 18u) | (uint(e + 15) << 27u);
}

// The first word of page `page` of the pool at `pool`.
uint velk_light_page(uint pool, uint page)
{
    return VELK_LIGHT_WORDS.data[pool + VELK_POOL_SEGMENT_WORDS + (page >> 14u)] + (page & 16383u) * VELK_LIGHT_PAGE_WORDS;
}

// The record at place `place` of the kept cut `cut`'s pool, when a partition holds it: its
// word, the partition's state and the record's place in the partition's page table. False for a
// place of a free page, or past the records its page's partition holds.
bool velk_cut_live_record(uint cut, uint place, out uint record, out uint state, out uint at)
{
    record = 0u;
    state = 0u;
    at = 0u;
    if (place >= VELK_LIGHT_WORDS.data[cut + VELK_CUT_CAPACITY]) return false;
    uint header = VELK_LIGHT_WORDS.data[cut + VELK_CUT_PAGE_HEADERS] + (place / VELK_CUT_PAGE_RECORDS) * VELK_CUT_PAGE_WORDS;
    uint owner = VELK_LIGHT_WORDS.data[header + VELK_CUT_PAGE_OWNER];
    if (owner >= VELK_LIGHT_WORDS.data[cut + VELK_CUT_PARTITIONS]) return false;
    state = VELK_LIGHT_WORDS.data[cut + VELK_CUT_STATES_WORD] + owner * VELK_CUT_PARTITION_WORDS;
    at = VELK_LIGHT_WORDS.data[header + VELK_CUT_PAGE_PLACE] * VELK_CUT_PAGE_RECORDS + place % VELK_CUT_PAGE_RECORDS;
    if (at >= VELK_LIGHT_WORDS.data[state + VELK_PART_COUNT]) return false;
    record = VELK_LIGHT_WORDS.data[cut + VELK_CUT_RECORD_POOL] + place * VELK_RECORD_END;
    return true;
}

// Words before the points of a record with `table` words after its header: an even number, so
// that a point's two words are on the same page.
uint velk_light_skip_for(uint table)
{
    return VELK_PAGE_ROOTS + ((table + 1u) & ~1u);
}

// Whether the record whose first page starts at `first` has its own points in pieces, and if so
// how many triangles; 0 for none.
uint velk_light_pieced(uint first)
{
    uint kind = VELK_LIGHT_WORDS.data[first + VELK_PAGE_KIND];
    return (kind & VELK_LIGHT_PIECED) != 0u ? min(kind & 0xFFFFu, 256u) : 0u;
}

// Words before the points of the record whose first page starts at `first`: its header, and for
// own points in pieces (VELK_LIGHT_PIECED) a word per triangle naming its root piece.
uint velk_light_skip(uint first)
{
    return velk_light_skip_for(velk_light_pieced(first));
}

// Page `k` of the record whose first page starts at `first`, k from 1, as its header names it.
uint velk_light_nth_page(uint first, uint k)
{
    return VELK_LIGHT_WORDS.data[first + VELK_PAGE_PAGES - 1u + min(k, VELK_LIGHT_DIRECT_PAGES - 1u)];
}

// The arena word of word `at` of the record whose first page starts at `first`, counted from
// its first page's first word across its pages.
uint velk_light_word(uint pool, uint first, uint at)
{
    uint k = at / VELK_LIGHT_PAGE_WORDS;
    if (k == 0u) return first + at;
    return velk_light_page(pool, velk_light_nth_page(first, k)) + at % VELK_LIGHT_PAGE_WORDS;
}

// Point word `p` of the record whose first page starts at word `first`.
uint velk_light_point_word(uint pool, uint first, uint p)
{
    return velk_light_word(pool, first, velk_light_skip(first) + p);
}

// The word of own point `p` of the record whose first page starts at `first`: how much of each
// slot's light it does not get. Its trace state is the word after it, on the same page.
uint velk_light_own_word(uint pool, uint first, uint p)
{
    return velk_light_word(pool, first, velk_light_skip(first) + 2u * p);
}

// The own points of the record whose first page starts at `first`, two words each.
uint velk_light_own_points(uint first)
{
    return VELK_LIGHT_WORDS.data[first + VELK_PAGE_POINT_WORDS] / 2u;
}

// The blocker word of the point whose two words start at word `at` of page `page`: for each
// slot, 4 bits of what its last ray to that light found (VELK_LIGHT_BLOCKER_*). Not in the pages: each segment has a word after its pages for each two words of
// them, so a point's is where its two words are, halved.
uint velk_light_page_blocker(uint pool, uint page, uint at)
{
    uint segment = VELK_LIGHT_WORDS.data[pool + VELK_POOL_SEGMENT_WORDS + (page >> 14u)];
    uint in_segment = (page & 16383u) * VELK_LIGHT_PAGE_WORDS + at;
    return segment + VELK_LIGHT_SEGMENT_PAGE_WORDS + in_segment / 2u;
}

// The blocker word of own point `p` of the record whose first page starts at `first`.
uint velk_light_blocker_word(uint pool, uint first, uint p)
{
    uint at = velk_light_skip(first) + 2u * p;
    uint k = at / VELK_LIGHT_PAGE_WORDS;
    uint page = k == 0u ? VELK_LIGHT_WORDS.data[first + VELK_PAGE_FIRST] : velk_light_nth_page(first, k);
    return velk_light_page_blocker(pool, page, at % VELK_LIGHT_PAGE_WORDS);
}

// What a point's last ray to a light found, 4 bits: a light due a ray, never traced at the point
// or made due by a change (what the point has of it stays until its next ray); one whose ray
// something blocked; one whose ray found nothing in the way.
#define VELK_LIGHT_BLOCKER_DUE 0u
#define VELK_LIGHT_BLOCKER_BLOCKED 1u
#define VELK_LIGHT_BLOCKER_CLEAR 15u

// Whether any light of blocker codes `codes` is due (VELK_LIGHT_BLOCKER_DUE): any 4 bits 0.
bool velk_light_any_due(uint codes)
{
    return (((codes - 0x11111111u) & ~codes) & 0x88888888u) != 0u;
}

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


// Point (i, j) of a piece's grid, with i + j at most its level n, at weights
// (1 - (i + j) / n, i / n, j / n) of the piece's corners: row by row in j, each row in i, rows as
// long as a grid of level VELK_PIECE_LEVEL has them, whatever the piece's level.
uint velk_piece_point(uint i, uint j)
{
    return j * (VELK_PIECE_LEVEL + 1u) - (j * (j - 1u)) / 2u + i;
}

// The place (i, j) of point `q` of a piece's grid, as velk_piece_point counts them.
uvec2 velk_piece_place(uint q)
{
    for (uint j = 0u; j <= VELK_PIECE_LEVEL; ++j) {
        uint in_row = VELK_PIECE_LEVEL + 1u - j;
        if (q < in_row) return uvec2(q, j);
        q -= in_row;
    }
    return uvec2(0u, 0u);
}

// A piece's place in its triangle (VELK_PIECE_PLACE): at depth `depth` the triangle is
// 2^depth steps along each edge, its weights (1 - (x + y) / 2^depth, x / 2^depth, y / 2^depth)
// at step (x, y); a piece is at (i, j) with its corners at (i, j), (i + 1, j) and (i, j + 1), or
// turned (`turned`), at (i + 1, j + 1), (i, j + 1) and (i + 1, j). i in bits 0 to 12, j in 13 to 25,
// turned bit 26, the depth in bits 27 to 30, and bit 31 (VELK_PIECE_PLACE_EDGE) for a piece made
// where light differed rather than to bring the levels to the light spacing, which alone may be
// joined back into its parent (light_pieces_src). A root, at depth 0, is at (0, 0) and keeps its
// grid's level in bits 0 to 2 instead (velk_piece_root_place).
#define VELK_PIECE_PLACE_EDGE 0x80000000u

uint velk_piece_place_word(uint depth, uint i, uint j, bool turned)
{
    return (i & 8191u) | ((j & 8191u) << 13u) | (turned ? (1u << 26u) : 0u) | (depth << 27u);
}

// A root's place, with its grid's level `level` (VELK_PIECE_ROOT_LEVELS).
uint velk_piece_root_place(uint level)
{
    return level & 7u;
}

uint velk_piece_depth(uint place)
{
    return (place >> 27u) & 15u;
}

// The level of the grid of the piece at `place`: its own for a root, VELK_PIECE_LEVEL below.
uint velk_piece_level(uint place)
{
    return velk_piece_depth(place) == 0u ? clamp(place & 7u, 2u, VELK_PIECE_LEVEL) : VELK_PIECE_LEVEL;
}

// (i, j) of the piece at `place`; a root's is (0, 0).
uvec2 velk_piece_ij(uint place)
{
    return velk_piece_depth(place) == 0u ? uvec2(0u, 0u) : uvec2(place & 8191u, (place >> 13u) & 8191u);
}

// The corners of the piece at `place`, as steps of its depth (velk_piece_place_word).
void velk_piece_corners(uint place, out ivec2 c[3])
{
    uvec2 ij = velk_piece_ij(place);
    int i = int(ij.x);
    int j = int(ij.y);
    if (velk_piece_depth(place) == 0u || (place & (1u << 26u)) == 0u) {
        c[0] = ivec2(i, j);
        c[1] = ivec2(i + 1, j);
        c[2] = ivec2(i, j + 1);
    } else {
        c[0] = ivec2(i + 1, j + 1);
        c[1] = ivec2(i, j + 1);
        c[2] = ivec2(i + 1, j);
    }
}

// The place of child `k` of the piece at `place`: 0 to 2 the child at that corner, keeping it
// in its place, the other two corners the midpoints of the edges from it; 3 the middle one, its
// corner k the midpoint of the edge across from the piece's corner k.
uint velk_piece_child_place(uint place, uint k)
{
    uint depth = velk_piece_depth(place) + 1u;
    uvec2 ij = velk_piece_ij(place);
    uint i = 2u * ij.x;
    uint j = 2u * ij.y;
    if (depth == 1u || (place & (1u << 26u)) == 0u) {
        if (k == 0u) return velk_piece_place_word(depth, i, j, false);
        if (k == 1u) return velk_piece_place_word(depth, i + 1u, j, false);
        if (k == 2u) return velk_piece_place_word(depth, i, j + 1u, false);
        return velk_piece_place_word(depth, i, j, true);
    }
    if (k == 0u) return velk_piece_place_word(depth, i + 1u, j + 1u, true);
    if (k == 1u) return velk_piece_place_word(depth, i, j + 1u, true);
    if (k == 2u) return velk_piece_place_word(depth, i + 1u, j, true);
    return velk_piece_place_word(depth, i + 1u, j + 1u, false);
}

// Where point (a, b) of the grid of the piece at `place` is in its triangle, as steps of a
// VELK_PIECE_UNITS-th of its depth's steps; every level a grid may have divides that.
ivec2 velk_piece_point_at(uint place, uvec2 ab)
{
    ivec2 c[3];
    velk_piece_corners(place, c);
    int step = int(VELK_PIECE_UNITS / velk_piece_level(place));
    return int(VELK_PIECE_UNITS) * c[0] + step * (int(ab.x) * (c[1] - c[0]) + int(ab.y) * (c[2] - c[0]));
}

// The weights in its triangle of point (a, b) of the grid of the piece at `place`.
vec3 velk_piece_weights(uint place, uvec2 ab)
{
    ivec2 x = velk_piece_point_at(place, ab);
    float steps = float(VELK_PIECE_UNITS) * exp2(float(velk_piece_depth(place)));
    return vec3(steps - float(x.x + x.y), float(x.x), float(x.y)) / steps;
}

// The point of the grid of the piece at `place` at `x` (as velk_piece_point_at counts), or
// VELK_LIGHT_NONE when it has none there.
uint velk_piece_point_of(uint place, ivec2 x)
{
    ivec2 c[3];
    velk_piece_corners(place, c);
    uint n = velk_piece_level(place);
    int step = int(VELK_PIECE_UNITS / n);
    ivec2 r = x - int(VELK_PIECE_UNITS) * c[0];
    int a = r.x * (c[1].x - c[0].x);
    int b = r.y * (c[2].y - c[0].y);
    if (a < 0 || b < 0 || a % step != 0 || b % step != 0) return VELK_LIGHT_NONE;
    a /= step;
    b /= step;
    if (a + b > int(n)) return VELK_LIGHT_NONE;
    return velk_piece_point(uint(a), uint(b));
}

// How far apart on screen, in pixels, the points of a piece at `depth` are, in a triangle whose
// longest edge is `tri_px` pixels long where the pixel is and whose root's grid is of level
// `root_level`.
float velk_piece_spacing(uint depth, uint root_level, float tri_px)
{
    uint n = depth == 0u ? root_level : VELK_PIECE_LEVEL;
    return tri_px / (float(n) * exp2(float(depth)));
}

// The level of a root's grid for a triangle whose longest edge is `tri_px` pixels long, its points
// about `light_pixels` apart: one of VELK_PIECE_ROOT_LEVELS. A triangle at least three times
// VELK_LIGHT_COARSE_PIECES times that long gets the coarsest grid whose points are still that far
// apart, so that its root is its coarse level, and not its vertices. Not from twice: as many
// roots more cost moving lights about a third more trace.
uint velk_piece_root_level(float tri_px, float light_pixels)
{
    float coarse = VELK_LIGHT_COARSE_PIECES * light_pixels;
    if (tri_px >= 3.0 * coarse) {
        uint n = uint(clamp(floor(tri_px / coarse), 2.0, float(VELK_PIECE_LEVEL)));
        return n == 5u ? 4u : n;
    }
    uint n = uint(clamp(ceil(tri_px / light_pixels), 2.0, float(VELK_PIECE_LEVEL)));
    return n == 5u ? VELK_PIECE_LEVEL : n;
}

// Which child of a piece weights `l` of it are in, as velk_piece_child_place numbers them; `l`
// becomes the weights in that child.
uint velk_piece_step(inout vec3 l)
{
    uint k = 3u;
    if (l.x >= 0.5) {
        l = vec3(2.0 * l.x - 1.0, 2.0 * l.y, 2.0 * l.z);
        k = 0u;
    } else if (l.y >= 0.5) {
        l = vec3(2.0 * l.x, 2.0 * l.y - 1.0, 2.0 * l.z);
        k = 1u;
    } else if (l.z >= 0.5) {
        l = vec3(2.0 * l.x, 2.0 * l.y, 2.0 * l.z - 1.0);
        k = 2u;
    } else {
        l = vec3(1.0) - 2.0 * l;
    }
    l = clamp(l, 0.0, 1.0);
    l /= max(l.x + l.y + l.z, 1e-6);
    return k;
}

// A pixel's way down its triangle's levels of light (velk_piece_way): the triangle's vertex points
// (VELK_PIECE_VERTICES), then its root piece and the pieces under it. The level it reads, the one
// above that and its coarse level, each with the pixel's weights in it.
struct VelkPieceWay {
    uint piece;           // the first level whose points are at most the light spacing apart, or
    uint depth;           // the deepest there is; its depth when a piece
    vec3 at;
    float spacing;        // how far apart its points are on screen, in pixels
    uint root_level;      // the level of the root's grid (velk_piece_level), 0 without a root
    uint parent;          // VELK_PIECE_VERTICES above a root, VELK_LIGHT_NONE above the vertex points
    vec3 parent_at;
    uint coarse;          // the deepest level whose points are at least VELK_LIGHT_COARSE_PIECES
    uint coarse_depth;    // times the light spacing apart, the vertex points when none is
    vec3 coarse_at;
    float coarse_spacing;
};

// Where a pixel or hit is among its record's light roots (velk_light_root_of).
struct VelkLightRoot {
    uint slot;    // its root word's, VELK_LIGHT_NONE when its record has no pieces for its triangle
    vec3 at;      // where it is in the root: its triangle's weights
    float size;   // the root's size in world units: its triangle's longest edge
};

// Where a pixel or hit at weights `b` of triangle `t` (its longest edge `tri_size` world units)
// of the record whose first page starts at `first` is among the record's light roots.
VelkLightRoot velk_light_root_of(uint first, uint t, vec3 b, float tri_size)
{
    VelkLightRoot r;
    r.slot = VELK_LIGHT_NONE;
    r.at = b;
    r.size = tri_size;
    uint pieced = velk_light_pieced(first);
    if (pieced == 0u) return r;
    uint cluster = VELK_LIGHT_WORDS.data[first + VELK_PAGE_CLUSTER];
    if (t >= VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_TRIANGLES]) return r;
    if (t >= pieced) return r;
    r.slot = t;
    return r;
}

// The way down for a pixel at weights `b` of its triangle, whose longest edge is `tri_px` pixels
// long there and whose root piece is `root` (none at or past the pool's pages), to points at most
// `light_pixels` apart. Under the triangle's vertex points the root is at `r` (velk_light_root_of),
// `root_px` pixels across.
VelkPieceWay velk_piece_way(uint pool, uint root, vec3 b, float tri_px, VelkLightRoot r, float root_px, float light_pixels)
{
    uint pages = VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES];
    VelkPieceWay w;
    w.piece = VELK_PIECE_VERTICES;
    w.depth = 0u;
    w.at = b;
    w.spacing = tri_px;
    w.root_level = 0u;
    w.parent = VELK_LIGHT_NONE;
    w.parent_at = b;
    w.coarse = VELK_PIECE_VERTICES;
    w.coarse_depth = 0u;
    w.coarse_at = b;
    w.coarse_spacing = tri_px;
    if (root >= pages || tri_px <= light_pixels) return w;
    w.root_level = velk_piece_level(VELK_LIGHT_WORDS.data[velk_light_page(pool, root) + VELK_PIECE_PLACE]);
    w.parent = VELK_PIECE_VERTICES;
    w.piece = root;
    w.at = r.at;
    w.spacing = velk_piece_spacing(0u, w.root_level, root_px);
    for (uint step = 0u; step <= VELK_PIECE_DEEPEST; ++step) {
        if (w.spacing >= VELK_LIGHT_COARSE_PIECES * light_pixels) {
            w.coarse = w.piece;
            w.coarse_depth = w.depth;
            w.coarse_at = w.at;
            w.coarse_spacing = w.spacing;
        }
        if (w.spacing <= light_pixels || w.depth >= VELK_PIECE_DEEPEST) break;
        vec3 l = w.at;
        uint k = velk_piece_step(l);
        uint child = VELK_LIGHT_WORDS.data[velk_light_page(pool, w.piece) + VELK_PIECE_CHILDREN + k];
        if (child >= pages) break;
        w.parent = w.piece;
        w.parent_at = w.at;
        w.piece = child;
        w.at = l;
        ++w.depth;
        w.spacing = velk_piece_spacing(w.depth, w.root_level, root_px);
    }
    return w;
}


// The grid places of the three points of light around weights `w` of a triangle's corners, in
// its grid of level `n`, and their weights. Without a grid, the corners.
void velk_light_around(uint n, vec3 w, out uvec2 places[3], out vec3 weights)
{
    if (n < 2u) {
        places[0] = uvec2(0u, 0u);
        places[1] = uvec2(1u, 0u);
        places[2] = uvec2(0u, 1u);
        weights = w;
        return;
    }
    float a = clamp(w.y, 0.0, 1.0) * float(n);
    float b = clamp(w.z, 0.0, 1.0) * float(n);
    uint i = min(uint(a), n - 1u);
    uint j = min(uint(b), n - 1u - i);
    float fa = clamp(a - float(i), 0.0, 1.0);
    float fb = clamp(b - float(j), 0.0, 1.0);
    if (fa + fb > 1.0 && i + j + 2u <= n) {
        places[0] = uvec2(i + 1u, j + 1u);
        places[1] = uvec2(i, j + 1u);
        places[2] = uvec2(i + 1u, j);
        weights = vec3(fa + fb - 1.0, 1.0 - fa, 1.0 - fb);
    } else {
        float s = max(fa + fb, 1.0);
        places[0] = uvec2(i, j);
        places[1] = uvec2(i + 1u, j);
        places[2] = uvec2(i, j + 1u);
        weights = vec3(1.0 - (fa + fb) / s, fa / s, fb / s);
    }
}

// The three points of the piece at page `page` around weights `l` of it, as words of the pool,
// and their weights.
void velk_piece_around(uint pool, uint page, vec3 l, out uint words[3], out vec3 weights)
{
    uvec2 places[3];
    uint first = velk_light_page(pool, page);
    uint place = VELK_LIGHT_WORDS.data[first + VELK_PIECE_PLACE];
    velk_light_around(velk_piece_level(place), l, places, weights);
    for (uint k = 0u; k < 3u; ++k) words[k] = first + 2u * velk_piece_point(places[k].x, places[k].y);
}

// The vertex of corner `corner` of a triangle of the record whose first page starts at `first`,
// kept within its vertices.
uint velk_way_vertex(uint first, uint corner)
{
    return min(corner, max(VELK_LIGHT_WORDS.data[first + VELK_PAGE_VERTICES], 1u) - 1u);
}

// The three points around weights `l` of level `page` of a pixel's way down (VelkPieceWay) and
// their weights: a piece's, or with VELK_PIECE_VERTICES the vertex points of the triangle with
// corners `corners` in the record whose first page starts at `first`.
void velk_way_around(uint pool, uint first, uvec3 corners, uint page, vec3 l, out uint words[3], out vec3 weights)
{
    if (page != VELK_PIECE_VERTICES) {
        velk_piece_around(pool, page, l, words, weights);
        return;
    }
    for (uint k = 0u; k < 3u; ++k) words[k] = velk_light_own_word(pool, first, velk_way_vertex(first, corners[k]));
    weights = l;
}

// The blocker word of point `k` around a pixel on level `page` of its way down, whose words
// velk_way_around gave as `word`.
uint velk_way_blocker(uint pool, uint first, uvec3 corners, uint page, uint k, uint word)
{
    if (page == VELK_PIECE_VERTICES) return velk_light_blocker_word(pool, first, velk_way_vertex(first, corners[k]));
    return velk_light_page_blocker(pool, page, word - velk_light_page(pool, page));
}


// What the cell of a pixel's coarse level (VelkPieceWay) around it says about the levels under
// it, in the triangle with corners `corners` of the record whose first page starts at `first`:
// 0 when it is not known yet (a point not traced, or with `fresh`, stale since a change or with
// one of its record's lights due), 1 when its three points carry the same light, each light
// fully lit or fully blocked, so that the levels under it could only repeat it: exact; 2 when
// they differ. With `fresh` (the lighting), a due light is no answer: the pixel reads the levels
// under it, which may have traced it since. Without (the listing), what a due light had still
// judges the cell, so that the levels under it are listed while it moves. Without `fresh`, a cell
// whose stale points agree is not exact where a place next to them in its piece has other light:
// an edge could cross into it by the time they are traced again.
uint velk_way_cell(uint pool, uint first, uvec3 corners, VelkPieceWay way, bool fresh)
{
    uint words[3];
    vec3 w;
    velk_way_around(pool, first, corners, way.coarse, way.coarse_at, words, w);
    uint shade = VELK_LIGHT_NONE;
    bool same = true;
    bool stale = false;
    // The record's lights, 4 bits each: a point's blocker code 0 under one is a light due.
    uint lit = 0u;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        if (VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s] != VELK_LIGHT_NONE) lit |= 15u << (4u * s);
    }
    for (uint k = 0u; k < 3u; ++k) {
        uint s = VELK_LIGHT_WORDS.data[words[k]];
        if (s == VELK_LIGHT_NONE) return 0u;
        if (fresh) {
            uint codes = VELK_LIGHT_WORDS.data[velk_way_blocker(pool, first, corners, way.coarse, k, words[k])];
            for (uint l = 0u; l < VELK_LIGHT_SLOTS; ++l) {
                if (((lit >> (4u * l)) & 15u) != 0u && ((codes >> (4u * l)) & 15u) == VELK_LIGHT_BLOCKER_DUE) return 0u;
            }
        }
        uint piece = way.coarse == VELK_PIECE_VERTICES ? VELK_LIGHT_NONE : way.coarse;
        bool old = velk_light_point_traces(pool, first, piece, VELK_LIGHT_WORDS.data[words[k] + 1u]) == 0u;
        if (fresh && old) return 0u;
        stale = stale || old;
        if (k == 0u) shade = s;
        else if (s != shade) same = false;
    }
    // Every light's four bits all clear or all set.
    if (!same || shade != (shade & 0x11111111u) * 15u) return 2u;
    if (fresh || !stale || way.coarse == VELK_PIECE_VERTICES) return 1u;
    uint at = velk_light_page(pool, way.coarse);
    uint place = VELK_LIGHT_WORDS.data[at + VELK_PIECE_PLACE];
    uint n = velk_piece_level(place);
    uvec2 places[3];
    velk_light_around(n, way.coarse_at, places, w);
    // A grid point's neighbours, in the grid's triangles.
    const ivec2 kAround[6] = ivec2[6](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1), ivec2(1, -1), ivec2(-1, 1));
    for (uint k = 0u; k < 3u; ++k) {
        for (uint a = 0u; a < 6u; ++a) {
            ivec2 q = ivec2(places[k]) + kAround[a];
            if (q.x < 0 || q.y < 0 || q.x + q.y > int(n)) continue;
            uint p = velk_piece_point(uint(q.x), uint(q.y));
            uint s = VELK_LIGHT_WORDS.data[at + 2u * p];
            if (s != VELK_LIGHT_NONE && s != shade) return 2u;
        }
    }
    return 1u;
}

// The way down ended at its coarse level, for a pixel whose coarse cell is exact or, for the
// claim, not known yet: the levels under it are not read.
void velk_way_stop_at_coarse(inout VelkPieceWay w)
{
    w.piece = w.coarse;
    w.depth = w.coarse_depth;
    w.at = w.coarse_at;
    w.spacing = w.coarse_spacing;
    w.parent = VELK_LIGHT_NONE;
    w.parent_at = w.coarse_at;
}

// The first page of the record whose triangle the piece at page `page` is in.
uint velk_piece_record(uint pool, uint page)
{
    return VELK_LIGHT_WORDS.data[velk_light_page(pool, page) + VELK_PIECE_OWNER] & 0xFFFFFFu;
}

// A point list's count (VELK_LIGHT_LIST_*) in the pool's header, and its word's.
uint velk_light_list_count_word(uint list)
{
    return list == VELK_LIGHT_LIST_UNTRACED ? VELK_POOL_UNTRACED_POINTS
         : list == VELK_LIGHT_LIST_COARSE   ? VELK_POOL_COARSE_POINTS
                                            : VELK_POOL_FINE_POINTS;
}
uint velk_light_list_word_word(uint list)
{
    return list == VELK_LIGHT_LIST_UNTRACED ? VELK_POOL_UNTRACED_POINTS_WORD
         : list == VELK_LIGHT_LIST_COARSE   ? VELK_POOL_COARSE_POINTS_WORD
                                            : VELK_POOL_FINE_POINTS_WORD;
}

// The header words holding a point list's points traced this frame and its points in their room,
// by its place `k` in velk_points_list's order.
uint velk_points_take_word(uint k)
{
    return VELK_POOL_POINT_TAKES + k;
}
uint velk_points_count_word(uint k)
{
    return VELK_POOL_POINT_COUNTS + k;
}

// The points of light at the corners of a pixel's triangle (corners `corners`, the pixel at
// weights `b` of them) of the record whose first page starts at `first`, up to nine: each
// corner's own point or the three it links to, the words they are in, their numbers there, their
// weights and the first pages of the records they are in, whose lights their slots are. Returns
// how many.
uint velk_light_pixel_points(uint pool, uint first, uvec3 corners, vec3 b, out uint words[9], out uint ids[9],
                             out float weights[9], out uint owners[9])
{
    uint count = 0u;
    for (uint k = 0u; k < 3u; ++k) {
        uint point_words[3];
        uint point_ids[3];
        vec3 w;
        uint owner;
        velk_light_points(pool, first, corners[k], point_words, point_ids, w, owner);
        if (owner == VELK_LIGHT_NONE) continue;
        for (uint j = 0u; j < 3u; ++j) {
            if (point_words[j] == VELK_LIGHT_NONE) continue;
            words[count] = point_words[j];
            ids[count] = point_ids[j];
            weights[count] = b[k] * w[j];
            owners[count] = owner;
            ++count;
        }
    }
    return count;
}

// Whether one value `value` (a record's or a quad node's) is due a trace: not traced yet, or
// changed since (light_changes_main_src).
bool velk_light_one_due(uint value)
{
    return (value & VELK_LIGHT_ONE_TRACED) == 0u;
}

// Whether a coarse light not traced yet already has its light: that of the one value its record
// had before its points (light_search_src). Its trace then only chooses its lights, which are the
// one value's, chosen from the same sphere, and casts no rays. A one value whose every light got
// through is not told apart; it is traced as any other.
bool velk_light_seeded(uint value)
{
    return value >= VELK_LIGHT_ONE_VALUE && value < VELK_LIGHT_PENDING && (value & VELK_LIGHT_ONE_TRACED) == 0u
        && (value & VELK_LIGHT_ONE_SHADE_BITS) != 0u;
}

// The node of the quad tree at `tree` over node `node` whose quad is as wide on screen as
// `light_pixels`, the light of all the quads under it: the topmost going up from `node` that is
// not wider. In world space; `cam` the camera.
uint velk_light_quad_node(uint tree, uint node, vec3 cam, float pixels_per_unit, float light_pixels)
{
    uint i = (node - tree) / VELK_QUAD_NODE_WORDS;
    for (uint step = 0u; step < 32u && i > 0u; ++step) {
        uint parent = (i - 1u) / 2u;
        uint w = tree + parent * VELK_QUAD_NODE_WORDS;
        float radius = uintBitsToFloat(VELK_LIGHT_WORDS.data[w + VELK_QUAD_NODE_RADIUS]);
        if (radius < 0.0) break;
        vec3 center = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[w + VELK_QUAD_NODE_CENTER]),
                           uintBitsToFloat(VELK_LIGHT_WORDS.data[w + VELK_QUAD_NODE_CENTER + 1u]),
                           uintBitsToFloat(VELK_LIGHT_WORDS.data[w + VELK_QUAD_NODE_CENTER + 2u]));
        vec3 u = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[w + VELK_QUAD_NODE_U]),
                      uintBitsToFloat(VELK_LIGHT_WORDS.data[w + VELK_QUAD_NODE_U + 1u]),
                      uintBitsToFloat(VELK_LIGHT_WORDS.data[w + VELK_QUAD_NODE_U + 2u]));
        vec3 v = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[w + VELK_QUAD_NODE_V]),
                      uintBitsToFloat(VELK_LIGHT_WORDS.data[w + VELK_QUAD_NODE_V + 1u]),
                      uintBitsToFloat(VELK_LIGHT_WORDS.data[w + VELK_QUAD_NODE_V + 2u]));
        float near = length(center - cam) - radius;
        if (near <= 0.0 || 2.0 * max(length(u), length(v)) * pixels_per_unit > light_pixels * near) break;
        i = parent;
    }
    return tree + i * VELK_QUAD_NODE_WORDS;
}
