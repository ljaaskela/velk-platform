#define VELK_LIGHT_NONE 0xFFFFFFFFu
#define VELK_LIGHT_PENDING 0xFFFFFFFEu
#define VELK_LIGHT_ONE_VALUE 0x80000000u
#define VELK_LIGHT_OWN 1u
#define VELK_LIGHT_PAGE_WORDS 64u
#define VELK_LIGHT_SEGMENT_PAGE_WORDS 1048576u
#define VELK_LIGHT_DIRECT_PAGES 15u
#define VELK_LIGHT_PIECED 0x40000000u
#define VELK_LIGHT_TWO_SIDED 0x20000000u
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
