#ifndef VELK_RENDER_FRAME_LIGHT_POOL_LAYOUT_H
#define VELK_RENDER_FRAME_LIGHT_POOL_LAYOUT_H

#include <velk-render/frame/cull_layout.h>

#include <cstdint>

namespace velk {

/// A view's light pool, in the culling arena: where the light of the kept cuts' records is kept
/// (kCutRecordWords, its kCutRecordLight word). A record's light word is one of:
///   kLightNone      not read yet
///   kLightPending   being given its light, for the moment it takes
///   below kLightOneValue  the first page of its light
///   kLightOneValue and up  one value for the whole record, in the bits under kLightOneValue
/// Light is about kLightPixels apart on screen wherever it is. A record gets its light the first
/// time a pixel shows it (light_claim_src): one value for a cluster smaller than that on screen,
/// a whole mesh, a shape drawn by its own pass and a cluster of a mesh that deforms, whose
/// vertices are not its own. A larger cluster goes on the search list (light_search_src), which
/// gives it pages: a point per vertex of its own. A quad record's light is on the node of its quad tree that is as
/// wide as kLightPixels over it, one value for all the quads under that node. A record's light
/// goes with it while its partition keeps its cut, and to its like when the partition chooses
/// again. Pages no record names are found free every frame
/// (light_mark_src, light_sweep_src).
///
/// The pool is a header of kLightPoolHeaderWords (VELK_LIGHT_POOL_HEADER), then a stack of the
/// free pages, then a bit per page marking those in use, then a bit per page marking those that
/// are pieces, all for as many pages as the pool may grow to, then the search list and the split
/// list, and its pages in segments of kLightSegmentPages, each a region of its own, so that
/// growing moves no page, with its pages' blocker words after them (kLightSegmentWordsPerPage).
///
/// A record's first page starts with its header (VELK_LIGHT_PAGE_HEADER), then for own points
/// in pieces (kLightPieced) a word per triangle naming the first page of its root piece,
/// kLightNone for none yet, kLightPending while it is being made. Its point words follow, from an
/// even word on, then those of its other pages in order, at most kLightDirectPages in all: with
/// its own points two words a point, a point per vertex: how much of each slot's light it does not
/// get (4 bits a slot, the first lowest, 15 none, kLightNone until first traced), then how many
/// times it has been traced (top 4 bits) and the frame it was last listed (the rest; kLightNone
/// before that).
///
/// A triangle's light inside it is a tree of pieces. Its root is the whole triangle; a piece's four
/// children split it by its edges' midpoints: the three at its corners, each keeping that corner
/// in its place, then the middle one, whose corner k is the midpoint of the edge across from
/// corner k. A piece is one page (VELK_LIGHT_PIECE_PAGE): a grid of level kLightPieceLevel over it,
/// a root's as fine as its triangle is wide on screen, its points two words each as above, its
/// children's pages, where it is in its triangle, its record and triangle, its parent and the frame
/// it was last read. Above the root is the level of the triangle's vertex points. A pixel goes down
/// these levels by its weights to the first whose points are at most kLightPixels apart on
/// screen, or that has nothing under it; its coarse level, traced first, is the deepest on the way
/// whose points are at least kLightCoarsePieces times kLightPixels apart, else the vertex points.
/// Where the coarse level's three points around a pixel carry the same light, each light fully
/// lit or fully blocked, the pixel reads the coarse level and goes no further down.
/// What comes next under a level that is too coarse there, the root or a piece's children, is
/// made always while its points would still be kLightCoarsePieces times kLightPixels apart, below
/// that only where the light of the level's points differs (light_claim_src, light_search_src).
/// Pieces no pixel has read for kLightPieceKeepFrames are freed, four children at a time, and four
/// children whose light has become the same as their parent's (light_pieces_src). A triangle's
/// vertex points are traced whenever a pixel reads it, since rays that hit it read them.
///
/// A record of one value (kLightOneValue and up), a quad's node likewise, holds in the bits
/// under it: how much of each of its lights it does not get, 3 bits a slot, the first lowest
/// (kLightOneShadeBits); whether it has been traced (kLightOneTraced); whether a screen probe's
/// ray hit it while it was not (kLightOneAsked), so that the next frame's mark (light_mark_src)
/// lists it for tracing; and the last frame it was put on the trace list, its low 5 bits
/// (kLightOneQueuedShift). A trace clears the ask. Its lights are the strongest at its centre,
/// found where they are needed (velk_light_slots_at), not kept.
///
/// A record's radiance word, and a quad node's, is the indirect light its surface gets, RGB9E5,
/// kLightNone until a pixel the screen probes light shows it: a pixel in 16 a frame blends its
/// own into it (deferred_lighting_main_src). A record also keeps the way those pixels face on
/// average (kCutRecordRadianceDir), short for a record seen facing many ways. A probe ray's hit
/// that the screen probes do not cover takes it before the environment where it faces about that
/// way (velk_probe_hit_indirect); a quad faces one way.
inline constexpr uint32_t kLightNone = 0xFFFFFFFFu;
inline constexpr uint32_t kLightPending = 0xFFFFFFFEu;
inline constexpr uint32_t kLightOneValue = 0x80000000u;
inline constexpr uint32_t kLightOneTraced = 0x40000000u;
inline constexpr uint32_t kLightOneAsked = 0x20000000u;
inline constexpr uint32_t kLightOneShadeBits = 0x00FFFFFFu;
inline constexpr uint32_t kLightOneQueuedShift = 24;
/// A record's light before it has its own: this with the first page of the record it replaced in
/// a cut chosen again, one of coarser detail made from its cluster's group, whose lights and coarse
/// light its own start from (light_search_src). The light mark keeps that page meanwhile.
inline constexpr uint32_t kLightInherit = 0x40000000u;
inline constexpr uint32_t kLightOwn = 1u;
inline constexpr uint32_t kLightPageWords = 64;

/// The header of a record's first page, its words in order, named as VELK_LIGHT_POOL_HEADER's
/// (kLightPage and VELK_PAGE_). The words from CHANGED on are its own points' only.
#define VELK_LIGHT_PAGE_HEADER(X)                                                                                    \
    X(Vertices, VERTICES, 0)          /* its cluster's vertices */                                                   \
    X(Kind, KIND, 1)                  /* own points: kLightOwn, or kLightPieced with its triangles */                  \
    X(PointWords, POINT_WORDS, 2)     /* how many point words it has */                                              \
    X(Pages, PAGES, 3)                /* its other pages from the second, kLightNone for none */                     \
    X(Changed, CHANGED, 17)           /* the frame something last changed what its points could get, or kLightNone */ \
    X(Cluster, CLUSTER, 18)           /* its cluster's word in its hierarchy's table */                              \
    X(Reset, RESET, 19)               /* the frame its lights were last chosen again, or kLightNone */               \
    X(Slots, SLOTS, 20)               /* its lights, a light index a slot (kLightSlots), kLightNone for none */      \
    X(Index, INDEX, 28)               /* its draw's first index word */                                              \
    X(Instance, INSTANCE, 29)         /* its instance when something last changed what it could get, for its pieces */ \
    X(Coarse, COARSE, 30)             /* its coarse light: a one value from its cluster's sphere with its points'    \
                                         lights, read under its points until they are trusted */                     \
    X(Read, READ, 31)                 /* not 0 when a pixel read its points since the last mark; for the log */      \
    X(First, FIRST, 32)               /* its first page's number, which its points' blocker words go by */           \
    X(Touched, TOUCHED, 33)           /* the frame a change last altered some of its lights' light, for its pieces */ \
    X(Roots, ROOTS, 34)               /* for own points in pieces, the first of the triangles' root words; even,     \
                                         so that a point's two words are on one page */

#define VELK_LIGHT_PAGE_CPP_WORD(cpp, glsl, word) inline constexpr uint32_t kLightPage##cpp = word;
VELK_LIGHT_PAGE_HEADER(VELK_LIGHT_PAGE_CPP_WORD)
#undef VELK_LIGHT_PAGE_CPP_WORD
/// The page header's words, for the shaders: a #define VELK_PAGE_ each.
#define VELK_LIGHT_PAGE_GLSL_WORD(cpp, glsl, word) "#define VELK_PAGE_" #glsl " " #word "u\n"
#define VELK_LIGHT_PAGE_HEADER_GLSL VELK_LIGHT_PAGE_HEADER(VELK_LIGHT_PAGE_GLSL_WORD)
/// The header's words: the triangles' root words follow it.
inline constexpr uint32_t kLightPageHeaderWords = kLightPageRoots;
static_assert(kLightPageRoots % 2u == 0u, "a point's two words on one page");
static_assert(kLightPageChanged == kLightPagePages + 14u, "the header names 14 pages after the first");
static_assert(kLightPageIndex == kLightPageSlots + 8u, "eight light slots");
/// Own points in pieces have this in their header's word 1, with their triangle count under it:
/// a word per triangle follows the header, naming its root piece.
inline constexpr uint32_t kLightPieced = 0x40000000u;
/// Own points of a surface seen from both sides (kDrawFlagDoubleSided) have this in their kind
/// too: each light is traced from the side it shines on.
inline constexpr uint32_t kLightTwoSided = 0x20000000u;
/// The pages a record has at most, all named in its header: its header, a root word for each of
/// up to 256 triangles and its points, two words for each of up to 256 vertices, or three when it
/// links.
inline constexpr uint32_t kLightDirectPages = 15;
static_assert(kLightPageRoots + 256u + 2u * 256u <= kLightDirectPages * kLightPageWords
                  && kLightPageRoots + 3u * 256u <= kLightDirectPages * kLightPageWords,
              "a record's pages fit its header");

/// A piece's page, its words in order, named as VELK_LIGHT_POOL_HEADER's (kLightPiece and
/// VELK_PIECE_): its points first, two words each as a record's own points.
#define VELK_LIGHT_PIECE_PAGE(X)                                                                                     \
    X(Children, CHILDREN, 56)         /* its four children's pages, in the order its corners go, then the middle;    \
                                         kLightNone for none, all four or none */                                    \
    X(Place, PLACE, 60)               /* where it is in its triangle (velk_piece_corners) */                         \
    X(Owner, OWNER, 61)               /* its record's first page (bits 0 to 23) and its triangle (24 up) */          \
    X(Parent, PARENT, 62)             /* its parent's page, kLightNone for a root */                                 \
    X(Read, READ, 63)                 /* the pool's frame it was last read (kLightPoolFrame, bits 0 to 27),          \
                                         kLightPieceAsked once a pixel asked for its children that frame, and in     \
                                         bits 29 to 31 for how many frames in a row its children's light has been    \
                                         the same as its own (kLightPieceSameFrames) */

#define VELK_LIGHT_PIECE_CPP_WORD(cpp, glsl, word) inline constexpr uint32_t kLightPiece##cpp = word;
VELK_LIGHT_PIECE_PAGE(VELK_LIGHT_PIECE_CPP_WORD)
#undef VELK_LIGHT_PIECE_CPP_WORD
/// The piece page's words, for the shaders: a #define VELK_PIECE_ each.
#define VELK_LIGHT_PIECE_GLSL_WORD(cpp, glsl, word) "#define VELK_PIECE_" #glsl " " #word "u\n"
#define VELK_LIGHT_PIECE_PAGE_GLSL VELK_LIGHT_PIECE_PAGE(VELK_LIGHT_PIECE_GLSL_WORD)
/// A piece's grid: that many steps along each of its edges, its points kLightPiecePoints. A root's
/// grid is as fine as its triangle is wide on screen over kLightPixels, of level 2, 3, 4 or 6, the
/// level kept in its place word (VELK_PIECE_PLACE); its points are numbered as in a grid of
/// kLightPieceLevel.
inline constexpr uint32_t kLightPieceLevel = 6;
inline constexpr uint32_t kLightPiecePoints = (kLightPieceLevel + 1u) * (kLightPieceLevel + 2u) / 2u;
/// Where a piece's points are in its triangle is counted in this many steps of its depth's: a
/// multiple of every level a grid may have, so that a root's points shared with its children are
/// found exactly.
inline constexpr uint32_t kLightPieceUnits = 12;
static_assert(2u * kLightPiecePoints == kLightPieceChildren, "a piece's points end where its children begin");
static_assert(kLightPieceRead + 1u == kLightPageWords, "a piece is one page");
/// The deepest a piece may be, its root at 0; its place in its triangle holds 13 bits each way.
inline constexpr uint32_t kLightPieceDeepest = 12;
/// A piece's READ word has this once a pixel asked for its children in the frame it holds.
inline constexpr uint32_t kLightPieceAsked = 0x10000000u;
/// A level of a triangle's light is split, wherever its light is, as long as the next level's
/// points would still be this many times kLightPixels apart on screen; the deepest level on a
/// pixel's way down whose points are is its coarse level, whose points are traced first.
inline constexpr uint32_t kLightCoarsePieces = 2;
/// The most levels of pieces made at once under a piece, for the coarse levels.
inline constexpr uint32_t kLightPieceLevelsAtOnce = 4;
/// Frames a piece's four children are kept when no pixel reads any of them, nor anything under
/// them, while the pool is short of pages; a root likewise. With room they are kept longer
/// (velk_pieces_keep_frames): 8 times as long past a quarter of its pages free, 32 times past
/// half.
inline constexpr uint32_t kLightPieceKeepFrames = 32;
/// A piece's four children without children of their own are dropped when all their points and
/// the piece's carry the same light, each light fully through or fully blocked, for this many
/// frames in a row and one more: the piece's own points say as much.
inline constexpr uint32_t kLightPieceSameFrames = 7;

inline constexpr uint32_t kLightSlots = 8;
inline constexpr uint32_t kLightSegmentPages = 16384;
/// A segment's words a page: the page's, and after all its pages a blocker word for each two
/// words of them (velk_light_blocker_word). Mirrors VELK_LIGHT_SEGMENT_PAGE_WORDS, its pages' words.
inline constexpr uint32_t kLightSegmentWordsPerPage = kLightPageWords + kLightPageWords / 2u;
static_assert(uint64_t(kLightSegmentPages) * kLightPageWords == 1048576u, "Mirrors VELK_LIGHT_SEGMENT_PAGE_WORDS");
inline constexpr uint32_t kLightMaxSegments = 64;
/// The pool grows a quarter ahead of a frame asking when it stayed under a sixteenth free this
/// many frames, at most once every kLightPoolAheadEvery: past a full keep of the sweep's shortest
/// (VELK_PIECE_KEEP_FRAMES), so that pieces out of view were let go of first.
/// Pixels VELK_LIGHT_READ can name for the lighting to write its terms at (VELK_DEBUG_READ_TERMS),
/// and the words of each one's terms (VELK_LIGHT_READ_WORDS).
inline constexpr uint32_t kLightReadPixels = 4;
inline constexpr uint32_t kLightReadWords = 80;
inline constexpr uint32_t kLightPoolAheadFrames = 40;
inline constexpr uint32_t kLightPoolAheadEvery = 60;

/// The light pool's header, its words in order: each one's name in C++ (kLightPool, then the
/// first name) and in the shaders (VELK_POOL_, then the second), and its place. "This frame":
/// cleared by the sweep's begin (light_sweep_src), which copies it to its "last" word first, for
/// the log. "Summed": added up until the log reads and clears it.
#define VELK_LIGHT_POOL_HEADER(X)                                                                                    \
    X(FreePages, FREE_PAGES, 0)                  /* free pages on the stack */                                       \
    X(Pages, PAGES, 1)                           /* pages, in all the segments */                                    \
    X(PagesWanted, PAGES_WANTED, 2)              /* pages asked for this frame that were not there */                \
    X(RecordsPaged, RECORDS_PAGED, 3)            /* records with pages, this frame's mark */                         \
    X(RecordsOneValue, RECORDS_ONE_VALUE, 4)     /* records with one value, likewise */                              \
    X(Segments, SEGMENTS, 5)                     /* segments */                                                      \
    X(StackWord, STACK_WORD, 6)                  /* the word of the free stack */                                    \
    X(MarksWord, MARKS_WORD, 7)                  /* the word of the marks, a bit a page */                           \
    X(PagesTaken, PAGES_TAKEN, 8)                /* pages taken off the stack this frame, from its top down */       \
    X(Searched, SEARCHED, 9)                     /* records put on the search list this frame, past its room too */ \
    X(SearchWord, SEARCH_WORD, 10)               /* the word of the search list (kLightSearchWords a record) */      \
    X(LastRecordsPaged, LAST_RECORDS_PAGED, 12)                                                                      \
    X(LastRecordsOneValue, LAST_RECORDS_ONE_VALUE, 13)                                                               \
    X(LastSearched, LAST_SEARCHED, 15)                                                                               \
    X(SegmentWords, SEGMENT_WORDS, 16)           /* the word of each segment, kLightMaxSegments of them */           \
    X(Traces, TRACES, 80)                        /* lights put on the trace list this frame, past its room too */    \
    X(TraceWord, TRACE_WORD, 81)                 /* the word of the trace list (kLightTraceWords a light) */         \
    X(LastTraces, LAST_TRACES, 82)                                                                                   \
    X(UntracedPixels, UNTRACED_PIXELS, 83)       /* lit pixels this frame whose light was not traced yet */          \
    X(LastUntracedPixels, LAST_UNTRACED_PIXELS, 84)                                                                  \
    X(RaysListed, RAYS_LISTED, 85)               /* rays put on the trace list this frame, against RAY_BUDGET */     \
    X(RaysCast, RAYS_CAST, 86)                   /* rays the trace cast this frame */                                \
    X(Steps, STEPS, 87)                          /* the steps their walks took */                                    \
    X(WorstRays, WORST_RAYS, 88)                 /* RAYS_CAST of the frame with the most STEPS since the log */      \
    X(WorstSteps, WORST_STEPS, 89)               /* and its STEPS; the log clears both */                            \
    X(FirstRays, FIRST_RAYS, 90)                 /* of this frame's rays, those of first traces */                   \
    X(WorstFirstRays, WORST_FIRST_RAYS, 91)      /* FIRST_RAYS of the frame of WORST_RAYS */                         \
    X(SumTriangles, SUM_TRIANGLES, 92)           /* summed: the trace's triangles tested, in sixteens */             \
    X(SumRecords, SUM_RECORDS, 93)               /* summed: records tested */                                        \
    X(SumRays, SUM_RAYS, 94)                     /* summed: rays */                                                  \
    X(SumSteps, SUM_STEPS, 95)                   /* summed: walk steps, in sixteens */                               \
    X(PointRaysCharged, POINT_RAYS_CHARGED, 96)  /* rays the traced points were charged this frame, against RAY_BUDGET */ \
    X(SumFirstTraces, SUM_FIRST_TRACES, 98)      /* summed: points traced the first time */                          \
    X(SumLaterTraces, SUM_LATER_TRACES, 99)      /* summed: points traced again */                                   \
    X(DeepestTrace, DEEPEST_TRACE, 100)          /* the deepest walk stack in the trace, until the log */            \
    X(DeepestProbes, DEEPEST_PROBES, 101)        /* and in the screen probes */                                      \
    X(SumTopNodes, SUM_TOP_NODES, 102)           /* summed: walks' top tree nodes, in sixteens */                    \
    X(SumPartitionNodes, SUM_PARTITION_NODES, 103) /* summed: partitions' tree nodes, in sixteens */                 \
    X(SumMeshNodes, SUM_MESH_NODES, 104)         /* summed: meshes' or clusters' tree nodes, in sixteens */          \
    X(SumPartitions, SUM_PARTITIONS, 105)        /* summed: partitions entered */                                    \
    X(RayBudget, RAY_BUDGET, 106)                /* the frame's budget in rays, which adjust_light_rays sets */      \
    X(LastFirstRaysAsked, LAST_FIRST_RAYS_ASKED, 107) /* FIRST_RAYS_ASKED of the last frame */                       \
    X(LastRaysCharged, LAST_RAYS_CHARGED, 108)   /* the rays the last frame's work was charged */                    \
    X(FirstRaysAsked, FIRST_RAYS_ASKED, 109)     /* rays first traces asked for this frame, refused ones too */      \
    X(UntracedPoints, UNTRACED_POINTS, 110)      /* points on the untraced list this frame, past its room too */     \
    X(UntracedPointsWord, UNTRACED_POINTS_WORD, 112) /* the word of the untraced list (kLightPointWords a point) */   \
    X(LastUntracedPoints, LAST_UNTRACED_POINTS, 114)                                                                 \
    X(SumChanged, SUM_CHANGED, 116)              /* summed: records whose light a change made due */                 \
    X(FinePoints, FINE_POINTS, 117)              /* points on the fine list this frame, past its room too */         \
    X(CoarsePoints, COARSE_POINTS, 118)          /* points on the coarse list this frame, likewise */                \
    X(CoarsePointsWord, COARSE_POINTS_WORD, 119) /* the word of the coarse list */                                   \
    X(LastCoarsePoints, LAST_COARSE_POINTS, 120)                                                                     \
    X(SumRetraces, SUM_RETRACES, 121)            /* summed: traces of points traced from none again after a change */ \
    X(SumLaidNew, SUM_LAID_NEW, 122)             /* summed: own points laid out for records without light */         \
    X(SumLaidFromOne, SUM_LAID_FROM_ONE, 123)    /* summed: for records grown from one value */                      \
    X(SumPiecesMade, SUM_PIECES_MADE, 125)       /* summed: pieces made */                                           \
    X(SumPiecesFreed, SUM_PIECES_FREED, 126)     /* summed: pieces freed */                                          \
    X(SumSplitsRefused, SUM_SPLITS_REFUSED, 127) /* summed: asks for pieces not made, the list or the pool full */   \
    X(Splits, SPLITS, 128)                       /* asks for pieces this frame, past the split list's room too */    \
    X(SplitWord, SPLIT_WORD, 129)                /* the word of the split list (kLightSplitWords an ask) */          \
    X(LastSplits, LAST_SPLITS, 130)                                                                                  \
    X(PiecesWord, PIECES_WORD, 131)              /* the word of the piece bits, a bit a page */                      \
    X(Frame, FRAME, 132)                         /* the pool's frame, one more at every sweep's begin */             \
    X(MarkPieces, MARK_PIECES, 133)              /* this frame's mark: pieces kept */                                \
    X(MarkDeepest, MARK_DEEPEST, 134)            /* and the deepest of them */                                       \
    X(LastMarkPieces, LAST_MARK_PIECES, 135)                                                                         \
    X(LastMarkDeepest, LAST_MARK_DEEPEST, 136)                                                                       \
    X(Lights, LIGHTS, 137)                       /* the scene's lights, at most kLightSlots, as the last claim had them */ \
    X(FinePointsWord, FINE_POINTS_WORD, 138)     /* the word of the fine list */                                     \
    X(LastFinePoints, LAST_FINE_POINTS, 139)                                                                         \
    X(LongestWalk, LONGEST_WALK, 140)            /* the most walk steps one thread of the trace took, since the log */ \
    X(TraceArgs, TRACE_ARGS, 141)                /* the trace list's dispatch: groups x, y, z (velk_points_args) */  \
    X(PointArgs, POINT_ARGS, 144)                /* the point lists' dispatch: groups x, y, z */                     \
    X(PointTakes, POINT_TAKES, 147)              /* each point list's points traced this frame (velk_points_takes) */ \
    X(PointCounts, POINT_COUNTS, 151)            /* and its points, in their room */                                 \
    X(SpreadGroups, SPREAD_GROUPS, 155)          /* the point dispatch's groups for points traced from none */       \
    X(SumSlotChoices, SUM_SLOT_CHOICES, 156)     /* summed: records' lights chosen at their centre */                \
    X(SumLeftOutTenth, SUM_LEFT_OUT_TENTH, 157)  /* summed: of them, those leaving out a light a tenth as strong as their strongest */ \
    X(SumLeftOutQuarter, SUM_LEFT_OUT_QUARTER, 158) /* summed: and a quarter as strong */                            \
    X(SumListedBy, SUM_LISTED_BY, 159)           /* summed, every 16th frame: points listed by what lists them, four \
                                                    words (VELK_LIGHT_LISTED_*) */                                   \
    X(SumRecordsHit, SUM_RECORDS_HIT, 163)       /* summed: of the records rays tested, those whose box they passed */ \
    X(SumHitBy, SUM_HIT_BY, 164)                 /* summed: of those, patches, quads, without an instance, clusters, whole \
                                                    meshes, meshes without a tree: six words */                      \
    X(SumMeshWide, SUM_MESH_WIDE, 170)           /* summed: whole meshes walked by box size, four words (velk_ray_mesh_wide) */ \
    X(SumMeshTris, SUM_MESH_TRIS, 174)           /* summed: and by triangles, four words */                          \
    X(MostPagesWanted, MOST_PAGES_WANTED, 178)   /* the most pages any frame asked for that were not there, since \
                                                    the pool was last grown or looked at */                          \
    X(FillThreads, FILL_THREADS, 179)            /* threads that fill the device (compute_fill_threads) */           \
    X(SpreadLanes, SPREAD_LANES, 180)            /* the lanes this frame's points traced from none spread over */    \
    X(SumLaidInherited, SUM_LAID_INHERITED, 182) /* summed: own points laid out from a coarser record's (kLightInherit) */ \
    X(SumPiecesUnread, SUM_PIECES_UNREAD, 183)   /* summed: pieces let go of as no pixel read them */                \
    X(SumPiecesMerged, SUM_PIECES_MERGED, 184)   /* summed: pieces let go of with their parent's light */            \
    X(SumProbesRead, SUM_PROBES_READ, 185)       /* summed: screen probes that read their cell without rays, that \
                                                    added rays to it, that cast rays alone, and settled cells a \
                                                    change woke: four words */                                       \
    X(LastFreePages, LAST_FREE_PAGES, 189)       /* pages the last frame left on the stack, for the pieces' keep */  \
    X(DemandFrames, DEMAND_FRAMES, 190)          /* frames in a row, up to 3, that asked for pages not there */      \
    X(ListRays, LIST_RAYS, 194)                  /* the rays each point list's points this frame are due, past its \
                                                    room too: three words (velk_light_list_at) */                    \
    X(Debug, DEBUG, 203)                         /* the word of the debug block (VELK_LIGHT_DEBUG_BLOCK) while \
                                                    VELK_LIGHT_READ names pixels, kLightNone otherwise */            \
    X(HeaderEnd, HEADER_END, 204)

/// The debug block, a region of its own made only while VELK_LIGHT_READ names pixels (its word in
/// the pool's DEBUG): each word's name in C++ (kLightDebug, then the first name) and in the shaders
/// (VELK_DEBUG_, then the second), and its place in the block.
#define VELK_LIGHT_DEBUG_BLOCK(X)                                                                                    \
    X(Pending, PENDING, 0)                       /* the lit pixels' lights this frame that bring them light and \
                                                    whose points have not traced them yet, or a change made stale */ \
    X(LastRaysCast, LAST_RAYS_CAST, 2)           /* the last frame's RAYS_CAST, PENDING and READ_STAMP: three \
                                                    words, the same frame's, which the sweep's begin copies */       \
    X(ReadStamp, READ_STAMP, 6)                  /* the frame the lighting last ran in (present_counter), so that \
                                                    the copy above says its frame */                                 \
    X(ReadPixels, READ_PIXELS, 7)                /* VELK_LIGHT_READ's pixels, x | y << 16, kLightNone for none: \
                                                    kLightReadPixels words */                                        \
    X(ReadTerms, READ_TERMS, 11)                 /* the lighting's terms at them, kLightReadWords each \
                                                    (VELK_LIGHT_READ_*) */                                           \
    X(End, END, 331)

#define VELK_LIGHT_POOL_CPP_WORD(cpp, glsl, word) inline constexpr uint32_t kLightPool##cpp = word;
VELK_LIGHT_POOL_HEADER(VELK_LIGHT_POOL_CPP_WORD)
#undef VELK_LIGHT_POOL_CPP_WORD
#define VELK_LIGHT_POOL_GLSL_WORD(cpp, glsl, word) "#define VELK_POOL_" #glsl " " #word "u\n"
#define VELK_LIGHT_DEBUG_CPP_WORD(cpp, glsl, word) inline constexpr uint32_t kLightDebug##cpp = word;
VELK_LIGHT_DEBUG_BLOCK(VELK_LIGHT_DEBUG_CPP_WORD)
#undef VELK_LIGHT_DEBUG_CPP_WORD
/// The header's and the debug block's words, for the shaders: a #define VELK_POOL_ or VELK_DEBUG_ each.
#define VELK_LIGHT_DEBUG_GLSL_WORD(cpp, glsl, word) "#define VELK_DEBUG_" #glsl " " #word "u\n"
#define VELK_LIGHT_POOL_HEADER_GLSL                                                                                  \
    VELK_LIGHT_POOL_HEADER(VELK_LIGHT_POOL_GLSL_WORD) VELK_LIGHT_DEBUG_BLOCK(VELK_LIGHT_DEBUG_GLSL_WORD)
inline constexpr uint32_t kLightPoolHeaderWords = kLightPoolHeaderEnd;
static_assert(kLightPoolTraces == kLightPoolSegmentWords + kLightMaxSegments, "the segments' words end where the trace's begin");
static_assert(kLightDebugReadTerms == kLightDebugReadPixels + kLightReadPixels
                  && kLightDebugEnd == kLightDebugReadTerms + kLightReadPixels * kLightReadWords,
              "the read pixels' words, then their terms, end the debug block");
/// Records the search list takes a frame; more wait for the next.
inline constexpr uint32_t kLightSearchCapacity = 1024;  ///< Mirrors VELK_LIGHT_SEARCH_CAPACITY.
inline constexpr uint32_t kLightSearchWords = kSearchEntryWords;
/// Asks for pieces the split list takes a frame, and its words an ask (VELK_SPLIT_ENTRY in
/// cull_layout.h); more ask again on a later frame. The search makes them, a work group each.
inline constexpr uint32_t kLightSplitCapacity = 2048;  ///< Mirrors VELK_LIGHT_SPLIT_CAPACITY.
inline constexpr uint32_t kLightSplitWords = kSplitEntryWords;
/// Lights the trace list takes a frame, and its words a light (VELK_TRACE_ENTRY in
/// cull_layout.h).
inline constexpr uint32_t kLightTraceCapacity = 8192;  ///< Mirrors VELK_LIGHT_TRACE_CAPACITY.
inline constexpr uint32_t kLightTraceWords = kTraceEntryWords;
/// Points of light each of the three point lists takes a frame (VELK_LIGHT_LIST_UNTRACED and the
/// others), and their words a point (VELK_POINT_ENTRY in cull_layout.h). Past the frame's budget
/// of rays they wait for a later frame.
inline constexpr uint32_t kLightPointCapacity = 65536;  ///< Mirrors VELK_LIGHT_POINT_CAPACITY.
inline constexpr uint32_t kLightPointLists = 3;         ///< Mirrors VELK_LIGHT_LISTS.
inline constexpr uint32_t kLightPointWords = kPointEntryWords;
/// The light trace's budget in rays a frame to start with, which all of the frame's light work
/// shares by priority (velk_points_trace_point); what does not fit waits for a later frame.
/// adjust_light_rays then scales it by the GPU time the trace took against its target.
inline constexpr uint32_t kLightTraceRays = 65536;
/// The budget in rays when the trace has no time target: more than any frame lists.
inline constexpr uint32_t kLightTraceRaysUnlimited = 1u << 30;

/// What changed this frame, for light_changes_main_src: VELK_CHANGE_LIST, then a VELK_CHANGE a
/// change (cull_layout.h).
inline constexpr uint32_t kLightChangeHeaderWords = kChangeListChanges;
inline constexpr uint32_t kLightChangeWords = kChangeWords;
/// The most changes a frame lists; more count as everything changed.
inline constexpr uint32_t kLightChanges = 128;
/// On a change's light (kChangeLight): it only moved, by less than its size, so that what its
/// points have of it stays theirs while it is due (velk_change_stale), even when it is all the
/// light a record has, rather than the record's light being traced again from none.
inline constexpr uint32_t kLightChangeSmall = 0x40000000u;

/// Bounce light from screen probes: a probe per kLightProbePixels square of the screen, at the
/// surface its middle pixel shows. kLightProbeWords each (VELK_PROBE): its light as 12 halves
/// (velk_probe_read), its position relative to the camera (3 floats), its normal (snorm 16 bits
/// each, octahedral) and how many frames it has gathered (a float, 0 for no surface). Its light
/// is the mean radiance arriving at its own normal, weighted by cosine, as the lighting takes it,
/// and for each colour the direction it mostly comes from (L1), which carries it to other normals
/// (velk_probe_eval). Three stores: this frame's rays, and the filtered light of this frame and
/// the last, swapping.
inline constexpr uint32_t kLightProbePixels = 16;
inline constexpr uint32_t kLightProbeWords = kProbeWords;
/// Rays a probe traces a frame, each in its own quarter of the hemisphere.
inline constexpr uint32_t kLightProbeRays = 4;
/// Threads in a work group of the probes' gather: a 2 by 2 tile of probes, kLightProbeRays each.
inline constexpr uint32_t kLightProbeGroupThreads = 16;
/// Threads in a work group of the reflections' trace: 8 by 4 blocks of 2 by 2 pixels, a block each.
inline constexpr uint32_t kReflectGroupThreads = 32;
/// The most frames a glossy reflection blends (reflect_resolve_src): few while the camera moves,
/// more while it stands still; and the still frames after which the reflections are kept as they
/// are, neither traced nor resolved: the blend full, then each block's four pixels once more.
inline constexpr uint32_t kReflectMovingFrames = 16;
inline constexpr uint32_t kReflectSettledFrames = 64;
inline constexpr uint32_t kReflectStillFrames = kReflectSettledFrames + 4;
/// The probes' light kept in world space (VELK_PROBE_CELL_GLSL): a cell for a place and the way a
/// surface there faces, as wide as a probe's square is at its distance from the camera, found by
/// a hash. A probe whose cell has kProbeCellSettled samples and nothing changed since casts no
/// rays and takes its light; else one probe a frame adds its rays to it. Cells unread for
/// kProbeCellKeepFrames are let go of. A light or a box that changed near a cell
/// (kProbeCellLevels), or everything changing, starts its light moving again, from
/// kProbeCellWoken samples.
inline constexpr uint32_t kProbeCellSlots = 1u << 17;
inline constexpr uint32_t kProbeCellWords = 12;
inline constexpr uint32_t kProbeCellSettled = 64;
inline constexpr uint32_t kProbeCellWoken = 4;
inline constexpr uint32_t kProbeCellKeepFrames = 240;
/// The widths a cell can have, 2^(k - 3) for k below this: the bits of a change's VELK_CHANGE_WAKES.
/// A light wakes the cells of a width once it moved that far since it last woke them, or its
/// brightness changed: the light it casts around moves by as much, and a cell does not see a
/// move smaller than itself.
inline constexpr uint32_t kProbeCellLevels = 32;
/// The list of probes that cast rays this frame (light_probes_main_src): how many, then the
/// trace's dispatch arguments, then two words a probe.
inline constexpr uint32_t kProbeListHeaderWords = 4;
inline constexpr uint32_t kProbeListEntryWords = 2;
static_assert(kLightProbeRays * 4u == kLightProbeGroupThreads, "a group is a 2 by 2 tile of probes");

} // namespace velk

#endif // VELK_RENDER_FRAME_LIGHT_POOL_LAYOUT_H
