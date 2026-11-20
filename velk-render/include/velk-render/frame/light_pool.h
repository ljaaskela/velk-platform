#ifndef VELK_RENDER_FRAME_LIGHT_POOL_H
#define VELK_RENDER_FRAME_LIGHT_POOL_H

#include <velk/string_view.h>
#include <velk/vector.h>

#include <velk-render/frame/cull_layout.h>
#include <velk-render/interface/intf_gpu_arena.h>

#include <cstdint>
#include <cstdlib>
#include <utility>

namespace velk {

/// A view's light pool, in the culling arena: where the light of the kept cuts' records is kept
/// (kCutRecordWords, its last word). A record's light word is one of:
///   kLightNone      not read yet
///   kLightPending   being given its light, for the moment it takes
///   below kLightOneValue  the first page of its light
///   kLightOneValue and up  one value for the whole record, in the bits under kLightOneValue
/// Light is about kLightPixels apart on screen wherever it is. A record gets its light the first
/// time a pixel shows it (light_claim_src): one value for a cluster smaller than that on screen,
/// a whole mesh, a shape drawn by its own pass and a cluster of a mesh that deforms, whose
/// vertices are not its own. A larger cluster goes on the search list (light_search_src), which
/// gives it pages: where its instance has records for light only (kCutLightOnly), the coarser
/// clusters at the light's level, each of its vertices names the three points of the nearest
/// of their triangles, and those records get their own points; else it has a point per vertex
/// itself, being coarse enough. A quad record's light is on the node of its quad tree that is as
/// wide as kLightPixels over it, one value for all the quads under that node. A record's light
/// goes with it while its partition keeps its cut, and to its like when the partition chooses
/// again. Pages no record names, directly or through its vertices, are found free every frame
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
/// before that); with kLightLinked three words per vertex: the first page of the record for light
/// only whose points it reads, their numbers there (8 bits each, the first lowest), and the weights
/// of the second and third (unorm 16 bits each, the second lowest; the first's is what is left of 1).
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
/// (kLightOneShadeBits); whether it has been traced (kLightOneTraced); and the last frame it was
/// put on the trace list, its low 5 bits (kLightOneQueuedShift). Its lights are the strongest at
/// its centre, found where they are needed (velk_light_slots_at), not kept.
inline constexpr uint32_t kLightNone = 0xFFFFFFFFu;
inline constexpr uint32_t kLightPending = 0xFFFFFFFEu;
inline constexpr uint32_t kLightOneValue = 0x80000000u;
inline constexpr uint32_t kLightOneTraced = 0x40000000u;
inline constexpr uint32_t kLightOneShadeBits = 0x00FFFFFFu;
inline constexpr uint32_t kLightOneQueuedShift = 24;
inline constexpr uint32_t kLightOwn = 1u;
inline constexpr uint32_t kLightLinked = 0x80000000u;
inline constexpr uint32_t kLightPageWords = 64;

/// The header of a record's first page, its words in order, named as VELK_LIGHT_POOL_HEADER's
/// (kLightPage and VELK_PAGE_). The words from CHANGED on are its own points' only.
#define VELK_LIGHT_PAGE_HEADER(X)                                                                                    \
    X(Vertices, VERTICES, 0)          /* its cluster's vertices */                                                   \
    X(Kind, KIND, 1)                  /* own points: kLightOwn, or kLightPieced with its triangles; or kLightLinked */ \
    X(PointWords, POINT_WORDS, 2)     /* how many point words it has */                                              \
    X(Pages, PAGES, 3)                /* its other pages from the second, kLightNone for none */                     \
    X(Changed, CHANGED, 17)           /* the frame something last changed what its points could get, or kLightNone */ \
    X(Cluster, CLUSTER, 18)           /* its cluster's word in its hierarchy's table */                              \
    X(Reset, RESET, 19)               /* the frame its lights were last chosen again, or kLightNone */               \
    X(Slots, SLOTS, 20)               /* its lights, a light index a slot (kLightSlots), kLightNone for none */      \
    X(Index, INDEX, 28)               /* its draw's first index word */                                              \
    X(Coarse, COARSE, 30)             /* its coarse light: a one value from its cluster's sphere with its points'    \
                                         lights, read under its points until they are trusted */                     \
    X(Read, READ, 31)                 /* not 0 when a pixel read its points since the last mark; for the log */      \
    X(First, FIRST, 32)               /* its first page's number, which its points' blocker words go by */           \
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
/// them; a root likewise.
inline constexpr uint32_t kLightPieceKeepFrames = 32;
/// A piece's four children without children of their own are dropped when all their points and
/// the piece's carry the same light, each light fully through or fully blocked, for this many
/// frames in a row and one more: the piece's own points say as much.
inline constexpr uint32_t kLightPieceSameFrames = 7;

/// Traces a point of light takes, one a frame while a pixel reads it, after which it is traced
/// again only when something changes it (light_changes_main_src).
inline constexpr uint32_t kLightWarmTraces = 8;
inline constexpr uint32_t kLightSlots = 8;
inline constexpr uint32_t kLightSegmentPages = 16384;
/// A segment's words a page: the page's, and after all its pages a blocker word for each two
/// words of them (velk_light_blocker_word). Mirrors VELK_LIGHT_SEGMENT_PAGE_WORDS, its pages' words.
inline constexpr uint32_t kLightSegmentWordsPerPage = kLightPageWords + kLightPageWords / 2u;
static_assert(uint64_t(kLightSegmentPages) * kLightPageWords == 1048576u, "Mirrors VELK_LIGHT_SEGMENT_PAGE_WORDS");
inline constexpr uint32_t kLightMaxSegments = 64;

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
    X(RecordsLinked, RECORDS_LINKED, 11)         /* of the records with pages, those that link, this frame's mark */ \
    X(LastRecordsPaged, LAST_RECORDS_PAGED, 12)                                                                      \
    X(LastRecordsOneValue, LAST_RECORDS_ONE_VALUE, 13)                                                               \
    X(LastRecordsLinked, LAST_RECORDS_LINKED, 14)                                                                    \
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
    X(SumFirstTraces, SUM_FIRST_TRACES, 98)      /* summed: points traced the first time */                          \
    X(SumLaterTraces, SUM_LATER_TRACES, 99)      /* summed: points traced again */                                   \
    X(DeepestTrace, DEEPEST_TRACE, 100)          /* the deepest walk stack in the trace, until the log */            \
    X(DeepestProbes, DEEPEST_PROBES, 101)        /* and in the screen probes */                                      \
    X(SumTopNodes, SUM_TOP_NODES, 102)           /* summed: walks' top tree nodes, in sixteens */                    \
    X(SumPartitionNodes, SUM_PARTITION_NODES, 103) /* summed: partitions' tree nodes, in sixteens */                 \
    X(SumMeshNodes, SUM_MESH_NODES, 104)         /* summed: meshes' or clusters' tree nodes, in sixteens */          \
    X(SumPartitions, SUM_PARTITIONS, 105)        /* summed: partitions entered */                                    \
    X(RayBudget, RAY_BUDGET, 106)                /* the frame's budget in rays: STEP_BUDGET over a ray's steps */    \
    X(LastFirstRaysAsked, LAST_FIRST_RAYS_ASKED, 107) /* FIRST_RAYS_ASKED of the last frame */                       \
    X(StepBudget, STEP_BUDGET, 108)              /* the budget in walk steps a frame (kLightTraceStepsBudget) */     \
    X(FirstRaysAsked, FIRST_RAYS_ASKED, 109)     /* rays first traces asked for this frame, refused ones too */      \
    X(FirstPoints, FIRST_POINTS, 110)            /* points on the first trace list this frame, past its room too */  \
    X(LaterPoints, LATER_POINTS, 111)            /* points on the later trace list this frame, likewise */           \
    X(FirstPointsWord, FIRST_POINTS_WORD, 112)   /* the word of the first trace list (kLightPointWords a point) */   \
    X(LaterPointsWord, LATER_POINTS_WORD, 113)   /* the word of the later trace list */                              \
    X(LastFirstPoints, LAST_FIRST_POINTS, 114)                                                                       \
    X(LastLaterPoints, LAST_LATER_POINTS, 115)                                                                       \
    X(SumChanged, SUM_CHANGED, 116)              /* summed: records whose light a change made due */                 \
    X(LaterRestarts, LATER_RESTARTS, 117)        /* of the later points, those traced from none after a change */    \
    X(CoarsePoints, COARSE_POINTS, 118)          /* points on the coarse trace list this frame, past its room too */ \
    X(CoarsePointsWord, COARSE_POINTS_WORD, 119) /* the word of the coarse trace list */                             \
    X(LastCoarsePoints, LAST_COARSE_POINTS, 120)                                                                     \
    X(SumRetraces, SUM_RETRACES, 121)            /* summed: traces of points traced from none again after a change */ \
    X(SumLaidNew, SUM_LAID_NEW, 122)             /* summed: own points laid out for records without light */         \
    X(SumLaidFromOne, SUM_LAID_FROM_ONE, 123)    /* summed: for records grown from one value */                      \
    X(SumLaidLightOnly, SUM_LAID_LIGHT_ONLY, 124) /* summed: for records for light only */                           \
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
    X(HeaderEnd, HEADER_END, 137)

#define VELK_LIGHT_POOL_CPP_WORD(cpp, glsl, word) inline constexpr uint32_t kLightPool##cpp = word;
VELK_LIGHT_POOL_HEADER(VELK_LIGHT_POOL_CPP_WORD)
#undef VELK_LIGHT_POOL_CPP_WORD
/// The header's words, for the shaders: a #define VELK_POOL_ each.
#define VELK_LIGHT_POOL_GLSL_WORD(cpp, glsl, word) "#define VELK_POOL_" #glsl " " #word "u\n"
#define VELK_LIGHT_POOL_HEADER_GLSL VELK_LIGHT_POOL_HEADER(VELK_LIGHT_POOL_GLSL_WORD)
inline constexpr uint32_t kLightPoolHeaderWords = kLightPoolHeaderEnd;
static_assert(kLightPoolTraces == kLightPoolSegmentWords + kLightMaxSegments, "the segments' words end where the trace's begin");
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
/// Points of light each of the three point lists takes a frame (first traces of coarse points,
/// other first traces, later traces), and their words a point (VELK_POINT_ENTRY in
/// cull_layout.h). Coarse points are all traced; past the frame's budget of rays the others wait
/// for a later frame.
inline constexpr uint32_t kLightPointCapacity = 65536;  ///< Mirrors VELK_LIGHT_POINT_CAPACITY.
inline constexpr uint32_t kLightPointWords = kPointEntryWords;
/// Work groups of the trace for points from the point lists, after kLightTraceCapacity for the
/// trace list's: a point a thread.
inline constexpr uint32_t kLightPointGroups = 3u * kLightPointCapacity / 64u;
/// The walk steps the light trace may take a frame, about 2.5 ms of rays. Each frame it becomes
/// a budget in rays by the steps a ray took the frame before; lights past it wait for a later
/// frame, first traces before the others.
inline constexpr uint32_t kLightTraceStepsBudget = 4000000;
/// First traces other than coarse points' may take this many times the frame's budget, while
/// any are listed, so that what appears is traced soon; later traces keep the budget itself.
inline constexpr uint32_t kLightFirstBudgetScale = 4;  ///< Mirrors VELK_LIGHT_FIRST_BUDGET_SCALE.
/// Walk steps a ray is assumed to take before any has been traced, for the first frame's budget.
inline constexpr uint32_t kLightAssumedSteps = 64;

// Reading the pool, for a shader that defines VELK_LIGHT_WORDS as the culling arena's buffer.
#define VELK_LIGHT_POOL_GLSL VELK_LIGHT_POOL_HEADER_GLSL VELK_LIGHT_PAGE_HEADER_GLSL VELK_LIGHT_PIECE_PAGE_GLSL R"(
#define VELK_LIGHT_NONE 0xFFFFFFFFu
#define VELK_LIGHT_PENDING 0xFFFFFFFEu
#define VELK_LIGHT_ONE_VALUE 0x80000000u
#define VELK_LIGHT_OWN 1u
#define VELK_LIGHT_LINKED 0x80000000u
#define VELK_LIGHT_PAGE_WORDS 64u
#define VELK_LIGHT_SEGMENT_PAGE_WORDS 1048576u
#define VELK_LIGHT_WARM_TRACES 8u
#define VELK_LIGHT_DIRECT_PAGES 15u
#define VELK_LIGHT_PIECED 0x40000000u
#define VELK_LIGHT_SLOTS 8u
#define VELK_LIGHT_ONE_TRACED 0x40000000u
#define VELK_LIGHT_ONE_SHADE_BITS 0x00FFFFFFu
#define VELK_LIGHT_ONE_QUEUED_SHIFT 24u
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
// A split list entry's how (VELK_SPLIT_ENTRY_HOW) with this asks for a triangle's root.
#define VELK_LIGHT_SPLIT_ROOT 0x80000000u
// kLightSearchCapacity, kLightSplitCapacity, kLightTraceCapacity, kLightTraceWords.
#define VELK_LIGHT_SEARCH_CAPACITY 1024u
#define VELK_LIGHT_SPLIT_CAPACITY 2048u
#define VELK_LIGHT_TRACE_CAPACITY 8192u
#define VELK_LIGHT_TRACE_WORDS VELK_TRACE_ENTRY_WORDS
// kLightPointCapacity, kLightPointWords, kLightFirstBudgetScale.
#define VELK_LIGHT_POINT_CAPACITY 65536u
#define VELK_LIGHT_POINT_WORDS VELK_POINT_ENTRY_WORDS
#define VELK_LIGHT_FIRST_BUDGET_SCALE 4u
// The point lists (velk_light_list_point): first traces, later traces, first traces of coarse
// points. Their counts and words in the pool's header.
#define VELK_LIGHT_LIST_FIRST 0u
#define VELK_LIGHT_LIST_LATER 1u
#define VELK_LIGHT_LIST_COARSE 2u
// What a light on the trace list is in: a record of one value whose sphere is its cluster's, one
// whose sphere is its whole mesh's, a quad tree node.
#define VELK_LIGHT_TRACE_CLUSTER 1u
#define VELK_LIGHT_TRACE_MESH 2u
#define VELK_LIGHT_TRACE_NODE 3u
// A record's coarse light (VELK_PAGE_COARSE of the first page the entry names).
#define VELK_LIGHT_TRACE_COARSE 4u
// How many traces of a point it takes for it to replace its record's coarse light outright.
#define VELK_LIGHT_TRUSTED_TRACES 8.0
// kLightPixels.
#define VELK_LIGHT_PIXELS 4.0

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
    return (kind & VELK_LIGHT_PIECED) != 0u && kind != VELK_LIGHT_LINKED ? min(kind & 0xFFFFu, 256u) : 0u;
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
// slot, 4 bits of how far its last blocked ray went to what blocked it (velk_light_blocker_code),
// 0 for none. Not in the pages: each segment has a word after its pages for each two words of
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

// A blocker's distance as 4 bits: 1 for 1/128 or nearer, doubling each step, 15 for 128 or more.
uint velk_light_blocker_code(float d)
{
    return uint(clamp(round(log2(max(d, 1e-6))) + 8.0, 1.0, 15.0));
}

float velk_light_blocker_distance(uint code)
{
    return exp2(float(code) - 8.0);
}

// A point's trace count (its state's top 4 bits): VELK_LIGHT_WARM_TRACES traces warm it up from
// none, after which it is settled.

// The trace count of the own point of the record whose first page starts at `first` with trace
// state `state`: none when its record's lights were chosen again, or something changed what it
// could get, since it was last listed. Such a point keeps its light word until it is traced
// again, but its light is stale: pixels read what is under it, and its next trace starts over.
uint velk_light_point_traces(uint first, uint state)
{
    if (state == VELK_LIGHT_NONE) return 0u;
    uint listed = state & 0x0FFFFFFFu;
    uint reset = VELK_LIGHT_WORDS.data[first + VELK_PAGE_RESET];
    if (reset != VELK_LIGHT_NONE && listed < reset) return 0u;
    uint changed = VELK_LIGHT_WORDS.data[first + VELK_PAGE_CHANGED];
    if (changed != VELK_LIGHT_NONE && listed < changed) return 0u;
    return state >> 28u;
}

// Whether a point with trace count `traces` is due a trace: not traced, or warming up.
bool velk_light_point_due(uint traces)
{
    return traces < VELK_LIGHT_WARM_TRACES;
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
// as the words they are in, their numbers there and their weights: its own point, or the three
// it links to. A word is VELK_LIGHT_NONE where there is no point. `owner` gets the first page of
// the record the points are in, whose lights their slots are, VELK_LIGHT_NONE for none.
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
    if (VELK_LIGHT_WORDS.data[first + VELK_PAGE_KIND] != VELK_LIGHT_LINKED) {
        words[0] = velk_light_own_word(pool, first, vertex);
        owner = first;
        return;
    }
    uint light = VELK_LIGHT_WORDS.data[velk_light_point_word(pool, first, vertex * 3u)];
    if (light >= VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES]) return;
    uint points = VELK_LIGHT_WORDS.data[velk_light_point_word(pool, first, vertex * 3u + 1u)];
    vec2 w = unpackUnorm2x16(VELK_LIGHT_WORDS.data[velk_light_point_word(pool, first, vertex * 3u + 2u)]);
    uint other = velk_light_page(pool, light);
    for (uint k = 0u; k < 3u; ++k) {
        uint p = (points >> (8u * k)) & 255u;
        ids[k] = p;
        if (p < VELK_LIGHT_WORDS.data[other + VELK_PAGE_VERTICES]) words[k] = velk_light_own_word(pool, other, p);
    }
    weights = vec3(max(1.0 - w.x - w.y, 0.0), w.x, w.y);
    owner = other;
}

// A trace list entry's first word: what it is (VELK_LIGHT_TRACE_KIND bits: a VELK_LIGHT_TRACE_
// kind, or VELK_LIGHT_TRACE_SKIP for nothing).
#define VELK_LIGHT_TRACE_KIND 15u
#define VELK_LIGHT_TRACE_SKIP 15u

)" R"(
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
// turned bit 26, the depth from bit 27. A root, at depth 0, is at (0, 0) and keeps its grid's
// level in bits 0 to 2 instead (velk_piece_root_place).
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
    return tri_px / (float(depth == 0u ? root_level : VELK_PIECE_LEVEL) * exp2(float(depth)));
}

// The level of a root's grid for a triangle whose longest edge is `tri_px` pixels long, its points
// about `light_pixels` apart: one of VELK_PIECE_ROOT_LEVELS.
uint velk_piece_root_level(float tri_px, float light_pixels)
{
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

// The way down for a pixel at weights `b` of its triangle, whose longest edge is `tri_px` pixels
// long there and whose root piece is `root` (none at or past the pool's pages), to points at most
// `light_pixels` apart.
VelkPieceWay velk_piece_way(uint pool, uint root, vec3 b, float tri_px, float light_pixels)
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
    w.spacing = velk_piece_spacing(0u, w.root_level, tri_px);
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
        w.spacing = velk_piece_spacing(w.depth, w.root_level, tri_px);
    }
    return w;
}

)" R"(
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
    velk_light_around(velk_piece_level(VELK_LIGHT_WORDS.data[first + VELK_PIECE_PLACE]), l, places, weights);
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

)" R"(
// What the cell of a pixel's coarse level (VelkPieceWay) around it says about the levels under
// it, in the triangle with corners `corners` of the record whose first page starts at `first`:
// 0 when it is not known yet (a point not traced, or with `fresh`, stale since a change), 1 when
// its three points carry the same light, each light fully lit or fully blocked, so that the
// levels under it could only repeat it: exact; 2 when they differ. Without `fresh`, a cell whose
// stale points agree is
// not exact where a place next to them in its piece has other light: an edge could cross into it
// by the time they are traced again.
uint velk_way_cell(uint pool, uint first, uvec3 corners, VelkPieceWay way, bool fresh)
{
    uint words[3];
    vec3 w;
    velk_way_around(pool, first, corners, way.coarse, way.coarse_at, words, w);
    uint shade = VELK_LIGHT_NONE;
    bool same = true;
    bool stale = false;
    for (uint k = 0u; k < 3u; ++k) {
        uint s = VELK_LIGHT_WORDS.data[words[k]];
        if (s == VELK_LIGHT_NONE) return 0u;
        bool old = velk_light_point_traces(first, VELK_LIGHT_WORDS.data[words[k] + 1u]) == 0u;
        if (fresh && old) return 0u;
        stale = stale || old;
        if (k == 0u) shade = s;
        else if (s != shade) same = false;
    }
    // Every light's four bits all clear or all set.
    if (!same || shade != (shade & 0x11111111u) * 15u) return 2u;
    if (fresh || !stale || way.coarse == VELK_PIECE_VERTICES) return 1u;
    uint at = velk_light_page(pool, way.coarse);
    uint n = velk_piece_level(VELK_LIGHT_WORDS.data[at + VELK_PIECE_PLACE]);
    uvec2 places[3];
    velk_light_around(n, way.coarse_at, places, w);
    const ivec2 kAround[6] = ivec2[6](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1), ivec2(1, -1), ivec2(-1, 1));
    for (uint k = 0u; k < 3u; ++k) {
        for (uint a = 0u; a < 6u; ++a) {
            ivec2 q = ivec2(places[k]) + kAround[a];
            if (q.x < 0 || q.y < 0 || q.x + q.y > int(n)) continue;
            uint s = VELK_LIGHT_WORDS.data[at + 2u * velk_piece_point(uint(q.x), uint(q.y))];
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
    return list == VELK_LIGHT_LIST_COARSE ? VELK_POOL_COARSE_POINTS : VELK_POOL_FIRST_POINTS + list;
}
uint velk_light_list_word_word(uint list)
{
    return list == VELK_LIGHT_LIST_COARSE ? VELK_POOL_COARSE_POINTS_WORD : VELK_POOL_FIRST_POINTS_WORD + list;
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
)"

// Putting lights on the trace list, after VELK_LIGHT_POOL_GLSL, for the shaders that list them
// (the claim and the search); apart, since it writes the pool and shaders that only read it
// declare it read-only.
#define VELK_LIGHT_LIST_GLSL R"(
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
    velk_light_list_entry(pool, VELK_LIGHT_TRACE_COARSE, page, instance, VELK_LIGHT_WORDS.data[first + VELK_PAGE_CLUSTER],
                          8u * max(lights, 1u));
}

// Puts the point of light whose words start at `word` on a point list, as `entry_first` and
// `entry_point` (VELK_POINT_ENTRY), once a frame, when it is due (velk_light_point_due); `first`
// is the first page of its record, whose changes make it stale. Counting as untraced, on the
// coarse list when it is a `coarse` point (a vertex's, or a coarse piece's), so that it is traced
// this frame; else on the first trace list when it was never traced, or the later one when a
// change made its light stale, which the coarser light under it stands in for meanwhile. Warming
// up, on the later one. Its trace state takes the frame, and its count as
// velk_light_point_traces says, so that the trace reads which trace this is. Past the list's room
// it waits for a later frame.
void velk_light_list_at(uint pool, uint word, uint first, uint entry_first, uint entry_point, uint instance, uint frame,
                        bool coarse)
{
    uint at = word + 1u;
    uint state = VELK_LIGHT_WORDS.data[at];
    uint traces = velk_light_point_traces(first, state);
    if (!velk_light_point_due(traces)) return;
    uint now = frame & 0x0FFFFFFFu;
    if (state != VELK_LIGHT_NONE && (state & 0x0FFFFFFFu) == now) return;
    if (atomicCompSwap(VELK_LIGHT_WORDS.data[at], state, (traces << 28u) | now) != state) return;
    bool stale = traces == 0u && VELK_LIGHT_WORDS.data[at - 1u] != VELK_LIGHT_NONE;
    uint list = traces == 0u && coarse ? VELK_LIGHT_LIST_COARSE
              : traces == 0u && !stale ? VELK_LIGHT_LIST_FIRST
                                       : VELK_LIGHT_LIST_LATER;
    if (list == VELK_LIGHT_LIST_LATER && stale) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_LATER_RESTARTS], 1u);
    uint i = atomicAdd(VELK_LIGHT_WORDS.data[pool + velk_light_list_count_word(list)], 1u);
    if (i >= VELK_LIGHT_POINT_CAPACITY) return;
    uint w = VELK_LIGHT_WORDS.data[pool + velk_light_list_word_word(list)] + i * VELK_LIGHT_POINT_WORDS;
    VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_FIRST] = entry_first;
    VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_POINT] = entry_point;
    VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_INSTANCE] = instance;
}

// Puts own point `p` of the record whose first page starts at `first` on a point list
// (velk_light_list_at).
void velk_light_list_point(uint pool, uint first, uint p, uint instance, uint frame, bool coarse)
{
    if (p >= velk_light_own_points(first)) return;
    velk_light_list_at(pool, velk_light_own_word(pool, first, p), first, first, p, instance, frame, coarse);
}

// Puts point `q` of the piece at page `page`, whose record's first page starts at `first`, on a
// point list (velk_light_list_at).
void velk_light_list_piece_point(uint pool, uint page, uint q, uint first, uint instance, uint frame, bool coarse)
{
    if (q >= VELK_PIECE_POINTS) return;
    velk_light_list_at(pool, velk_light_page(pool, page) + 2u * q, first, page, q | VELK_LIGHT_POINT_PIECE, instance, frame,
                       coarse);
}
)"

// The lights a place keeps, for a shader built on the deferred prelude with the light grid
// (VELK_LIGHT_GRID_GLSL): Light, velk_light_at, VELK_GLOBALS and pc.lights_base. Positions are
// relative to the camera.
#define VELK_LIGHT_SLOTS_GLSL R"(
// How much `light` could bring a place at `at`, as a luminance, whichever way the place faces.
float velk_light_strength(Light light, vec3 at)
{
    float bright = dot(light.color_intensity.rgb, vec3(0.2126, 0.7152, 0.0722)) * light.color_intensity.a;
    if (light.flags.x == 0u) return bright;
    vec3 to = light.position.xyz - at;
    float dist = length(to);
    float range = light.params.x;
    if (range > 0.0 && dist >= range) return 0.0;
    if (light.flags.x == 2u && dot(-to / max(dist, 1e-6), light.direction.xyz) <= light.params.z) return 0.0;
    float t = range > 0.0 ? 1.0 - dist / range : 1.0 / (1.0 + dist * dist);
    return bright * t * t;
}

// The lights that could bring the place at `at` the most, strongest first, as light indices
// from pc.lights_base, VELK_LIGHT_NONE in the slots left over.
void velk_light_slots_at(vec3 at, out uint slots[VELK_LIGHT_SLOTS])
{
    float most[VELK_LIGHT_SLOTS];
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        slots[s] = VELK_LIGHT_NONE;
        most[s] = 0.0;
    }
    uint first;
    uint candidates = velk_light_grid_count(at + VELK_GLOBALS.cam_pos.xyz, first);
    for (uint c = 0u; c < candidates; ++c) {
        uint i = velk_light_grid_at(first, c);
        float strength = velk_light_strength(velk_light_at(pc.lights_base + i), at);
        if (strength <= most[VELK_LIGHT_SLOTS - 1u]) continue;
        uint s = VELK_LIGHT_SLOTS - 1u;
        while (s > 0u && most[s - 1u] < strength) {
            most[s] = most[s - 1u];
            slots[s] = slots[s - 1u];
            --s;
        }
        most[s] = strength;
        slots[s] = i;
    }
}
)"

// Adding up the lights the points of light give a pixel or a ray's hit: needs
// VELK_LIGHT_POOL_GLSL and VELK_LIGHT_SLOTS_GLSL first.
#define VELK_LIGHT_ADD_GLSL R"(
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

// How wide the penumbra of `light` is at `at` (relative to the camera, as the light is) behind a
// blocker `d` from it toward the light, in world units: the light's spread seen from the
// blocker, carried on to `at`.
float velk_light_penumbra(Light light, vec3 at, float d)
{
    if (light.flags.x == 0u) return 2.0 * d * tan(light.params.w);
    float to_light = length(light.position.xyz - at);
    return 2.0 * light.params.w * d / max(to_light - d, 1e-3);
}

// A point of light's lights, from its word `word` in the record whose first page is `owner`,
// with `weight`: over the record's coarse light, by how many times the point has been traced
// (VELK_LIGHT_TRUSTED_TRACES), so that its first few rays do not show as jumps. False when
// neither is traced yet.
bool velk_light_add_point(uint word, uint owner, float weight, inout uint lights[VELK_LIGHT_SLOTS],
                          inout vec2 sums[VELK_LIGHT_SLOTS])
{
    uint shade = VELK_LIGHT_WORDS.data[word];
    uint coarse = VELK_LIGHT_WORDS.data[owner + VELK_PAGE_COARSE];
    bool has_coarse = coarse >= VELK_LIGHT_ONE_VALUE && coarse < VELK_LIGHT_PENDING && (coarse & VELK_LIGHT_ONE_TRACED) != 0u;
    if (shade == VELK_LIGHT_NONE && !has_coarse) return false;
    uint traces = velk_light_point_traces(owner, VELK_LIGHT_WORDS.data[word + 1u]);
    float trust = shade == VELK_LIGHT_NONE ? 0.0 : min(float(traces), VELK_LIGHT_TRUSTED_TRACES) / VELK_LIGHT_TRUSTED_TRACES;
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
// under it, or left out when nothing is. `nearest` keeps the nearest blocker the points had, a
// light at a time, when `sized`. True when every point it reads counts fully, so that what was
// under does not show.
bool velk_piece_light(uint pool, uint page, uint first, uvec3 corners, vec3 l, bool sized, inout bool known,
                      inout float under[VELK_LIGHT_SLOTS], inout uint nearest)
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
            uint traces = velk_light_point_traces(first, VELK_LIGHT_WORDS.data[words[k] + 1u]);
            trust = !known ? 1.0 : min(float(traces), 1.0);
            if (trust < 1.0) full = false;
            if (trust > 0.0 && sized) {
                uint codes = VELK_LIGHT_WORDS.data[velk_way_blocker(pool, first, corners, page, k, words[k])];
                // The nearest per light: the lower of each 4 bits, 0 counting as none.
                for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
                    uint c = (codes >> (4u * s)) & 15u;
                    uint had = (nearest >> (4u * s)) & 15u;
                    if (c != 0u && c < had) nearest = (nearest & ~(15u << (4u * s))) | (c << (4u * s));
                }
            }
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

// The lights of the points a pixel or hit at weights `b` of triangle `t` (corners `corners`) of
// the record whose first page starts at `first` reads, added one at a time. False when none of
// them is traced yet. `at` is relative to the camera, `px_per_world` the pixels a world unit is
// there and `tri_size` the triangle's longest edge; a hit passes 0 for both and reads its
// triangle's corners.
//
// In pieces: the level the pixel's way down reaches (velk_piece_way), over the record's coarse
// light; its coarse level where that level's cell is exact (velk_way_cell, its points traced and
// not stale). Where a point of it is not traced yet, or stale since a change, the levels above
// show through: its coarse level, then the one above it, then it.
bool velk_light_add_points(uint pool, uint first, uint t, uvec3 corners, vec3 b, vec3 at, float px_per_world,
                           float tri_size, inout uint lights[VELK_LIGHT_SLOTS], inout vec2 sums[VELK_LIGHT_SLOTS])
{
    bool traced = false;
    if (px_per_world > 0.0 && t < velk_light_pieced(first)) {
        uint root = VELK_LIGHT_WORDS.data[velk_light_root_word(pool, first, t)];
        float tri_px = tri_size * px_per_world;
        VelkPieceWay way = velk_piece_way(pool, root, b, tri_px, VELK_LIGHT_PIXELS);
        if (velk_way_cell(pool, first, corners, way, true) == 1u) velk_way_stop_at_coarse(way);
        uint coarse = VELK_LIGHT_WORDS.data[first + VELK_PAGE_COARSE];
        bool coarse_known = coarse >= VELK_LIGHT_ONE_VALUE && coarse < VELK_LIGHT_PENDING && (coarse & VELK_LIGHT_ONE_TRACED) != 0u;
        float base[VELK_LIGHT_SLOTS];
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) base[s] = 1.0 - float((coarse >> (3u * s)) & 7u) / 7.0;
        // Blockers are read only for a light with a size: without one its penumbra has none.
        bool sized = false;
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
            uint slot = VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s];
            if (slot != VELK_LIGHT_NONE && velk_light_at(pc.lights_base + slot).params.w > 0.0) sized = true;
        }
        float under[VELK_LIGHT_SLOTS];
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) under[s] = base[s];
        bool known = coarse_known;
        uint nearest = 0xFFFFFFFFu;
        if (!velk_piece_light(pool, way.piece, first, corners, way.at, sized, known, under, nearest)) {
            for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) under[s] = base[s];
            known = coarse_known;
            nearest = 0xFFFFFFFFu;
            if (way.coarse != way.piece && way.coarse != way.parent) {
                velk_piece_light(pool, way.coarse, first, corners, way.coarse_at, false, known, under, nearest);
            }
            if (way.parent != VELK_LIGHT_NONE) {
                velk_piece_light(pool, way.parent, first, corners, way.parent_at, false, known, under, nearest);
            }
            velk_piece_light(pool, way.piece, first, corners, way.at, sized, known, under, nearest);
        }
        if (!known) return false;
        // Where a light's penumbra (velk_light_penumbra) is wider on screen than the points'
        // spacing, toward the coarse level, whose points are farther apart, so that a wide
        // penumbra's single rays do not show. Narrower, the points as they are: they cannot say
        // where between them an edge is. A light without a size has no penumbra.
        if (sized && way.coarse != way.piece) {
            float fine_px = way.spacing;
            float coarse_px = way.coarse_spacing;
            float blend[VELK_LIGHT_SLOTS];
            bool any = false;
            for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
                blend[s] = 0.0;
                uint slot = VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s];
                if (slot == VELK_LIGHT_NONE) continue;
                Light light = velk_light_at(pc.lights_base + slot);
                if (light.params.w <= 0.0) continue;
                // 15 is none read, or a blocker so far away that the edge is wider than any piece.
                uint c = (nearest >> (4u * s)) & 15u;
                if (c == 15u) continue;
                float wide = velk_light_penumbra(light, at, velk_light_blocker_distance(c)) * px_per_world;
                if (wide > fine_px) {
                    blend[s] = clamp((wide - fine_px) / max(coarse_px - fine_px, 1e-3), 0.0, 1.0);
                    any = true;
                }
            }
            if (any) {
                float coarse_light[VELK_LIGHT_SLOTS];
                for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) coarse_light[s] = base[s];
                bool coarse_light_known = coarse_known;
                uint unused = 0xFFFFFFFFu;
                velk_piece_light(pool, way.coarse, first, corners, way.coarse_at, false, coarse_light_known, coarse_light, unused);
                if (coarse_light_known) {
                    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) under[s] = mix(under[s], coarse_light[s], blend[s]);
                }
            }
        }
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
)"

// What a pixel's light is, for the lighting pass: VELK_LIGHT_ADD_GLSL and velk_light_direct, which
// needs velk_instances too.
#define VELK_LIGHT_READ_GLSL VELK_LIGHT_ADD_GLSL R"(
// The direct light the pixel with ids `ids` at `at` (relative to the camera) gets from the
// points of light: up to VELK_LIGHT_SLOTS lights, filled in order, the rest VELK_LIGHT_NONE, and
// how much of each gets through. False when it has no traced light yet. `b` is where the pixel
// is in its triangle (velk_pixel_weights), `tri_size` the triangle's longest edge in world
// units, `light_pixels` kLightPixels.
bool velk_light_direct(uint pool, uvec2 ids, vec3 b, vec3 at, float tri_size, float light_pixels,
                       out uint lights[VELK_LIGHT_SLOTS], out float through[VELK_LIGHT_SLOTS])
{
    vec2 sums[VELK_LIGHT_SLOTS];
    for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
        lights[k] = VELK_LIGHT_NONE;
        sums[k] = vec2(0.0);
        through[k] = 0.0;
    }
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint record = velk_ids_record(d, ids);
    if (record == VELK_NO_RECORD) return false;
    GlobalData g = velk_globals.data[d.globals_base];
    uint entry = VELK_LIGHT_WORDS.data[record + VELK_RECORD_ENTRY];

    if (entry == VELK_CUT_QUAD) {
        mat4 vp = g.view_projection;
        float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
        uint node = velk_light_quad_node(VELK_LIGHT_WORDS.data[record + VELK_RECORD_QUAD_TREE], VELK_LIGHT_WORDS.data[record + VELK_RECORD_QUAD_NODE],
                                         g.cam_pos.xyz, pixels_per_unit, light_pixels);
        uint value = VELK_LIGHT_WORDS.data[node + VELK_QUAD_NODE_LIGHT];
        if (value >= VELK_LIGHT_PENDING || (value & VELK_LIGHT_ONE_TRACED) == 0u) return false;
        vec3 center = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[node + VELK_QUAD_NODE_CENTER]),
                           uintBitsToFloat(VELK_LIGHT_WORDS.data[node + VELK_QUAD_NODE_CENTER + 1u]),
                           uintBitsToFloat(VELK_LIGHT_WORDS.data[node + VELK_QUAD_NODE_CENTER + 2u])) - g.cam_pos.xyz;
        velk_light_add_one(value, center, lights, sums);
    } else {
        uint value = VELK_LIGHT_WORDS.data[record + VELK_RECORD_LIGHT];
        if (value >= VELK_LIGHT_PENDING) return false;
        ElementInstance inst = velk_instances.data[d.instances_base + velk_ids_slot(d, ids)];
        if (value >= VELK_LIGHT_ONE_VALUE) {
            if ((value & VELK_LIGHT_ONE_TRACED) == 0u) return false;
            // From its cluster's sphere, or its whole mesh's.
            uint sphere = entry == 0xFFFFFFFFu ? VELK_LIGHT_WORDS.data[record + VELK_RECORD_MESH_TABLE]
                        : VELK_LIGHT_WORDS.data[entry + VELK_CLUSTER_ENTRY_TABLE] + VELK_LIGHT_WORDS.data[record + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS + VELK_LOD_CLUSTER_OWN_SPHERE;
            vec3 local = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere]), uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere + 1u]),
                              uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere + 2u]));
            velk_light_add_one(value, velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g), lights,
                               sums);
        } else {
            if (value >= VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES]) return false;
            uint first = velk_light_page(pool, value);
            uint cluster = velk_ids_cluster(d, ids);
            if (cluster == VELK_NO_CLUSTER) return false;
            // The triangle's corners, and how many pixels a world unit at the pixel is.
            uint word = d.index_base + velk_cluster_first_index(cluster) + velk_ids_triangle(d, ids) * 3u;
            uvec3 corners = uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
            mat4 vp = g.view_projection;
            float px_per_world = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5 / max(length(at), 1e-4);
            if (!velk_light_add_points(pool, first, velk_ids_triangle(d, ids), corners, b, at, px_per_world, tri_size, lights, sums)) {
                return false;
            }
        }
    }
    for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) through[k] = sums[k].y / max(sums[k].x, 1e-6);
    return true;
}
)"

/**
 * Traces the one values on the trace list (light_claim_src lists them), a group of threads
 * each, eight rays a light from its centre; then, in kLightPointGroups more groups, the points
 * of light on the point lists, a point a thread (velk_points_trace_point). Built on the light
 * grid's code (light_grid_src), whose velk_light_cell_words is the culling arena; pc.spare3 is
 * the pool.
 */
inline constexpr string_view light_points_trace_main_src = R"(
#define VELK_LIGHT_WORDS velk_light_cell_words
)" VELK_LIGHT_POOL_GLSL VELK_LIGHT_SLOTS_GLSL R"(
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
uint g_first_rays = 0u;  // of those, for lights traced for the first time

// How much of the light from a point of `light` chosen by `seed` is kept from `p` (facing `n`,
// `lift` off its surface): 1 where something is in the way, 0 where nothing is, between for
// patches of small members on the way (VELK_CUT_RAY_QUAD). How far away what kept it was is in
// velk_ray_blocker_t, 0 when the surface faces away.
float velk_points_blocked(Light light, vec3 p, vec3 n, float lift, uint seed)
{
    velk_ray_blocker_t = 0.0;
    vec2 r = vec2(float(velk_points_hash(seed) & 0xFFFFu), float(velk_points_hash(seed ^ 0x9e3779b9u) & 0xFFFFu)) / 65535.0;
    vec3 dir;
    float t_max = 1e30;
    if (light.flags.x == 0u) {
        vec3 l = -light.direction.xyz;
        vec3 a = normalize(abs(l.y) < 0.9 ? cross(l, vec3(0.0, 1.0, 0.0)) : cross(l, vec3(1.0, 0.0, 0.0)));
        vec3 b = cross(l, a);
        float spread = tan(light.params.w) * sqrt(r.x);
        float turn = 6.2831853 * r.y;
        dir = normalize(l + (a * cos(turn) + b * sin(turn)) * spread);
    } else {
        vec3 target = light.position.xyz;
        if (light.flags.x == 3u) {
            vec3 a;
            vec3 b;
            velk_area_axes(light.direction.xyz, light.params.w, a, b);
            target += a * (2.0 * r.x - 1.0) + b * (2.0 * r.y - 1.0);
        } else if (light.params.w > 0.0) {
            float turn = 6.2831853 * r.y;
            float z = 2.0 * r.x - 1.0;
            target += light.params.w * vec3(sqrt(1.0 - z * z) * cos(turn), z, sqrt(1.0 - z * z) * sin(turn));
        }
        vec3 to = target - p;
        float dist = length(to);
        dir = to / max(dist, 1e-6);
        t_max = dist * 0.999;
    }
    // Facing away, the surface is in its own way.
    if (dot(n, dir) <= 0.0) return 1.0;
    Ray ray;
    ray.origin = p + n * lift;
    ray.dir = dir;
    ++g_rays;
    return 1.0 - trace_visibility(ray, max(t_max - lift, 0.0));
}

// This thread's rays and their walks' steps, into the frame's counts (pool header 86 and 87).
void velk_points_count(uint pool)
{
    if (g_rays != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_RAYS_CAST], g_rays);
    // Exact, as the frame's budget in rays is made from it.
    if (velk_ray_steps != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_STEPS], velk_ray_steps);
    if (g_first_rays != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_FIRST_RAYS], g_first_rays);
    // Added up until the log reads them: triangles tested, records, rays and steps.
    if (velk_ray_tris != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_TRIANGLES], (velk_ray_tris + 15u) / 16u);
    if (velk_ray_records != 0u) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_RECORDS], velk_ray_records);
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

)" R"(
// The slot of `slots` to trace once more at `p`, chosen at random by `seed` in proportion to
// how much each could bring.
uint velk_points_pick(uint slots[VELK_LIGHT_SLOTS], vec3 p, uint seed)
{
    float strengths[VELK_LIGHT_SLOTS];
    float total = 0.0;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        strengths[s] = slots[s] == VELK_LIGHT_NONE ? 0.0 : velk_light_strength(velk_light_at(pc.lights_base + slots[s]), p);
        total += strengths[s];
    }
    float pick = float(velk_points_hash(seed) & 0xFFFFFFu) / 16777215.0 * total;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        if (pick < strengths[s]) return s;
        pick -= strengths[s];
    }
    return 0u;
}

// Traces the point of light at place `i` of the frame's points to trace: all of the coarse list,
// then the first trace list's, as many as VELK_LIGHT_FIRST_BUDGET_SCALE times the frame's budget
// in rays has left after the trace list's (pool header 85 of 106) and the coarse points', then
// the later one's, as many as the budget itself has left, each starting at a place that moves
// every frame, so that what waits is not always the same. A point never traced, or stale since a
// change, takes a ray for each of its record's lights, choosing them at its cluster's centre when
// nothing has yet; a warm-up trace one ray, to one light chosen by how much each could bring,
// moving that slot toward what the ray found.
void velk_points_trace_point(uint pool, uint i, uint frame)
{
    GlobalData g = VELK_GLOBALS;
    uint lights = max(min(pc.light_count, VELK_LIGHT_SLOTS), 1u);
    uint coarses = min(VELK_LIGHT_WORDS.data[pool + VELK_POOL_COARSE_POINTS], VELK_LIGHT_POINT_CAPACITY);
    uint firsts = min(VELK_LIGHT_WORDS.data[pool + VELK_POOL_FIRST_POINTS], VELK_LIGHT_POINT_CAPACITY);
    uint laters = min(VELK_LIGHT_WORDS.data[pool + VELK_POOL_LATER_POINTS], VELK_LIGHT_POINT_CAPACITY);
    uint budget = VELK_LIGHT_WORDS.data[pool + VELK_POOL_RAY_BUDGET];
    uint first_budget = budget > 0xFFFFFFFFu / VELK_LIGHT_FIRST_BUDGET_SCALE ? 0xFFFFFFFFu : budget * VELK_LIGHT_FIRST_BUDGET_SCALE;
    uint used = VELK_LIGHT_WORDS.data[pool + VELK_POOL_RAYS_LISTED] + coarses * lights;
    uint take_firsts = min(firsts, (first_budget - min(used, first_budget)) / lights);
    used += take_firsts * lights;
    // A later point traced from none after a change takes a ray a light, the others one; taken
    // in proportion, as they are mixed in the list.
    uint left = budget - min(used, budget);
    uint restarts = min(VELK_LIGHT_WORDS.data[pool + VELK_POOL_LATER_RESTARTS], laters);
    uint later_rays = laters + restarts * (lights - 1u);
    uint take_laters = later_rays <= left ? laters : uint(float(left) * float(laters) / float(later_rays));
    // Some every frame, so that light always moves on.
    if (coarses + take_firsts + take_laters == 0u) {
        take_firsts = min(firsts, 64u);
        take_laters = min(laters, 64u - take_firsts);
    }
    uint list;
    uint listed_points;
    uint place;
    if (i < coarses) {
        list = VELK_LIGHT_LIST_COARSE;
        listed_points = coarses;
        place = i;
    } else if (i < coarses + take_firsts) {
        list = VELK_LIGHT_LIST_FIRST;
        listed_points = firsts;
        place = i - coarses;
    } else if (i < coarses + take_firsts + take_laters) {
        list = VELK_LIGHT_LIST_LATER;
        listed_points = laters;
        place = i - coarses - take_firsts;
    } else {
        return;
    }
    place = (place + velk_points_hash(frame * 2654435761u + list) % listed_points) % listed_points;
    uint w = VELK_LIGHT_WORDS.data[pool + velk_light_list_word_word(list)] + place * VELK_LIGHT_POINT_WORDS;
    uint first = VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_FIRST];
    uint q = VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_POINT];
    uint instance = VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_INSTANCE];
    uint pages = VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES];
    // A piece's point: its page, and its record's first page and triangle from its owner word.
    uint piece = VELK_LIGHT_NONE;
    uint piece_triangle = 0u;
    if ((q & VELK_LIGHT_POINT_PIECE) != 0u) {
        piece = first;
        q &= ~VELK_LIGHT_POINT_PIECE;
        if (piece >= pages || q >= VELK_PIECE_POINTS) return;
        uint owner = VELK_LIGHT_WORDS.data[velk_light_page(pool, piece) + VELK_PIECE_OWNER];
        if ((owner & 0xFFFFFFu) >= pages) return;
        first = velk_light_page(pool, owner & 0xFFFFFFu);
        piece_triangle = owner >> 24u;
        if (piece_triangle >= velk_light_pieced(first)) return;
    }
    uint kind = VELK_LIGHT_WORDS.data[first + VELK_PAGE_KIND];
    if (kind == VELK_LIGHT_LINKED || (piece == VELK_LIGHT_NONE && q >= velk_light_own_points(first))) return;
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
    }

    // A vertex, or a piece's point in a triangle at weights of its corners.
    uint vertex_word = VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_VERTEX];
    uint first_index = VELK_LIGHT_WORDS.data[first + VELK_PAGE_INDEX] + VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_INDEX];
    vec3 local;
    vec3 normal;
    if (piece == VELK_LIGHT_NONE) {
        if (q >= vertices) return;
        uint vw = vertex_word + q * VELK_VERTEX3D_WORDS;
        local = vec3(uintBitsToFloat(velk_mesh_words.data[vw]), uintBitsToFloat(velk_mesh_words.data[vw + 1u]),
                     uintBitsToFloat(velk_mesh_words.data[vw + 2u]));
        normal = vec3(uintBitsToFloat(velk_mesh_words.data[vw + 3u]), uintBitsToFloat(velk_mesh_words.data[vw + 4u]),
                      uintBitsToFloat(velk_mesh_words.data[vw + 5u]));
    } else {
        uint t = piece_triangle;
        uint place = VELK_LIGHT_WORDS.data[velk_light_page(pool, piece) + VELK_PIECE_PLACE];
        uvec2 ab = velk_piece_place(q);
        if (ab.x + ab.y > velk_piece_level(place)) return;
        vec3 b = velk_piece_weights(place, ab);
        local = vec3(0.0);
        normal = vec3(0.0);
        for (uint k = 0u; k < 3u; ++k) {
            uint vw = vertex_word + velk_mesh_words.data[first_index + t * 3u + k] * VELK_VERTEX3D_WORDS;
            local += b[k] * vec3(uintBitsToFloat(velk_mesh_words.data[vw]), uintBitsToFloat(velk_mesh_words.data[vw + 1u]),
                                 uintBitsToFloat(velk_mesh_words.data[vw + 2u]));
            normal += b[k] * vec3(uintBitsToFloat(velk_mesh_words.data[vw + 3u]), uintBitsToFloat(velk_mesh_words.data[vw + 4u]),
                                  uintBitsToFloat(velk_mesh_words.data[vw + 5u]));
        }
    }
    vec3 p = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g);
    vec3 n = normalize(to_world * (normal / max(abs(inst.size.xyz), vec3(1e-6))));
    // Off the surface by a share of the light spacing where the point is, so that it does not
    // shadow itself and still sees what stands on it.
    mat4 vp = g.view_projection;
    float spacing = VELK_LIGHT_PIXELS / max(length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5, 1e-6);
    float lift = max(0.05 * spacing * length(p), length(p) * 1e-4);

    uint pw = piece == VELK_LIGHT_NONE ? velk_light_own_word(pool, first, q) : velk_light_page(pool, piece) + 2u * q;
    uint old = VELK_LIGHT_WORDS.data[pw];
    uint state = VELK_LIGHT_WORDS.data[pw + 1u];
    // Which trace this is, as its listing counted it.
    uint traces = state == VELK_LIGHT_NONE ? 0u : state >> 28u;
    uint seed = velk_points_hash((piece == VELK_LIGHT_NONE ? first : pw) * 977u + q) ^ (frame * 0x9e3779b9u);
    uint value = 0u;
    uint bw = piece == VELK_LIGHT_NONE ? velk_light_blocker_word(pool, first, q) : velk_light_page_blocker(pool, piece, 2u * q);
    uint blockers = 0u;
    if (traces == 0u || old == VELK_LIGHT_NONE) {
        // From none: a ray a light, the light it had before a change left out.
        bool restart = old != VELK_LIGHT_NONE;
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
            if (slots[s] == VELK_LIGHT_NONE) continue;
            if (!restart) ++g_first_rays;
            float blocked = velk_points_blocked(velk_light_at(pc.lights_base + slots[s]), p, n, lift, seed + s);
            value |= uint(blocked * 15.0 + 0.5) << (4u * s);
            if (blocked > 0.0) blockers |= velk_light_blocker_code(velk_ray_blocker_t) << (4u * s);
        }
        traces = 0u;
        atomicAdd(VELK_LIGHT_WORDS.data[pool + (restart ? VELK_POOL_SUM_RETRACES : VELK_POOL_SUM_FIRST_TRACES)], 1u);
    } else {
        // Warming up, a running mean of its rays.
        float step = 1.0 / float(traces + 1u);
        value = old;
        blockers = VELK_LIGHT_WORDS.data[bw];
        uint s = velk_points_pick(slots, p, seed);
        if (slots[s] != VELK_LIGHT_NONE) {
            float blocked = velk_points_blocked(velk_light_at(pc.lights_base + slots[s]), p, n, lift, seed + 7u);
            if (blocked > 0.0) {
                blockers = (blockers & ~(15u << (4u * s))) | (velk_light_blocker_code(velk_ray_blocker_t) << (4u * s));
            }
            float was = float((old >> (4u * s)) & 15u);
            // Rounded up as often as the fraction says, so that small steps still arrive and
            // the mean is not pulled toward the first rays.
            float dither = float(velk_points_hash(seed ^ 0x51ed270bu) & 0xFFFFu) / 65536.0;
            uint now = min(uint(mix(was, blocked * 15.0, step) + dither), 15u);
            value = (old & ~(15u << (4u * s))) | (now << (4u * s));
        }
        atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_LATER_TRACES], 1u);
    }
    // Never the word for untraced: all eight slots fully dark reads one step lighter.
    if (value == VELK_LIGHT_NONE) value = 0xEFFFFFFFu;
    VELK_LIGHT_WORDS.data[pw] = value;
    VELK_LIGHT_WORDS.data[bw] = blockers;
    uint listed = state == VELK_LIGHT_NONE ? frame & 0x0FFFFFFFu : state & 0x0FFFFFFFu;
    VELK_LIGHT_WORDS.data[pw + 1u] = (min(traces + 1u, 15u) << 28u) | listed;
}

)" R"(
// The threads of a work group of the trace.
#define VELK_LIGHT_TRACE_THREADS 64u
shared uint s_slots[VELK_LIGHT_SLOTS];
// A one value's rays, a thread each.
shared float s_blocked[VELK_LIGHT_TRACE_THREADS];

void main()
{
    uint pool = pc.spare3;
    uint index = gl_WorkGroupID.x;
    uint lane = gl_LocalInvocationIndex;
    // Past the trace list's groups, points from the point lists, a thread each.
    if (index >= VELK_LIGHT_TRACE_CAPACITY) {
        velk_points_trace_point(pool, (index - VELK_LIGHT_TRACE_CAPACITY) * VELK_LIGHT_TRACE_THREADS + lane,
                                VELK_GLOBALS.present_counter);
        velk_points_count(pool);
        return;
    }
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

    // One value: from its centre, a ray a thread, eight a light.
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
        vec3 local = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere]), uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere + 1u]),
                          uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere + 2u]));
        center = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g);
        radius = uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere + 3u]) * scale;
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
    uint seed = velk_points_hash(at) ^ (frame * 0x9e3779b9u);
    // Out of its own sphere toward each light, facing it. Traced once, and again only when
    // something changes it: a few rays a light, so that it is settled at once.
    uint s = lane / 8u;
    uint light = s_slots[s];
    float blocked = 0.0;
    if (light != VELK_LIGHT_NONE) {
        Light l = velk_light_at(pc.lights_base + light);
        vec3 toward = l.flags.x == 0u ? -l.direction.xyz : normalize(l.position.xyz - center);
        ++g_first_rays;
        blocked = velk_points_blocked(l, center, toward, radius * 1.01 + length(center) * 1e-4, seed + lane);
    }
    s_blocked[lane] = blocked;
    barrier();
    if (lane == 0u) {
        uint shades = 0u;
        for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
            float sum = 0.0;
            for (uint r = 0u; r < 8u; ++r) sum += s_blocked[k * 8u + r];
            shades |= uint(sum / 8.0 * 7.0 + 0.5) << (3u * k);
        }
        VELK_LIGHT_WORDS.data[word] = VELK_LIGHT_ONE_VALUE | VELK_LIGHT_ONE_TRACED | (old & (31u << VELK_LIGHT_ONE_QUEUED_SHIFT)) | shades;
    }
    velk_points_count(pool);
}
)";

/// What changed this frame, for light_changes_main_src: VELK_CHANGE_LIST, then a VELK_CHANGE a
/// change (cull_layout.h).
inline constexpr uint32_t kLightChangeHeaderWords = kChangeListChanges;
inline constexpr uint32_t kLightChangeWords = kChangeWords;
/// The most changes a frame lists; more count as everything changed.
inline constexpr uint32_t kLightChanges = 128;

/**
 * Makes the light of each kept cut record that a change this frame could alter due again, a
 * thread per record of the cut at pc.spare2 of the top tree at pc.spare1: own points stale, to
 * be traced from none again, coarse ones first, a one value traced again, a quad's on its node
 * and the nodes above it. A change of a light alters what uses that light; a box alters what it could shadow
 * or stop shadowing, a record whose way to one of its lights passes through the box. Light that
 * nothing changed is not traced again. Built on the deferred prelude with the light grid;
 * velk_light_cell_words is the culling arena, pc.spare3 the pool and pc.spare4 the change list.
 */
inline constexpr string_view light_changes_main_src = R"(
#define VELK_LIGHT_WORDS velk_light_cell_words
)" VELK_LIGHT_POOL_GLSL VELK_LIGHT_SLOTS_GLSL R"(
#define VELK_CHANGES_MAX 128u

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

// Whether a change this frame could alter the light of a sphere at `center` (relative to the
// camera) of `radius`, which uses the lights `slots`.
bool velk_change_alters(uint changes, vec3 center, float radius, uint slots[VELK_LIGHT_SLOTS])
{
    vec3 cam = VELK_GLOBALS.cam_pos.xyz;
    uint count = min(VELK_LIGHT_WORDS.data[changes + VELK_CHANGE_LIST_COUNT], VELK_CHANGES_MAX);
    for (uint k = 0u; k < count; ++k) {
        uint w = changes + VELK_CHANGE_LIST_CHANGES + k * VELK_CHANGE_WORDS;
        uint light = VELK_LIGHT_WORDS.data[w + VELK_CHANGE_LIGHT];
        vec3 lo = velk_change_vec3(w + VELK_CHANGE_MIN) - cam;
        vec3 hi = velk_change_vec3(w + VELK_CHANGE_MAX) - cam;
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
            if (slots[s] == VELK_LIGHT_NONE) continue;
            if (light != VELK_LIGHT_NONE) {
                if (slots[s] == light) return true;
                continue;
            }
            if (velk_change_in_way(center, radius, velk_light_at(pc.lights_base + slots[s]), lo, hi)) return true;
        }
    }
    return false;
}

// A one value at `word` traced again on its next listing.
void velk_change_one(uint word)
{
    uint value = VELK_LIGHT_WORDS.data[word];
    if (value >= VELK_LIGHT_ONE_VALUE && value < VELK_LIGHT_PENDING) atomicAnd(VELK_LIGHT_WORDS.data[word], ~VELK_LIGHT_ONE_TRACED);
}

void main()
{
    uint pool = pc.spare3;
    uint changes = pc.spare4;
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

    // Light of its own: points on pages, or a one value. Linked points are the light of the
    // records for light only they link to, which are records of the cut too.
    bool own_points = false;
    uint first = 0u;
    if (value < VELK_LIGHT_ONE_VALUE) {
        if (value >= VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES]) return;
        first = velk_light_page(pool, value);
        if (VELK_LIGHT_WORDS.data[first + VELK_PAGE_KIND] == VELK_LIGHT_LINKED) return;
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
                        ? VELK_LIGHT_WORDS.data[(entry & ~VELK_CUT_LIGHT_ONLY) + VELK_CLUSTER_ENTRY_TABLE] + cluster * VELK_LOD_CLUSTER_WORDS + VELK_LOD_CLUSTER_OWN_SPHERE
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
    if (!all && !velk_change_alters(changes, center, radius, slots)) return;
    atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_CHANGED], 1u);
    if (own_points) {
        // Its coarse light traced again at once, and its points and its pieces' stale
        // (velk_light_point_traces): pixels read the light under them, its coarse pieces' points
        // traced again this frame as pixels read them, the rest from none as the budget allows.
        // When everything changed, their lights chosen again too: what changed may be one of them.
        velk_change_one(first + VELK_PAGE_COARSE);
        uint now = VELK_GLOBALS.present_counter & 0x0FFFFFFFu;
        if (all) {
            VELK_LIGHT_WORDS.data[first + VELK_PAGE_RESET] = now;
            for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s] = VELK_LIGHT_NONE;
        } else {
            VELK_LIGHT_WORDS.data[first + VELK_PAGE_CHANGED] = now;
        }
    } else {
        velk_change_one(record + VELK_RECORD_LIGHT);
    }
}
)";

/// Bounce light from screen probes: a probe per kLightProbePixels square of the screen, at the
/// surface its middle pixel shows. kLightProbeWords each: the mean radiance arriving at it over
/// the hemisphere around its normal, weighted by cosine (3 floats), its position relative to the
/// camera (3 floats), its normal (snorm 16 bits each, octahedral) and how many frames it has
/// gathered (a float, 0 for no surface). Two stores, this frame's and the last's, swapping.
inline constexpr uint32_t kLightProbePixels = 16;
inline constexpr uint32_t kLightProbeWords = 8;
inline constexpr uint32_t kLightProbeRays = 16;

/**
 * Gathers each screen probe's bounce light, four probes a group, a ray a thread: rays over the
 * hemisphere around its surface's normal, turned each frame; a hit brings what the light points
 * there say reaches it and its indirect light (velk_probe_hit_indirect), times its albedo, and
 * what it gives off; a miss the environment. Blended
 * with the probe the last frame had where this one was, when that was on the same surface. A
 * hit's albedo and emission come from its draw's shading summary (velk_probe_surface), so it is
 * composed by compose_rt_trace, without the materials' code. pc.shapes_base is the light pool,
 * pc.env.z the light header (the grid), pc.env.w the last frame's probe store, pc.extras this
 * frame's probe store, the G-buffer's size (x low, y high), its world position and its normal.
 */
inline constexpr string_view light_probes_main_src = R"(
layout(set = 1, binding = 16, std430) buffer ProbeWords { uint data[]; } probe_words;
#define VELK_LIGHT_WORDS probe_words
#define velk_light_cell_words probe_words
#define CELLS_HEADER pc.env.z
)" VELK_LIGHT_POOL_GLSL R"(
#define VELK_PROBE_PIXELS 16u
#define VELK_PROBE_RAYS 16u
)" R"(
uint velk_light_grid_hash(ivec3 cube)
{
    uint h = (uint(cube.x) * 0x8da6b343u) ^ (uint(cube.y) * 0xd8163841u) ^ (uint(cube.z) * 0xcb1ab31fu);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
uint velk_light_grid_count(vec3 world_at, out uint first)
{
    first = 0u;
    uint grid = probe_words.data[CELLS_HEADER + 18u];
    if (grid == 0xFFFFFFFFu) return pc.light_count;
    uint buckets = probe_words.data[grid];
    float size = uintBitsToFloat(probe_words.data[grid + 1u]);
    uint everywhere = probe_words.data[grid + 2u];
    uint b = grid + 4u + everywhere + (velk_light_grid_hash(ivec3(floor(world_at / size))) & (buckets - 1u)) * 2u;
    uint count = probe_words.data[b + 1u];
    if (count != 0u) first = grid + probe_words.data[b];
    return everywhere + count;
}
uint velk_light_grid_at(uint first, uint k)
{
    uint grid = probe_words.data[CELLS_HEADER + 18u];
    if (grid == 0xFFFFFFFFu) return k;
    uint everywhere = probe_words.data[grid + 2u];
    return k < everywhere ? probe_words.data[grid + 4u + k] : probe_words.data[first + k - everywhere];
}
)" VELK_LIGHT_SLOTS_GLSL VELK_LIGHT_ADD_GLSL R"(
// Each draw record's shading summary (kDrawShadeWords, gpu_data.h), at the record's index.
layout(set = 1, binding = 6, std430) readonly buffer VelkDrawShade { uint data[]; } velk_draw_shade;
#define VELK_DRAW_SHADE_WORDS 2u
#define VELK_DRAW_SHADE_UNLIT 0x01000000u
#define VELK_DRAW_SHADE_TINTED 0x02000000u

vec3 velk_srgb_to_linear(vec3 s)
{
    return mix(s / 12.92, pow((s + 0.055) / 1.055, vec3(2.4)), greaterThan(s, vec3(0.04045)));
}

// An RGB9E5 colour (rgb9e5 in gpu_data.h).
vec3 velk_rgb9e5(uint w)
{
    return vec3(float(w & 511u), float((w >> 9u) & 511u), float((w >> 18u) & 511u)) * exp2(float(int(w >> 27u) - 24));
}

// What a ray sees of the surface it hit, without its material's code: its draw's shading
// summary, its albedo times, where its material takes them, its instance's colour and its vertex
// colours, and on a quad the quad's colour. A lit surface reflects that albedo; an unlit one
// gives it off, on top of its emission. White and lit where the hit names no draw.
void velk_probe_surface(RayHit hit, out vec3 albedo, out vec3 emission, out bool lit)
{
    albedo = vec3(1.0);
    emission = vec3(0.0);
    lit = true;
    if (hit.draw == 0xFFFFFFFFu || hit.instance == 0xFFFFFFFFu) return;
    uint at = hit.draw * VELK_DRAW_SHADE_WORDS;
    uint look = 0x00FFFFFFu | VELK_DRAW_SHADE_TINTED;
    uint glow = 0u;
    if (at + 1u < uint(velk_draw_shade.data.length())) {
        look = velk_draw_shade.data[at];
        glow = velk_draw_shade.data[at + 1u];
    }
    // The draw record only where the tint is taken: quads have no material, so they are tinted.
    vec4 tint = vec4(1.0);
    if ((look & VELK_DRAW_SHADE_TINTED) != 0u) {
        VelkDrawData d = velk_draw_data.data[hit.draw];
        if ((d.flags & VELK_DRAW_QUADS) != 0u) {
            // A quad's hit names the quad's word, whose colour is what the quad shows.
            tint = unpackUnorm4x8(velk_culling.data[hit.instance + VELK_QUAD_NODE_COLOR]);
        } else {
            tint = velk_instances.data[hit.instance].color;
            if ((d.flags & VELK_DRAW_COLOR) != 0u && hit.corners.x != VELK_NO_CORNERS) {
                vec3 b = vec3(1.0 - hit.weights.x - hit.weights.y, hit.weights.x, hit.weights.y);
                tint *= velk_read_color(velk_cluster_color_word(d, hit.cluster, hit.corners.x)) * b.x
                      + velk_read_color(velk_cluster_color_word(d, hit.cluster, hit.corners.y)) * b.y
                      + velk_read_color(velk_cluster_color_word(d, hit.cluster, hit.corners.z)) * b.z;
            }
        }
    }
    vec3 surface = velk_srgb_to_linear(unpackUnorm4x8(look).rgb) * tint.rgb;
    lit = (look & VELK_DRAW_SHADE_UNLIT) == 0u;
    albedo = lit ? surface : vec3(0.0);
    emission = velk_rgb9e5(glow) + (lit ? vec3(0.0) : surface);
}

)" R"(
shared vec3 s_light[64];

vec3 velk_probe_oct_decode(uint packed)
{
    vec2 e = unpackSnorm2x16(packed);
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

uint velk_probe_oct_encode(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return packSnorm2x16(e);
}

// What light `index` brings a surface at `at` facing `n`, unshadowed.
vec3 velk_probe_light(uint index, vec3 at, vec3 n)
{
    Light light = velk_light_at(pc.lights_base + index);
    vec3 l = -light.direction.xyz;
    float atten = 1.0;
    if (light.flags.x != 0u) {
        vec3 to = light.position.xyz - at;
        float dist = length(to);
        l = to / max(dist, 1e-6);
        float t = clamp(1.0 - dist / max(light.params.x, 1e-6), 0.0, 1.0);
        atten = t * t;
        if (light.flags.x == 2u) atten *= smoothstep(light.params.z, light.params.y, dot(-l, light.direction.xyz));
    }
    return light.color_intensity.rgb * light.color_intensity.a * atten * max(dot(n, l), 0.0);
}

// The direct light a surface at `at`, facing `n`, gets as its light points there say, for a hit:
// its one value, or its record's points at its triangle's corners (no triangle index on a hit:
// without the grid between them), each light by how much of it gets through.
vec3 velk_probe_direct(RayHit hit, vec3 at, vec3 n)
{
    uint pool = pc.shapes_base;
    if (hit.record == VELK_NO_RECORD || pool == 0xFFFFFFFFu) return vec3(0.0);
    uint entry = probe_words.data[hit.record];
    uint value = probe_words.data[hit.record + VELK_RECORD_LIGHT];
    if (entry == VELK_CUT_QUAD || value >= VELK_LIGHT_PENDING) return vec3(0.0);
    uint lights[VELK_LIGHT_SLOTS];
    vec2 sums[VELK_LIGHT_SLOTS];
    for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
        lights[k] = VELK_LIGHT_NONE;
        sums[k] = vec2(0.0);
    }
    if (value >= VELK_LIGHT_ONE_VALUE) {
        if ((value & VELK_LIGHT_ONE_TRACED) == 0u) return vec3(0.0);
        velk_light_add_one(value, at, lights, sums);
    } else {
        if (value >= probe_words.data[pool + VELK_POOL_PAGES] || hit.corners.x == VELK_NO_CORNERS) return vec3(0.0);
        vec3 b = vec3(1.0 - hit.weights.x - hit.weights.y, hit.weights.x, hit.weights.y);
        if (!velk_light_add_points(pool, velk_light_page(pool, value), 0xFFFFFFFFu, hit.corners, b, vec3(0.0), 0.0, 0.0, lights, sums)) {
            return vec3(0.0);
        }
    }
    vec3 total = vec3(0.0);
    for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
        if (lights[k] == VELK_LIGHT_NONE) continue;
        total += velk_probe_light(lights[k], at, n) * (sums[k].y / max(sums[k].x, 1e-6));
    }
    return total;
}

// The environment along `rd`, as a miss sees it. Black without one.
vec3 velk_probe_sky(vec3 rd)
{
    if (pc.env.y == 0u) return vec3(0.0);
    float c = cos(pc.env_params.y);
    float s = sin(pc.env_params.y);
    vec3 dir = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
    vec2 uv = vec2(atan(dir.z, dir.x) / 6.28318531 + 0.5, asin(clamp(dir.y, -1.0, 1.0)) / 3.14159265 + 0.5);
    float lod = float(textureQueryLevels(velk_textures[nonuniformEXT(pc.env.y)])) - 1.0;
    return textureLod(velk_textures[nonuniformEXT(pc.env.y)], uv, lod).rgb * pc.env_params.x;
}

// The indirect light a hit at `at` facing `n` gets, as the lighting pass would light it: the
// last frame's probe where the hit was on screen, when that probe was on the same surface, so
// that light bounces once more each frame; else the environment around its normal, unshadowed.
vec3 velk_probe_hit_indirect(vec3 at, vec3 n, uint width, uint height, uint across)
{
    GlobalData g = VELK_GLOBALS;
    uint was_store = pc.env.w;
    vec4 clip = g.prev_view_projection * vec4(at + g.cam_moved, 1.0);
    if (was_store != 0xFFFFFFFFu && clip.w > 1e-5) {
        vec2 p = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(width), float(height));
        if (all(greaterThanEqual(p, vec2(0.0))) && p.x < float(width) && p.y < float(height)) {
            uint before = was_store + ((uint(p.y) / VELK_PROBE_PIXELS) * across + uint(p.x) / VELK_PROBE_PIXELS) * VELK_PROBE_WORDS;
            if (uintBitsToFloat(probe_words.data[before + VELK_PROBE_FRAMES]) > 0.0) {
                vec3 then_pos = vec3(uintBitsToFloat(probe_words.data[before + VELK_PROBE_POSITION]),
                                     uintBitsToFloat(probe_words.data[before + VELK_PROBE_POSITION + 1u]),
                                     uintBitsToFloat(probe_words.data[before + VELK_PROBE_POSITION + 2u])) - g.cam_moved;
                vec3 then_n = velk_probe_oct_decode(probe_words.data[before + VELK_PROBE_NORMAL]);
                if (dot(then_n, n) > 0.9 && abs(dot(then_n, at - then_pos)) < 0.05 * length(at) + 1e-3) {
                    return vec3(uintBitsToFloat(probe_words.data[before + VELK_PROBE_LIGHT]),
                                uintBitsToFloat(probe_words.data[before + VELK_PROBE_LIGHT + 1u]),
                                uintBitsToFloat(probe_words.data[before + VELK_PROBE_LIGHT + 2u]));
                }
            }
        }
    }
    return velk_probe_sky(n);
}

void main()
{
    uint lane = gl_LocalInvocationIndex;
    uint width = pc.extras.y & 0xFFFFu;
    uint height = pc.extras.y >> 16u;
    uint across = (width + VELK_PROBE_PIXELS - 1u) / VELK_PROBE_PIXELS;
    uint down = (height + VELK_PROBE_PIXELS - 1u) / VELK_PROBE_PIXELS;
    uint probes = across * down;
    uint probe = gl_WorkGroupID.x * 4u + lane / VELK_PROBE_RAYS;
    uint ray = lane % VELK_PROBE_RAYS;
    bool live = probe < probes;
    uint now = pc.extras.x + probe * VELK_PROBE_WORDS;
    uint was_store = pc.env.w;
    GlobalData g = VELK_GLOBALS;

    // Its surface: the middle pixel of its square.
    ivec2 pixel = ivec2(int(min((probe % across) * VELK_PROBE_PIXELS + VELK_PROBE_PIXELS / 2u, width - 1u)),
                        int(min((probe / across) * VELK_PROBE_PIXELS + VELK_PROBE_PIXELS / 2u, height - 1u)));
    vec3 pos = live ? texelFetch(velk_textures[nonuniformEXT(pc.extras.z)], pixel, 0).xyz : vec3(0.0);
    vec3 n4 = live ? texelFetch(velk_textures[nonuniformEXT(pc.extras.w)], pixel, 0).xyz : vec3(0.0);
    bool surface = live && dot(n4, n4) > 1e-6;
    vec3 n = surface ? normalize(n4) : vec3(0.0, 1.0, 0.0);

    // A ray over the hemisphere, by cosine, turned each frame.
    vec3 light = vec3(0.0);
    if (surface) {
        float turn = fract(float(g.present_counter) * 0.61803399);
        float u = (float(ray) + fract(turn + float(probe) * 0.7548777)) / float(VELK_PROBE_RAYS);
        float v = fract(float(ray) * 0.61803399 + turn);
        float r = sqrt(u);
        float phi = 6.2831853 * v;
        vec3 a = normalize(abs(n.y) < 0.9 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
        vec3 b = cross(n, a);
        vec3 dir = normalize(a * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(1.0 - u, 0.0)));
        Ray rr;
        rr.origin = pos + n * max(length(pos) * 1e-3, 1e-3);
        rr.dir = dir;
        RayHit hit;
        if (trace_closest_hit(rr, 1e30, hit)) {
            vec3 at = rr.origin + dir * hit.t;
            vec3 hn = normalize(hit.normal);
            if (dot(hn, dir) > 0.0) hn = -hn;
            vec3 albedo;
            vec3 emission;
            bool lit;
            velk_probe_surface(hit, albedo, emission, lit);
            // As the lighting pass lights a pixel: albedo times each light's N.L and strength, and
            // its indirect light.
            vec3 indirect = lit ? velk_probe_hit_indirect(at, hn, width, height, across) : vec3(0.0);
            light = albedo * (velk_probe_direct(hit, at, hn) + indirect) + emission;
        } else {
            light = velk_probe_sky(dir);
        }
    }
    s_light[lane] = light;
    // The deepest the walk's stack got, for the log (light pool header 101).
    if (pc.shapes_base != 0xFFFFFFFFu && velk_ray_deepest > 0) atomicMax(probe_words.data[pc.shapes_base + VELK_POOL_DEEPEST_PROBES], uint(velk_ray_deepest));
    barrier();
    if (!live || ray != 0u) return;

    // Cosine sampled: the mean of what the rays found is the radiance the lighting pass takes for
    // indirect light, as it takes the environment's.
    vec3 sum = vec3(0.0);
    for (uint k = 0u; k < VELK_PROBE_RAYS; ++k) sum += s_light[lane + k];
    vec3 arriving = sum / float(VELK_PROBE_RAYS);
    if (!surface) {
        for (uint w = 0u; w < VELK_PROBE_WORDS; ++w) probe_words.data[now + w] = 0u;
        return;
    }
    // Where this probe was last frame, on the same surface: blended with it.
    float frames = 1.0;
    vec3 prev_rel = pos + g.cam_moved;
    vec4 clip = g.prev_view_projection * vec4(prev_rel, 1.0);
    if (clip.w > 1e-5) {
        vec2 at = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(width), float(height));
        if (all(greaterThanEqual(at, vec2(0.0))) && at.x < float(width) && at.y < float(height)) {
            uint before = was_store + ((uint(at.y) / VELK_PROBE_PIXELS) * across + uint(at.x) / VELK_PROBE_PIXELS) * VELK_PROBE_WORDS;
            float had = uintBitsToFloat(probe_words.data[before + VELK_PROBE_FRAMES]);
            vec3 then_pos = vec3(uintBitsToFloat(probe_words.data[before + VELK_PROBE_POSITION]),
                                 uintBitsToFloat(probe_words.data[before + VELK_PROBE_POSITION + 1u]),
                                 uintBitsToFloat(probe_words.data[before + VELK_PROBE_POSITION + 2u])) - g.cam_moved;
            vec3 then_n = velk_probe_oct_decode(probe_words.data[before + VELK_PROBE_NORMAL]);
            float off = abs(dot(then_n, pos - then_pos));
            if (had > 0.0 && dot(then_n, n) > 0.9 && off < 0.05 * length(pos) + 1e-3) {
                vec3 then_light = vec3(uintBitsToFloat(probe_words.data[before + VELK_PROBE_LIGHT]),
                                       uintBitsToFloat(probe_words.data[before + VELK_PROBE_LIGHT + 1u]),
                                       uintBitsToFloat(probe_words.data[before + VELK_PROBE_LIGHT + 2u]));
                frames = min(had + 1.0, 16.0);
                arriving = mix(then_light, arriving, 1.0 / frames);
            }
        }
    }
    probe_words.data[now + VELK_PROBE_LIGHT] = floatBitsToUint(arriving.r);
    probe_words.data[now + VELK_PROBE_LIGHT + 1u] = floatBitsToUint(arriving.g);
    probe_words.data[now + VELK_PROBE_LIGHT + 2u] = floatBitsToUint(arriving.b);
    probe_words.data[now + VELK_PROBE_POSITION] = floatBitsToUint(pos.x);
    probe_words.data[now + VELK_PROBE_POSITION + 1u] = floatBitsToUint(pos.y);
    probe_words.data[now + VELK_PROBE_POSITION + 2u] = floatBitsToUint(pos.z);
    probe_words.data[now + VELK_PROBE_NORMAL] = velk_probe_oct_encode(n);
    probe_words.data[now + VELK_PROBE_FRAMES] = floatBitsToUint(frames);
}
)";

/**
 * Gives each record a pixel shows its light, the first time one does, as kLightNone says: one
 * value, a quad's node, or a place on the search list for a cluster wide enough for points, which
 * waits as kLightPending until light_search_src lays it out. A record with one value that has
 * grown to twice the light spacing goes on the list too. In a record with pieces, the pixel's way
 * down its triangle's levels (velk_piece_way, velk_light_list_way): the piece it reaches is read
 * this frame, and where the level is too coarse and has nothing under it the pixel asks on the
 * split list for its children, or for the triangle's root under its vertex points. A thread per
 * pixel. `light_pixels` is kLightPixels.
 *
 * Run again after the search with `again` set when the search had records or asks for pieces: it
 * lists the points of the records and pieces just made, so that their coarse points are traced
 * this frame, and puts nothing on the search or split list, which the next frame's search would
 * not see.
 */
inline constexpr string_view light_claim_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

struct LightInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer LightInstances { LightInstance data[]; } light_instances;
layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint ids_word;
    uint width;
    uint height;
    uint pool_word;
    float light_pixels;
    uint lights;  // the scene's lights, at most VELK_LIGHT_SLOTS
    uint again;   // after the search, for the records it laid out
    uint position_id;  // the G-buffer's positions, with each triangle's longest edge
} pc;

#define VELK_LIGHT_WORDS light_words
)" VELK_LIGHT_POOL_GLSL VELK_LIGHT_LIST_GLSL R"(
// Puts `record`, now pending, on the search list with the light it had and its instance; with
// no room, gives its light back.
void velk_light_search(uint record, uint was, uint instance)
{
    uint at = atomicAdd(light_words.data[pc.pool_word + VELK_POOL_SEARCHED], 1u);
    if (at >= VELK_LIGHT_SEARCH_CAPACITY) {
        light_words.data[record + VELK_RECORD_LIGHT] = was;
        return;
    }
    uint list = light_words.data[pc.pool_word + VELK_POOL_SEARCH_WORD] + at * VELK_SEARCH_ENTRY_WORDS;
    light_words.data[list + VELK_SEARCH_ENTRY_RECORD] = record;
    light_words.data[list + VELK_SEARCH_ENTRY_WAS] = was;
    light_words.data[list + VELK_SEARCH_ENTRY_INSTANCE] = instance;
}

// Puts a first trace of `rays` rays on the trace list (kLightTraceWords); nothing when the list or
// the frame's budget of rays is full, and it is listed again on a later frame.
void velk_light_trace(uint kind, uint at, uint instance, uint table, uint rays)
{
    velk_light_list_entry(pc.pool_word, kind, at, instance, table, rays);
}

// Lists the coarse light of the own points whose first page is `page`, when it is due.
void velk_light_list_record(uint page, uint instance, uint frame)
{
    if (page >= light_words.data[pc.pool_word + VELK_POOL_PAGES]) return;
    uint first = velk_light_page(pc.pool_word, page);
    if (light_words.data[first + VELK_PAGE_KIND] == VELK_LIGHT_LINKED) return;
    light_words.data[first + VELK_PAGE_READ] = 1u;
    velk_light_list_coarse(pc.pool_word, page, instance, frame, pc.lights);
}

// Puts an ask for pieces on the split list: for the children of the piece at page `page`, or with
// VELK_LIGHT_SPLIT_ROOT in `how`, for the root of the triangle in its low 8 bits of the record
// whose first page is `page`, its grid's level in bits 16 to 18; down to the depth in its bits 8
// to 15. False when the list is full.
bool velk_light_split(uint page, uint how)
{
    uint at = atomicAdd(light_words.data[pc.pool_word + VELK_POOL_SPLITS], 1u);
    if (at >= VELK_LIGHT_SPLIT_CAPACITY) {
        atomicAdd(light_words.data[pc.pool_word + VELK_POOL_SUM_SPLITS_REFUSED], 1u);
        return false;
    }
    uint w = light_words.data[pc.pool_word + VELK_POOL_SPLIT_WORD] + at * VELK_SPLIT_ENTRY_WORDS;
    light_words.data[w + VELK_SPLIT_ENTRY_PAGE] = page;
    light_words.data[w + VELK_SPLIT_ENTRY_HOW] = how;
    return true;
}

// The deepest depth from `from` on whose pieces' points are at least VELK_LIGHT_COARSE_PIECES
// times `light_pixels` apart, in a triangle whose longest edge is `tri_px` pixels long; `from`
// itself when none deeper is.
uint velk_piece_coarse_depth(float tri_px, float light_pixels, uint from)
{
    float over = tri_px / (float(VELK_PIECE_LEVEL) * VELK_LIGHT_COARSE_PIECES * light_pixels);
    uint deepest = over >= 2.0 ? uint(min(floor(log2(over)), float(VELK_PIECE_DEEPEST))) : 0u;
    return max(from, deepest);
}

// Lists the points of light the pixel at weights `b` of triangle `triangle` (corners `corners`)
// of the record whose first page starts at `first` reads on its way down (velk_piece_way), when
// they are due: its coarse level's three around it as coarse, then those of the level it
// reaches, which is its coarse level where that level's cell is exact or not known yet
// (velk_way_cell); and the triangle's vertex points as coarse whatever the coarse level is, which rays
// that hit the triangle read. A piece it reaches is read this frame. Where the level it reaches
// is farther apart than the light spacing and has nothing under it, it asks for what comes next,
// the triangle's root under its vertex points, or a piece's children: always while the next
// level's points would still be VELK_LIGHT_COARSE_PIECES times the spacing apart, down to the
// deepest such level at once; below that, only where its points around the pixel are traced and
// differ, or are not all fully lit or fully blocked for every light: where an edge or a penumbra
// passes. Once a frame a piece; a root, once until it is made. `tri_px` is the triangle's
// longest edge on screen where the pixel is.
void velk_light_list_way(uint first, uvec3 corners, uint triangle, vec3 b, float tri_px, uint instance, uint frame)
{
    uint pool = pc.pool_word;
    uint pages = light_words.data[pool + VELK_POOL_PAGES];
    uint root_word = velk_light_root_word(pool, first, triangle);
    uint root = light_words.data[root_word];
    VelkPieceWay way = velk_piece_way(pool, root, b, tri_px, pc.light_pixels);
    // Where its coarse cell is exact, or not known yet, it goes no further down: judged by its
    // points' light even when a change made them stale, so that the finer points an edge needs
    // are traced while it moves; the lighting judges it again with what is traced.
    uint cell = velk_way_cell(pool, first, corners, way, false);
    if (cell < 2u) velk_way_stop_at_coarse(way);
    bool on_piece = way.piece != VELK_PIECE_VERTICES;
    uint piece_first = on_piece ? velk_light_page(pool, way.piece) : 0u;
    if (on_piece) {
        uint now = light_words.data[pool + VELK_POOL_FRAME] & VELK_PIECE_FRAME_BITS;
        uint read = light_words.data[piece_first + VELK_PIECE_READ];
        if ((read & VELK_PIECE_FRAME_BITS) != now) {
            atomicCompSwap(light_words.data[piece_first + VELK_PIECE_READ], read, now | (read & VELK_PIECE_SAME_BITS));
        }
    }

    // The vertex points, coarse: the coarse level's own when it is them.
    for (uint k = 0u; k < 3u; ++k) velk_light_list_point(pool, first, velk_way_vertex(first, corners[k]), instance, frame, true);
    uint words[3];
    vec3 w;
    if (way.coarse != VELK_PIECE_VERTICES) {
        velk_piece_around(pool, way.coarse, way.coarse_at, words, w);
        uint coarse_first = velk_light_page(pool, way.coarse);
        for (uint k = 0u; k < 3u; ++k) {
            velk_light_list_piece_point(pool, way.coarse, (words[k] - coarse_first) / 2u, first, instance, frame, true);
        }
    }
    // The level it reaches: listed when it is not the coarse level, whose points it then holds.
    velk_way_around(pool, first, corners, way.piece, way.at, words, w);
    if (way.piece != way.coarse) {
        for (uint k = 0u; k < 3u; ++k) {
            if (w[k] > 0.0) velk_light_list_piece_point(pool, way.piece, (words[k] - piece_first) / 2u, first, instance, frame, false);
        }
    }

    if (pc.again != 0u) return;
    if (way.spacing <= pc.light_pixels) return;
    // Nothing under it yet: a piece without children, or the vertex points without a root.
    if (on_piece && (way.depth >= VELK_PIECE_DEEPEST || light_words.data[piece_first + VELK_PIECE_CHILDREN] < pages)) return;
    if (!on_piece && (root != VELK_LIGHT_NONE || tri_px <= 2.0 * pc.light_pixels)) return;
    uint level = on_piece ? way.root_level : velk_piece_root_level(tri_px, pc.light_pixels);
    uint next = on_piece ? way.depth + 1u : 0u;
    bool edge = velk_piece_spacing(next, level, tri_px) < VELK_LIGHT_COARSE_PIECES * pc.light_pixels;
    uint depth = next;
    if (!edge) {
        uint last = on_piece ? way.depth + VELK_PIECE_LEVELS_AT_ONCE : VELK_PIECE_LEVELS_AT_ONCE;
        depth = min(velk_piece_coarse_depth(tri_px, pc.light_pixels, next), min(last, VELK_PIECE_DEEPEST));
    } else {
        uint s0 = light_words.data[words[0]];
        uint s1 = light_words.data[words[1]];
        uint s2 = light_words.data[words[2]];
        if (s0 == VELK_LIGHT_NONE || s1 == VELK_LIGHT_NONE || s2 == VELK_LIGHT_NONE) return;
        // Every light's four bits all clear or all set, the same at all three.
        if (s0 == s1 && s1 == s2 && s0 == (s0 & 0x11111111u) * 15u) return;
    }
    if (on_piece) {
        uint before = atomicOr(light_words.data[piece_first + VELK_PIECE_READ], VELK_PIECE_ASKED);
        if ((before & VELK_PIECE_ASKED) == 0u) velk_light_split(way.piece, depth << 8u);
    } else if (atomicCompSwap(light_words.data[root_word], VELK_LIGHT_NONE, VELK_LIGHT_PENDING) == VELK_LIGHT_NONE) {
        if (!velk_light_split(light_words.data[first + VELK_PAGE_FIRST], VELK_LIGHT_SPLIT_ROOT | triangle | (depth << 8u) | (level << 16u))) {
            light_words.data[root_word] = VELK_LIGHT_NONE;
        }
    }
}

)" R"(
// Lists the points of light the pixel at `coord` (ids `ids`, of draw `d`) reads from the record
// whose first page starts at `first`, those it reads with any weight, when they are due
// (velk_light_list_point). Where the pixel is in its triangle, and its triangle's size, are as
// the resolve left them for the lighting too (velk_pixel_weights, the G-buffer's positions), so
// that both read the same points.
void velk_light_list_pixel(uint first, VelkDrawData d, uvec2 ids, uvec2 coord, uint instance, uint frame, GlobalData g)
{
    uint cluster = velk_ids_cluster(d, ids);
    if (cluster == VELK_NO_CLUSTER) return;
    uint triangle = velk_ids_triangle(d, ids);
    uint word = d.index_base + velk_cluster_first_index(cluster) + triangle * 3u;
    uvec3 corners = uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
    vec3 b = velk_pixel_weights(pc.ids_word, pc.width, ivec2(coord));
    // In pieces: the pixel's way down its triangle's levels, from its vertex points.
    uint pool = pc.pool_word;
    if (triangle < velk_light_pieced(first)) {
        vec4 position = texelFetch(velk_textures[nonuniformEXT(pc.position_id)], ivec2(coord), 0);
        mat4 vp = g.view_projection;
        float px_per_world = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5 / max(length(position.xyz), 1e-4);
        float tri_px = position.w * px_per_world;
        velk_light_list_way(first, corners, triangle, b, tri_px, instance, frame);
        return;
    }
    // A point per vertex, its own or those it links to: a vertex's point is coarse.
    uint words[9];
    uint points[9];
    float weights[9];
    uint owners[9];
    uint read = velk_light_pixel_points(pool, first, corners, b, words, points, weights, owners);
    for (uint k = 0u; k < read; ++k) {
        if (weights[k] > 0.0) {
            velk_light_list_point(pc.pool_word, owners[k], points[k], instance, frame, points[k] < light_words.data[owners[k]]);
        }
    }
}

// Lists the one value at word `at`, once a frame, when it is due: the frame's low bits go into
// it, so another pixel finds it listed. Its trace takes eight rays a light, each time a first.
void velk_light_list_one(uint at, uint kind, uint target, uint instance, uint table, uint frame)
{
    uint value = light_words.data[at];
    if (value < VELK_LIGHT_ONE_VALUE || value >= VELK_LIGHT_PENDING || !velk_light_one_due(value)) return;
    uint listed = (value & ~(31u << VELK_LIGHT_ONE_QUEUED_SHIFT)) | ((frame & 31u) << VELK_LIGHT_ONE_QUEUED_SHIFT);
    if (listed == value || atomicCompSwap(light_words.data[at], value, listed) != value) return;
    velk_light_trace(kind, target, instance, table, 8u * max(pc.lights, 1u));
}

void main()
{
    uvec2 coord = gl_GlobalInvocationID.xy;
    if (coord.x >= pc.width || coord.y >= pc.height) return;
    // Again after the search only when it laid records out or made pieces this frame.
    if (pc.again != 0u && light_words.data[pc.pool_word + VELK_POOL_SEARCHED] == 0u
        && light_words.data[pc.pool_word + VELK_POOL_SPLITS] == 0u) {
        return;
    }
    uvec2 ids = velk_pixel_ids(pc.ids_word, pc.width, ivec2(coord));
    if (ids.x == 0u) return;
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint record = velk_ids_record(d, ids);
    if (record == VELK_NO_RECORD) return;
    GlobalData g = velk_globals.data[d.globals_base];
    mat4 vp = g.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
    uint entry = light_words.data[record + VELK_RECORD_ENTRY];
    uint frame = g.present_counter;

    // A quad: one value on the node over it as wide as the light spacing.
    if (entry == VELK_CUT_QUAD) {
        uint node = velk_light_quad_node(light_words.data[record + VELK_RECORD_QUAD_TREE], light_words.data[record + VELK_RECORD_QUAD_NODE], g.cam_pos.xyz,
                                         pixels_per_unit, pc.light_pixels);
        if (light_words.data[node + VELK_QUAD_NODE_LIGHT] == VELK_LIGHT_NONE) {
            atomicCompSwap(light_words.data[node + VELK_QUAD_NODE_LIGHT], VELK_LIGHT_NONE, VELK_LIGHT_ONE_VALUE);
        }
        velk_light_list_one(node + VELK_QUAD_NODE_LIGHT, VELK_LIGHT_TRACE_NODE, node, 0u, 0u, frame);
        return;
    }

    uint was = light_words.data[record + VELK_RECORD_LIGHT];
    if (was == VELK_LIGHT_PENDING) return;
    uint instance = d.instances_base + velk_ids_slot(d, ids);
    // A whole mesh or a shape drawn by its own pass: one value, traced from the mesh's sphere.
    if (entry == 0xFFFFFFFFu) {
        if (was == VELK_LIGHT_NONE) {
            atomicCompSwap(light_words.data[record + VELK_RECORD_LIGHT], VELK_LIGHT_NONE, VELK_LIGHT_ONE_VALUE);
        } else if (light_words.data[record + VELK_RECORD_MESH_TABLE] != 0xFFFFFFFFu) {
            velk_light_list_one(record + VELK_RECORD_LIGHT, VELK_LIGHT_TRACE_MESH, record, instance,
                                light_words.data[record + VELK_RECORD_MESH_TABLE], frame);
        }
        return;
    }
    uint clusters = light_words.data[entry + VELK_CLUSTER_ENTRY_TABLE];
    // A cluster: as wide on screen as it is now, and its largest triangle.
    uint cluster = clusters + light_words.data[record + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS;
    uint vertices = light_words.data[cluster + VELK_LOD_CLUSTER_VERTICES];
    LightInstance li = light_instances.data[d.instances_base + velk_ids_slot(d, ids)];
    mat4 world = velk_sized_world(li.world_matrix, li.offset.xyz, li.size.xyz);
    float scale = max(max(length(world[0].xyz), length(world[1].xyz)), length(world[2].xyz));
    vec3 own = vec3(uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE]),
                    uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 1u]),
                    uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 2u]));
    float radius = uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale;
    float away = length(mat3(world) * own + (world[3].xyz - g.cam_pos.xyz));
    float across = 2.0 * radius * pixels_per_unit / max(away, 1e-3);

    // Pages are laid out once; pieces follow how near the camera is. The points this pixel reads
    // are traced when due, and the coarse light of the records they are in: its own, or those of
    // the records for light only it links to at this pixel's corners.
    if (was != VELK_LIGHT_NONE && was < VELK_LIGHT_ONE_VALUE) {
        if (was >= light_words.data[pc.pool_word + VELK_POOL_PAGES]) return;
        uint first = velk_light_page(pc.pool_word, was);
        if (light_words.data[first + VELK_PAGE_KIND] != VELK_LIGHT_LINKED) {
            velk_light_list_record(was, instance, frame);
            velk_light_list_pixel(first, d, ids, coord, instance, frame, g);
            return;
        }
        uint cluster = velk_ids_cluster(d, ids);
        if (cluster == VELK_NO_CLUSTER) return;
        uint word = d.index_base + velk_cluster_first_index(cluster) + velk_ids_triangle(d, ids) * 3u;
        for (uint k = 0u; k < 3u; ++k) {
            uint corner = velk_mesh_words.data[word + k];
            if (corner >= light_words.data[first + VELK_PAGE_VERTICES]) continue;
            velk_light_list_record(light_words.data[velk_light_point_word(pc.pool_word, first, corner * 3u)], instance, frame);
        }
        velk_light_list_pixel(first, d, ids, coord, instance, frame, g);
        return;
    }

    // After the search, nothing more goes on the search list: those wait for the next frame.
    if (was == VELK_LIGHT_NONE) {
        if (pc.again != 0u) return;
        if (atomicCompSwap(light_words.data[record + VELK_RECORD_LIGHT], VELK_LIGHT_NONE, VELK_LIGHT_PENDING) != VELK_LIGHT_NONE) return;
        if (vertices == 0u || across < pc.light_pixels) {
            light_words.data[record + VELK_RECORD_LIGHT] = VELK_LIGHT_ONE_VALUE;
            return;
        }
        velk_light_search(record, VELK_LIGHT_NONE, instance);
        return;
    }
    // One value, grown to twice the light spacing: points. Else traced from its cluster's sphere.
    if (pc.again != 0u || vertices == 0u || across < 2.0 * pc.light_pixels) {
        velk_light_list_one(record + VELK_RECORD_LIGHT, VELK_LIGHT_TRACE_CLUSTER, record, instance, cluster, frame);
        return;
    }
    if (atomicCompSwap(light_words.data[record + VELK_RECORD_LIGHT], was, VELK_LIGHT_PENDING) != was) return;
    velk_light_search(record, was, instance);
}
)";

/**
 * Lays out the records on the search list (light_claim_src), a work group each. The records of
 * the same instance and mesh entry for light only (kCutLightOnly) are found beside it in the
 * store, since an instance's records are written together. With none, the record is coarse
 * enough and gets a point per vertex, and with its draw's indices a root word per triangle, none
 * yet (kLightPieced, else kLightOwn). Else each of them that has no light yet gets its own points,
 * and each vertex of the record names the three points of the nearest of their triangles, with
 * its weights there (kLightLinked), a thread per vertex. A record that cannot be laid out this
 * frame (no room, or a record for light only another group is laying out) gets its light back
 * and asks again.
 *
 * Then, in kLightSplitCapacity more groups, makes the pieces the split list asks for, a work
 * group an ask: a triangle's root, or a piece's children, down to the depth asked, every level
 * whole. Their points start untraced, but for those they share with the level above, which take
 * its light: a root's corners from the vertices' points, a child's from its parent's. They are
 * linked in last, so that nothing reads them before they are written.
 */
inline constexpr string_view light_search_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct LightInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer LightInstances { LightInstance data[]; } light_instances;
layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint pool_word;
    uint globals_base;   // the view's, for sizes on screen
    float light_pixels;  // kLightPixels
    uint lights;         // the scene's lights, at most VELK_LIGHT_SLOTS
    uint top_word;       // the view's top tree for rays, whose cuts it lists; 0xFFFFFFFF for none
} pc;

#define VELK_LIGHT_WORDS light_words
)" VELK_LIGHT_POOL_GLSL VELK_LIGHT_LIST_GLSL R"(
// How far along a partition's records to look for an instance's records for light only, each way.
const uint kScan = 256u;

// Where `record` is in its partition, which the scan for records for light only stays within:
// its cut's record pool, the partition's page table in the current one, the record's place there
// and the partition's count. False when no cut of the view's top tree holds it.
bool velk_light_store_of(uint record, out uint pool, out uint table, out uint at, out uint count)
{
    pool = 0u;
    table = 0u;
    at = 0u;
    count = 0u;
    if (pc.top_word == 0xFFFFFFFFu) return false;
    uint cuts = light_words.data[pc.top_word + VELK_RAY_TOP_CUTS];
    for (uint c = 0u; c < cuts; ++c) {
        uint cut = light_words.data[pc.top_word + VELK_RAY_TOP_HEADER_WORDS + c * VELK_RAY_TOP_CUT_WORDS + VELK_RAY_TOP_CUT_CUT];
        uint start = light_words.data[cut + VELK_CUT_RECORD_POOL];
        if (record < start || record >= start + light_words.data[cut + VELK_CUT_CAPACITY] * VELK_RECORD_END) continue;
        uint word;
        uint state;
        if (!velk_cut_live_record(cut, (record - start) / VELK_RECORD_END, word, state, at)) return false;
        pool = start;
        uint parity = light_words.data[cut + VELK_CUT_TABLE_PARITY];
        table = light_words.data[cut + VELK_CUT_TABLES + parity] + light_words.data[state + VELK_PART_TABLE_FIRST];
        count = light_words.data[state + VELK_PART_COUNT];
        return true;
    }
    return false;
}
const uint kMaxLights = 32u;
// The most triangles a cluster has (kClusterMaxTriangles).
const uint kMaxTriangles = 256u;

shared bool s_linked;       // the record links to records for light only, a thread per vertex
shared bool s_own;          // or it has its own points, which all threads make untraced
shared uint s_page;         // then its first page
shared uint s_first;        // and that page's word
shared uint s_lights;
shared uint s_light_records[kMaxLights];
shared uint s_light_pages[kMaxLights];
shared uint s_index_base;
shared uint s_table;        // its triangles' root words, 0 for none
shared uint s_points;

// Takes `count` pages off the free stack; the kth is velk_light_taken(pool, base, k). False when
// there are not that many. The count of pages taken only ever moves forward, past a take that
// fits: a take that does not leaves it as it is, so that no two takes get the same pages. A
// swap that fails means another take moved it, so there are at most as many tries as takes.
bool velk_light_take(uint pool, uint count, out uint base)
{
    uint free_pages = light_words.data[pool + VELK_POOL_FREE_PAGES];
    uint taken = light_words.data[pool + VELK_POOL_PAGES_TAKEN];
    base = 0u;
    for (uint tries = 0u; tries < 4096u; ++tries) {
        if (taken + count > free_pages) return false;
        uint seen = atomicCompSwap(light_words.data[pool + VELK_POOL_PAGES_TAKEN], taken, taken + count);
        if (seen == taken) {
            base = taken;
            return true;
        }
        taken = seen;
    }
    return false;
}

uint velk_light_taken(uint pool, uint base, uint k)
{
    return light_words.data[light_words.data[pool + VELK_POOL_STACK_WORD] + light_words.data[pool + VELK_POOL_FREE_PAGES] - 1u - base - k];
}

// Pages for a record of `vertices` vertices of `cluster` (its word in its hierarchy's table), as
// `kind`, with `table` words after its header and `points` point words: its header written and
// its pages named. Its points are left to the caller. False, and the pages asked for, when there
// is no room.
bool velk_light_lay_out(uint vertices, uint cluster, uint kind, uint table, uint points, uint index_base, out uint first_page)
{
    uint pool = pc.pool_word;
    uint count = (velk_light_skip_for(table) + points + VELK_LIGHT_PAGE_WORDS - 1u) / VELK_LIGHT_PAGE_WORDS;
    first_page = VELK_LIGHT_NONE;
    if (count > VELK_LIGHT_DIRECT_PAGES) return false;
    uint base;
    if (!velk_light_take(pool, count, base)) {
        atomicAdd(light_words.data[pool + VELK_POOL_PAGES_WANTED], count);
        return false;
    }
    uint page0 = velk_light_taken(pool, base, 0u);
    uint first = velk_light_page(pool, page0);
    light_words.data[first + VELK_PAGE_VERTICES] = vertices;
    light_words.data[first + VELK_PAGE_KIND] = kind;
    light_words.data[first + VELK_PAGE_POINT_WORDS] = points;
    for (uint k = 1u; k < VELK_LIGHT_DIRECT_PAGES; ++k) {
        light_words.data[first + VELK_PAGE_PAGES - 1u + k] = k < count ? velk_light_taken(pool, base, k) : VELK_LIGHT_NONE;
    }
    light_words.data[first + VELK_PAGE_CHANGED] = VELK_LIGHT_NONE;
    light_words.data[first + VELK_PAGE_CLUSTER] = cluster;
    light_words.data[first + VELK_PAGE_RESET] = VELK_LIGHT_NONE;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) light_words.data[first + VELK_PAGE_SLOTS + s] = VELK_LIGHT_NONE;
    light_words.data[first + VELK_PAGE_INDEX] = index_base;
    light_words.data[first + VELK_PAGE_FIRST] = page0;
    light_words.data[first + VELK_PAGE_COARSE] = VELK_LIGHT_ONE_VALUE;
    light_words.data[first + VELK_PAGE_READ] = 0u;
    first_page = page0;
    return true;
}

// Puts the coarse light of the own points just laid out at `page` on the trace list, so that it
// is traced in the frame they are laid out; their points are listed as pixels read them.
void velk_light_trace_now(uint page, uint instance)
{
    uint frame = velk_globals.data[pc.globals_base].present_counter;
    velk_light_list_coarse(pc.pool_word, page, instance, frame, pc.lights);
}

)" R"(
// Making pieces (the split list), a work group an ask: the new pieces are the levels `s_from` to
// `s_to` under the top piece (level 0), every level whole, 4^r pieces at level r, taken off the
// free stack in that order from `s_base`. The top piece is new for a root (s_from 0), else it is
// the piece asked for, `s_top`.
shared uint s_split;        // 0 for nothing to make, 1 for a root, 2 for children
shared uint s_top;          // the top piece's page: the piece asked for, or the new root
shared uint s_top_place;
shared uint s_from;
shared uint s_to;
shared uint s_base;
shared uint s_record;       // the record's first page, and its first page's word
shared uint s_record_first;
shared uint s_triangle;
shared uint s_root_word;

// Pieces at levels from `from` up to, not with, `level`.
uint velk_split_before(uint from, uint level)
{
    uint n = 0u;
    for (uint r = from; r < level && r <= VELK_PIECE_LEVELS_AT_ONCE; ++r) n += 1u << (2u * r);
    return n;
}

// The page of piece `q` at level `level` of the pieces being made, or the top piece at level 0.
uint velk_split_page(uint level, uint q)
{
    if (level == 0u && s_from == 1u) return s_top;
    return velk_light_taken(pc.pool_word, s_base, velk_split_before(s_from, level) + q);
}

// The place of piece `q` at level `level` under the top piece: its digits in base 4, the
// highest first, the children it is in from the top down.
uint velk_split_place(uint level, uint q)
{
    uint place = s_top_place;
    for (uint r = level; r > 0u && r <= VELK_PIECE_LEVELS_AT_ONCE; --r) {
        place = velk_piece_child_place(place, (q >> (2u * (r - 1u))) & 3u);
    }
    return place;
}

// Writes piece `q` at level `level` of the pieces being made, its points untraced but for those
// its parent shares, or for a root its corners, which the vertices' points give.
void velk_split_write(uint level, uint q, uint frame)
{
    uint pool = pc.pool_word;
    uint page = velk_split_page(level, q);
    uint at = velk_light_page(pool, page);
    uint place = velk_split_place(level, q);
    for (uint k = 0u; k < 4u; ++k) {
        light_words.data[at + VELK_PIECE_CHILDREN + k] = level < s_to ? velk_split_page(level + 1u, 4u * q + k) : VELK_LIGHT_NONE;
    }
    light_words.data[at + VELK_PIECE_PLACE] = place;
    light_words.data[at + VELK_PIECE_OWNER] = s_record | (s_triangle << 24u);
    light_words.data[at + VELK_PIECE_PARENT] = level == 0u ? VELK_LIGHT_NONE : velk_split_page(level - 1u, q >> 2u);
    light_words.data[at + VELK_PIECE_READ] = frame;
    uint parent_at = level == 0u ? 0u : velk_light_page(pool, velk_split_page(level - 1u, q >> 2u));
    uint parent_place = level == 0u ? 0u : velk_split_place(level - 1u, q >> 2u);
    for (uint p = 0u; p < VELK_PIECE_POINTS; ++p) {
        uint shade = VELK_LIGHT_NONE;
        uint state = VELK_LIGHT_NONE;
        uint blockers = 0u;
        uvec2 ab = velk_piece_place(p);
        uint from_word = VELK_LIGHT_NONE;
        uint from_blocker = VELK_LIGHT_NONE;
        if (level == 0u && s_from == 0u) {
            // A root's corners are its triangle's vertices.
            uint n = velk_piece_level(place);
            uint corner = ab == uvec2(0u, 0u) ? 0u : ab == uvec2(n, 0u) ? 1u : ab == uvec2(0u, n) ? 2u : 3u;
            uint index = light_words.data[s_record_first + VELK_PAGE_INDEX];
            if (corner < 3u && index != VELK_LIGHT_NONE) {
                uint cluster = light_words.data[s_record_first + VELK_PAGE_CLUSTER];
                uint v = velk_mesh_words.data[index + light_words.data[cluster + VELK_LOD_CLUSTER_INDEX] + s_triangle * 3u + corner];
                if (v < light_words.data[s_record_first + VELK_PAGE_VERTICES]) {
                    from_word = velk_light_own_word(pool, s_record_first, v);
                    from_blocker = velk_light_blocker_word(pool, s_record_first, v);
                }
            }
        } else {
            // A point at an even step of this level is one of its parent's.
            ivec2 x = velk_piece_point_at(place, ab);
            if (((x.x | x.y) & 1) == 0) {
                uint shared_point = velk_piece_point_of(parent_place, x / 2);
                if (shared_point != VELK_LIGHT_NONE) {
                    from_word = parent_at + 2u * shared_point;
                    from_blocker = velk_light_page_blocker(pool, velk_split_page(level - 1u, q >> 2u), 2u * shared_point);
                }
            }
        }
        if (from_word != VELK_LIGHT_NONE) {
            shade = light_words.data[from_word];
            state = light_words.data[from_word + 1u];
            blockers = light_words.data[from_blocker];
        }
        light_words.data[at + 2u * p] = shade;
        light_words.data[at + 2u * p + 1u] = state;
        light_words.data[velk_light_page_blocker(pool, page, 2u * p)] = blockers;
    }
    // A piece now, for the pool's sweep.
    atomicOr(light_words.data[light_words.data[pool + VELK_POOL_PIECES_WORD] + (page >> 5u)], 1u << (page & 31u));
}

// Makes the pieces split list entry `index` asks for, all threads of the group together.
void velk_split(uint index)
{
    uint pool = pc.pool_word;
    uint lane = gl_LocalInvocationIndex;
    uint frame = light_words.data[pool + VELK_POOL_FRAME] & VELK_PIECE_FRAME_BITS;
    if (lane == 0u) {
        s_split = 0u;
        uint pages = light_words.data[pool + VELK_POOL_PAGES];
        uint w = light_words.data[pool + VELK_POOL_SPLIT_WORD] + index * VELK_SPLIT_ENTRY_WORDS;
        uint page = light_words.data[w + VELK_SPLIT_ENTRY_PAGE];
        uint how = light_words.data[w + VELK_SPLIT_ENTRY_HOW];
        uint depth = min((how >> 8u) & 255u, VELK_PIECE_DEEPEST);
        bool root = (how & VELK_LIGHT_SPLIT_ROOT) != 0u;
        if (page < pages) {
            if (root) {
                // The root word is pending while the ask is on the list: it is this group's.
                s_record = page;
                s_record_first = velk_light_page(pool, page);
                s_triangle = how & 255u;
                if (s_triangle < velk_light_pieced(s_record_first)) {
                    s_root_word = velk_light_root_word(pool, s_record_first, s_triangle);
                    if (light_words.data[s_root_word] == VELK_LIGHT_PENDING) {
                        s_split = 1u;
                        s_from = 0u;
                        s_to = min(depth, VELK_PIECE_LEVELS_AT_ONCE);
                        uint level = (how >> 16u) & 7u;
                        s_top_place = velk_piece_root_place(level == 0u ? VELK_PIECE_LEVEL : level);
                    }
                }
            } else if (velk_light_is_piece(pool, page)) {
                uint top = velk_light_page(pool, page);
                uint top_place = light_words.data[top + VELK_PIECE_PLACE];
                uint top_depth = velk_piece_depth(top_place);
                uint owner = light_words.data[top + VELK_PIECE_OWNER];
                if (light_words.data[top + VELK_PIECE_CHILDREN] >= pages && top_depth < VELK_PIECE_DEEPEST
                    && (owner & 0xFFFFFFu) < pages) {
                    s_split = 2u;
                    s_top = page;
                    s_top_place = top_place;
                    s_record = owner & 0xFFFFFFu;
                    s_record_first = velk_light_page(pool, s_record);
                    s_triangle = owner >> 24u;
                    s_from = 1u;
                    s_to = clamp(depth, top_depth + 1u, min(top_depth + VELK_PIECE_LEVELS_AT_ONCE, VELK_PIECE_DEEPEST)) - top_depth;
                }
            }
        }
        if (s_split != 0u) {
            uint count = velk_split_before(s_from, s_to + 1u);
            uint base;
            if (velk_light_take(pool, count, base)) {
                s_base = base;
                if (s_split == 1u) s_top = velk_light_taken(pool, base, 0u);
                atomicAdd(light_words.data[pool + VELK_POOL_SUM_PIECES_MADE], count);
            } else {
                atomicAdd(light_words.data[pool + VELK_POOL_PAGES_WANTED], count);
                atomicAdd(light_words.data[pool + VELK_POOL_SUM_SPLITS_REFUSED], 1u);
                if (s_split == 1u) light_words.data[s_root_word] = VELK_LIGHT_NONE;
                s_split = 0u;
            }
        } else if (root && page < pages) {
            // Not this group's to make after all: asked again when read.
            uint first = velk_light_page(pool, page);
            if ((how & 255u) < velk_light_pieced(first)) {
                uint word = velk_light_root_word(pool, first, how & 255u);
                if (light_words.data[word] == VELK_LIGHT_PENDING) light_words.data[word] = VELK_LIGHT_NONE;
            }
        }
    }
    memoryBarrierShared();
    barrier();
    if (s_split == 0u) return;
    // Level by level, so that each level's shared points come from the one above, written.
    for (uint level = s_from; level <= s_to && level <= VELK_PIECE_LEVELS_AT_ONCE; ++level) {
        uint pieces = 1u << (2u * level);
        for (uint q = lane; q < pieces; q += 64u) velk_split_write(level, q, frame);
        memoryBarrierBuffer();
        barrier();
    }
    // Then linked in, for the passes after this one.
    if (lane == 0u) {
        if (s_split == 1u) {
            light_words.data[s_root_word] = s_top;
        } else {
            uint top = velk_light_page(pool, s_top);
            for (uint k = 0u; k < 4u; ++k) light_words.data[top + VELK_PIECE_CHILDREN + k] = velk_split_page(1u, k);
        }
    }
}

)" R"(
vec3 velk_light_position(uint word)
{
    return vec3(uintBitsToFloat(velk_mesh_words.data[word]), uintBitsToFloat(velk_mesh_words.data[word + 1u]),
                uintBitsToFloat(velk_mesh_words.data[word + 2u]));
}

// The weights of the point of triangle a, b, c nearest to p.
vec3 velk_light_nearest(vec3 p, vec3 a, vec3 b, vec3 c)
{
    vec3 ab = b - a;
    vec3 ac = c - a;
    vec3 ap = p - a;
    float d1 = dot(ab, ap);
    float d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) return vec3(1.0, 0.0, 0.0);
    vec3 bp = p - b;
    float d3 = dot(ab, bp);
    float d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) return vec3(0.0, 1.0, 0.0);
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        float v = d1 / max(d1 - d3, 1e-30);
        return vec3(1.0 - v, v, 0.0);
    }
    vec3 cp = p - c;
    float d5 = dot(ab, cp);
    float d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) return vec3(0.0, 0.0, 1.0);
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        float w = d2 / max(d2 - d6, 1e-30);
        return vec3(1.0 - w, 0.0, w);
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0) {
        float w = (d4 - d3) / max((d4 - d3) + (d5 - d6), 1e-30);
        return vec3(0.0, 1.0 - w, w);
    }
    float denom = 1.0 / max(va + vb + vc, 1e-30);
    float v = vb * denom;
    float w = vc * denom;
    return vec3(1.0 - v - w, v, w);
}

void main()
{
    uint pool = pc.pool_word;
    uint index = gl_WorkGroupID.x;
    uint lane = gl_LocalInvocationIndex;
    // Past the search list's groups, the split list's.
    if (index >= VELK_LIGHT_SEARCH_CAPACITY) {
        uint split = index - VELK_LIGHT_SEARCH_CAPACITY;
        if (split < min(light_words.data[pool + VELK_POOL_SPLITS], VELK_LIGHT_SPLIT_CAPACITY)) velk_split(split);
        return;
    }
    if (index >= light_words.data[pool + VELK_POOL_SEARCHED]) return;
    uint list = light_words.data[pool + VELK_POOL_SEARCH_WORD] + index * VELK_SEARCH_ENTRY_WORDS;
    uint record = light_words.data[list + VELK_SEARCH_ENTRY_RECORD];
    uint was = light_words.data[list + VELK_SEARCH_ENTRY_WAS];
    uint instance = light_words.data[list + VELK_SEARCH_ENTRY_INSTANCE];
    uint entry = light_words.data[record + VELK_RECORD_ENTRY];
    uint slot = light_words.data[record + VELK_RECORD_SLOT];
    uint clusters = light_words.data[entry + VELK_CLUSTER_ENTRY_TABLE];
    uint cluster = clusters + light_words.data[record + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS;
    uint vertices = light_words.data[cluster + VELK_LOD_CLUSTER_VERTICES];
    GlobalData g = velk_globals.data[pc.globals_base];
    LightInstance inst = light_instances.data[instance];

    if (lane == 0u) {
        s_linked = false;
        s_own = false;
        s_lights = 0u;
        bool again = false;
        uint pages = light_words.data[pool + VELK_POOL_PAGES];
        // The instance's records for light only, beside it: an instance's records are together,
        // in order within its partition's records.
        uint record_pool;
        uint part_table;
        uint part_at;
        uint part_count;
        bool in_store = velk_light_store_of(record, record_pool, part_table, part_at, part_count);
        for (int dir = -1; in_store && dir <= 1; dir += 2) {
            // The page the scan is in, read from the table only when it steps onto another.
            uint table_page = 0xFFFFFFFFu;
            uint page_place = 0u;
            for (uint k = 1u; k <= kScan; ++k) {
                if (dir < 0 && part_at < k) break;
                if (dir > 0 && part_at + k >= part_count) break;
                uint near_at = dir < 0 ? part_at - k : part_at + k;
                if (near_at / VELK_CUT_PAGE_RECORDS != table_page) {
                    table_page = near_at / VELK_CUT_PAGE_RECORDS;
                    page_place = light_words.data[part_table + table_page] * VELK_CUT_PAGE_RECORDS;
                }
                uint near_place = page_place + near_at % VELK_CUT_PAGE_RECORDS;
                uint other = record_pool + near_place * VELK_RECORD_END;
                if (light_words.data[other + VELK_RECORD_SLOT] != slot) break;
                if (light_words.data[other + VELK_RECORD_ENTRY] != (entry | VELK_CUT_LIGHT_ONLY)) continue;
                uint light = light_words.data[other + VELK_RECORD_LIGHT];
                if (light == VELK_LIGHT_NONE) {
                    if (atomicCompSwap(light_words.data[other + VELK_RECORD_LIGHT], VELK_LIGHT_NONE, VELK_LIGHT_PENDING)
                        != VELK_LIGHT_NONE) {
                        again = true;
                        continue;
                    }
                    uint other_cluster = clusters + light_words.data[other + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS;
                    uint other_vertices = light_words.data[other_cluster + VELK_LOD_CLUSTER_VERTICES];
                    uint laid;
                    if (!velk_light_lay_out(other_vertices, other_cluster, VELK_LIGHT_OWN, 0u, 2u * other_vertices,
                                            VELK_LIGHT_NONE, laid)) {
                        light_words.data[other + VELK_RECORD_LIGHT] = VELK_LIGHT_NONE;
                        again = true;
                        continue;
                    }
                    uint other_first = velk_light_page(pool, laid);
                    for (uint p = 0u; p < other_vertices; ++p) {
                        uint at = velk_light_own_word(pool, other_first, p);
                        light_words.data[at] = VELK_LIGHT_NONE;
                        light_words.data[at + 1u] = VELK_LIGHT_NONE;
                        light_words.data[velk_light_blocker_word(pool, other_first, p)] = 0u;
                    }
                    atomicAdd(light_words.data[pool + VELK_POOL_SUM_LAID_LIGHT_ONLY], other_vertices);
                    light_words.data[other + VELK_RECORD_LIGHT] = laid;
                    velk_light_trace_now(laid, instance);
                    light = laid;
                }
                if (light == VELK_LIGHT_PENDING) {
                    again = true;
                } else if (light < pages && s_lights < kMaxLights) {
                    s_light_records[s_lights] = other;
                    s_light_pages[s_lights] = light;
                    ++s_lights;
                }
            }
        }
        uint draw = light_words.data[entry + VELK_CLUSTER_ENTRY_DRAW];
        if (again || (s_lights != 0u && draw == 0xFFFFFFFFu)) {
            light_words.data[record + VELK_RECORD_LIGHT] = was;
        } else if (s_lights == 0u) {
            // Coarse enough: its own points, a point a vertex, and with its draw's indices to find
            // its triangles' corners, a root word a triangle, its pieces made as pixels read them.
            // All threads make them untraced and without roots, below.
            uint index_base = draw == 0xFFFFFFFFu ? VELK_LIGHT_NONE : velk_draw_data.data[draw].index_base;
            uint triangles = min(light_words.data[cluster + VELK_LOD_CLUSTER_TRIANGLES], kMaxTriangles);
            uint table = index_base != VELK_LIGHT_NONE ? triangles : 0u;
            uint kind = table != 0u ? VELK_LIGHT_PIECED | triangles : VELK_LIGHT_OWN;
            uint laid;
            if (velk_light_lay_out(vertices, cluster, kind, table, 2u * vertices, index_base, laid)) {
                s_page = laid;
                s_first = velk_light_page(pool, laid);
                s_table = table;
                s_points = vertices;
                s_own = true;
            } else {
                light_words.data[record + VELK_RECORD_LIGHT] = was;
            }
        } else {
            uint laid;
            if (velk_light_lay_out(vertices, cluster, VELK_LIGHT_LINKED, 0u, vertices * 3u, VELK_LIGHT_NONE, laid)) {
                s_page = laid;
                s_first = velk_light_page(pool, laid);
                s_index_base = velk_draw_data.data[draw].index_base;
                s_linked = true;
            } else {
                light_words.data[record + VELK_RECORD_LIGHT] = was;
            }
        }
    }
    memoryBarrierBuffer();
    barrier();
    // Its own points: its triangles without roots and the points untraced, then the record names
    // them and they are traced this frame.
    if (s_own) {
        for (uint t = lane; t < s_table; t += 64u) {
            light_words.data[velk_light_root_word(pool, s_first, t)] = VELK_LIGHT_NONE;
        }
        for (uint p = lane; p < s_points; p += 64u) {
            uint at = velk_light_own_word(pool, s_first, p);
            light_words.data[at] = VELK_LIGHT_NONE;
            light_words.data[at + 1u] = VELK_LIGHT_NONE;
            light_words.data[velk_light_blocker_word(pool, s_first, p)] = 0u;
        }
        memoryBarrierBuffer();
        barrier();
        if (lane == 0u) {
            light_words.data[record + VELK_RECORD_LIGHT] = s_page;
            velk_light_trace_now(s_page, instance);
            // For the log, why: no light before, or one value before.
            uint why = was == VELK_LIGHT_NONE ? VELK_POOL_SUM_LAID_NEW : VELK_POOL_SUM_LAID_FROM_ONE;
            atomicAdd(light_words.data[pool + why], s_points);
        }
        return;
    }
    if (!s_linked) return;

    // Each vertex: the nearest point on the records for light only, all in the mesh's own space.
    uint vertex_word = light_words.data[cluster + VELK_LOD_CLUSTER_VERTEX];
    for (uint v = lane; v < vertices; v += 64u) {
        vec3 p = velk_light_position(vertex_word + v * VELK_VERTEX3D_WORDS);
        float best = 1e30;
        uint best_page = VELK_LIGHT_NONE;
        uint best_points = 0u;
        vec3 best_weights = vec3(1.0, 0.0, 0.0);
        for (uint i = 0u; i < s_lights; ++i) {
            uint other = clusters + light_words.data[s_light_records[i] + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS;
            vec3 center = vec3(uintBitsToFloat(light_words.data[other + VELK_LOD_CLUSTER_OWN_SPHERE]),
                               uintBitsToFloat(light_words.data[other + VELK_LOD_CLUSTER_OWN_SPHERE + 1u]),
                               uintBitsToFloat(light_words.data[other + VELK_LOD_CLUSTER_OWN_SPHERE + 2u]));
            if (length(p - center) - uintBitsToFloat(light_words.data[other + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) >= best) continue;
            uint triangles = light_words.data[other + VELK_LOD_CLUSTER_TRIANGLES];
            uint first_index = s_index_base + light_words.data[other + VELK_LOD_CLUSTER_INDEX];
            uint other_vertices = light_words.data[other + VELK_LOD_CLUSTER_VERTEX];
            for (uint t = 0u; t < triangles; ++t) {
                uint w = first_index + t * 3u;
                uvec3 k = uvec3(velk_mesh_words.data[w], velk_mesh_words.data[w + 1u], velk_mesh_words.data[w + 2u]);
                vec3 a = velk_light_position(other_vertices + k.x * VELK_VERTEX3D_WORDS);
                vec3 b = velk_light_position(other_vertices + k.y * VELK_VERTEX3D_WORDS);
                vec3 c = velk_light_position(other_vertices + k.z * VELK_VERTEX3D_WORDS);
                vec3 weights = velk_light_nearest(p, a, b, c);
                float d = length(p - (a * weights.x + b * weights.y + c * weights.z));
                if (d < best) {
                    best = d;
                    best_page = s_light_pages[i];
                    best_points = (k.x & 255u) | ((k.y & 255u) << 8u) | ((k.z & 255u) << 16u);
                    best_weights = weights;
                }
            }
        }
        light_words.data[velk_light_point_word(pool, s_first, v * 3u)] = best_page;
        light_words.data[velk_light_point_word(pool, s_first, v * 3u + 1u)] = best_points;
        light_words.data[velk_light_point_word(pool, s_first, v * 3u + 2u)] = packUnorm2x16(best_weights.yz);
    }
    memoryBarrierBuffer();
    barrier();
    if (lane == 0u) light_words.data[record + VELK_RECORD_LIGHT] = s_page;
}
)";

/**
 * Marks the pages the records of a kept cut name, and those of the records for light only their
 * vertices link to, and counts its records with pages and with one value. A thread per record
 * of the cut's pool, a group a page up to the last in use (VELK_CUT_DRAW_GROUPS).
 */
inline constexpr string_view light_mark_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint cut_word;
    uint pool_word;
} pc;

#define VELK_LIGHT_WORDS light_words
)" VELK_LIGHT_POOL_GLSL R"(
// Never past the pool's pages, whatever a record says.
void velk_light_mark(uint page)
{
    if (page >= light_words.data[pc.pool_word + VELK_POOL_PAGES]) return;
    atomicOr(light_words.data[light_words.data[pc.pool_word + VELK_POOL_MARKS_WORD] + (page >> 5u)], 1u << (page & 31u));
}

// A record's first page and its others. Its pieces are marked apart (light_pieces_src).
void velk_light_mark_record(uint light)
{
    velk_light_mark(light);
    uint first = velk_light_page(pc.pool_word, light);
    for (uint k = 1u; k < VELK_LIGHT_DIRECT_PAGES; ++k) velk_light_mark(light_words.data[first + VELK_PAGE_PAGES - 1u + k]);
}

void main()
{
    uint cut = pc.cut_word;
    // A record of the pool a thread, only those a partition holds.
    uint record;
    uint state;
    uint at;
    if (!velk_cut_live_record(cut, gl_GlobalInvocationID.x, record, state, at)) return;
    uint light = light_words.data[record + VELK_RECORD_LIGHT];
    if (light == VELK_LIGHT_NONE || light == VELK_LIGHT_PENDING) return;
    if (light >= VELK_LIGHT_ONE_VALUE) {
        atomicAdd(light_words.data[pc.pool_word + VELK_POOL_RECORDS_ONE_VALUE], 1u);
        return;
    }
    uint pages = light_words.data[pc.pool_word + VELK_POOL_PAGES];
    if (light >= pages) return;
    atomicAdd(light_words.data[pc.pool_word + VELK_POOL_RECORDS_PAGED], 1u);
    velk_light_mark_record(light);
    uint first = velk_light_page(pc.pool_word, light);
    if (light_words.data[first + VELK_PAGE_KIND] != VELK_LIGHT_LINKED) {
        light_words.data[first + VELK_PAGE_READ] = 0u;
        return;
    }
    atomicAdd(light_words.data[pc.pool_word + VELK_POOL_RECORDS_LINKED], 1u);
    // What its vertices link to, which neighbouring vertices mostly share.
    uint last = VELK_LIGHT_NONE;
    uint vertices = min(light_words.data[first + VELK_PAGE_VERTICES], 256u);
    for (uint v = 0u; v < vertices; ++v) {
        uint linked = light_words.data[velk_light_point_word(pc.pool_word, first, v * 3u)];
        if (linked != last && linked < pages) velk_light_mark_record(linked);
        last = linked;
    }
}
)";

/**
 * Keeps the pieces in use, after the records' mark (light_mark_src) and before the sweep, a thread
 * per page of the pool, twice. First (`phase` 0) it unlinks what no pixel read for
 * kLightPieceKeepFrames: a piece's four children, when all are without children of their own and
 * none was read, and a root without children likewise, from its record; and a piece's four
 * children without children of their own whose light has been the same as the piece's for
 * kLightPieceSameFrames and one more (velk_pieces_same), so that pixels read the piece again and
 * ask for children again where its light differs. Then (`phase` 1) it marks
 * every piece still linked, from its parent or its record, whose record is marked; the sweep frees
 * the rest. Only childless pieces are unlinked, so a piece's parent is in use while it is.
 */
inline constexpr string_view light_pieces_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint pool_word;
    uint phase;
} pc;

#define VELK_LIGHT_WORDS light_words
)" VELK_LIGHT_POOL_GLSL R"(
bool velk_pieces_marked(uint pool, uint page)
{
    return (light_words.data[light_words.data[pool + VELK_POOL_MARKS_WORD] + (page >> 5u)] & (1u << (page & 31u))) != 0u;
}

// Whether the piece starting at word `at`, which has no children, has not been read for
// VELK_PIECE_KEEP_FRAMES, the pool's frame being `now`.
bool velk_pieces_unread(uint at, uint now)
{
    uint read = light_words.data[at + VELK_PIECE_READ] & VELK_PIECE_FRAME_BITS;
    return ((now - read) & VELK_PIECE_FRAME_BITS) > VELK_PIECE_KEEP_FRAMES;
}

// Whether the piece starting at word `at` and its four children all carry the same light at
// every point of their grids, each light fully through or fully blocked, as their points'
// stored light says whether stale or not; a point not traced yet says no.
bool velk_pieces_same(uint pool, uint at, uint pages)
{
    uint shade = light_words.data[at];
    if (shade == VELK_LIGHT_NONE || shade != (shade & 0x11111111u) * 15u) return false;
    uint level = velk_piece_level(light_words.data[at + VELK_PIECE_PLACE]);
    for (uint q = 1u; q < VELK_PIECE_POINTS; ++q) {
        uvec2 ab = velk_piece_place(q);
        if (ab.x + ab.y <= level && light_words.data[at + 2u * q] != shade) return false;
    }
    for (uint k = 0u; k < 4u; ++k) {
        uint child = light_words.data[at + VELK_PIECE_CHILDREN + k];
        if (child >= pages) return false;
        uint child_at = velk_light_page(pool, child);
        for (uint q = 0u; q < VELK_PIECE_POINTS; ++q) {
            if (light_words.data[child_at + 2u * q] != shade) return false;
        }
    }
    return true;
}

void main()
{
    uint pool = pc.pool_word;
    uint page = gl_GlobalInvocationID.x;
    uint pages = light_words.data[pool + VELK_POOL_PAGES];
    if (page >= pages || !velk_light_is_piece(pool, page)) return;
    uint at = velk_light_page(pool, page);
    uint owner = light_words.data[at + VELK_PIECE_OWNER];
    uint record = owner & 0xFFFFFFu;
    uint parent = light_words.data[at + VELK_PIECE_PARENT];
    uint now = light_words.data[pool + VELK_POOL_FRAME] & VELK_PIECE_FRAME_BITS;
    bool live = record < pages && velk_pieces_marked(pool, record);
    uint first = live ? velk_light_page(pool, record) : 0u;
    uint triangle = owner >> 24u;
    if (live && triangle >= velk_light_pieced(first)) live = false;

    if (pc.phase == 0u) {
        if (!live) return;
        uint child0 = light_words.data[at + VELK_PIECE_CHILDREN];
        if (child0 < pages) {
            // Its four children, when none has children and none was read, or when none has
            // children and their light has been the same as its own for long enough.
            bool unread = true;
            bool leaves = true;
            for (uint k = 0u; k < 4u; ++k) {
                uint child = light_words.data[at + VELK_PIECE_CHILDREN + k];
                if (child >= pages) {
                    unread = false;
                    leaves = false;
                    break;
                }
                uint child_at = velk_light_page(pool, child);
                if (light_words.data[child_at + VELK_PIECE_CHILDREN] < pages) {
                    unread = false;
                    leaves = false;
                } else if (!velk_pieces_unread(child_at, now)) {
                    unread = false;
                }
            }
            bool merge = false;
            if (!unread && leaves) {
                uint read = light_words.data[at + VELK_PIECE_READ];
                uint count = read >> VELK_PIECE_SAME_SHIFT;
                if (velk_pieces_same(pool, at, pages)) {
                    merge = count >= VELK_PIECE_SAME_FRAMES;
                    count = merge ? 0u : count + 1u;
                } else {
                    count = 0u;
                }
                uint now_read = (read & ~VELK_PIECE_SAME_BITS) | (count << VELK_PIECE_SAME_SHIFT);
                if (now_read != read) light_words.data[at + VELK_PIECE_READ] = now_read;
            }
            if (unread || merge) {
                for (uint k = 0u; k < 4u; ++k) light_words.data[at + VELK_PIECE_CHILDREN + k] = VELK_LIGHT_NONE;
            }
        } else if (parent == VELK_LIGHT_NONE && velk_pieces_unread(at, now)) {
            atomicCompSwap(light_words.data[velk_light_root_word(pool, first, triangle)], page, VELK_LIGHT_NONE);
        }
        return;
    }

    // Still linked: from its parent, itself a piece of the same record, or as its record's root.
    bool linked = false;
    if (live && parent == VELK_LIGHT_NONE) {
        linked = light_words.data[velk_light_root_word(pool, first, triangle)] == page;
    } else if (live && parent < pages && velk_light_is_piece(pool, parent)) {
        uint parent_at = velk_light_page(pool, parent);
        if ((light_words.data[parent_at + VELK_PIECE_OWNER] & 0xFFFFFFu) == record) {
            for (uint k = 0u; k < 4u; ++k) linked = linked || light_words.data[parent_at + VELK_PIECE_CHILDREN + k] == page;
        }
    }
    if (!linked) {
        atomicAdd(light_words.data[pool + VELK_POOL_SUM_PIECES_FREED], 1u);
        return;
    }
    atomicOr(light_words.data[light_words.data[pool + VELK_POOL_MARKS_WORD] + (page >> 5u)], 1u << (page & 31u));
    atomicAdd(light_words.data[pool + VELK_POOL_MARK_PIECES], 1u);
    atomicMax(light_words.data[pool + VELK_POOL_MARK_DEEPEST], velk_piece_depth(light_words.data[at + VELK_PIECE_PLACE]));
}
)";

/**
 * Puts every page no mark says is in use on the free stack, no longer a piece, and clears the
 * marks for the next frame. A thread per 32 pages. Starts and ends the frame's marking: with
 * `begin` set, one thread empties the stack, the mark's counts and the search and split lists
 * instead, and moves the pool's frame on.
 */
inline constexpr string_view light_sweep_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint pool_word;
    uint begin;
} pc;
)" VELK_LIGHT_POOL_HEADER_GLSL R"(
void main()
{
    uint pool = pc.pool_word;
    uint word = gl_GlobalInvocationID.x;
    if (pc.begin != 0u) {
        if (word == 0u) {
            light_words.data[pool + VELK_POOL_LAST_RECORDS_PAGED] = light_words.data[pool + VELK_POOL_RECORDS_PAGED];
            light_words.data[pool + VELK_POOL_LAST_RECORDS_ONE_VALUE] = light_words.data[pool + VELK_POOL_RECORDS_ONE_VALUE];
            light_words.data[pool + VELK_POOL_LAST_RECORDS_LINKED] = light_words.data[pool + VELK_POOL_RECORDS_LINKED];
            light_words.data[pool + VELK_POOL_LAST_SEARCHED] = light_words.data[pool + VELK_POOL_SEARCHED];
            light_words.data[pool + VELK_POOL_LAST_TRACES] = light_words.data[pool + VELK_POOL_TRACES];
            light_words.data[pool + VELK_POOL_TRACES] = 0u;
            light_words.data[pool + VELK_POOL_LAST_UNTRACED_PIXELS] = light_words.data[pool + VELK_POOL_UNTRACED_PIXELS];
            light_words.data[pool + VELK_POOL_UNTRACED_PIXELS] = 0u;
            light_words.data[pool + VELK_POOL_RAYS_LISTED] = 0u;
            light_words.data[pool + VELK_POOL_LAST_FIRST_POINTS] = light_words.data[pool + VELK_POOL_FIRST_POINTS];
            light_words.data[pool + VELK_POOL_LAST_LATER_POINTS] = light_words.data[pool + VELK_POOL_LATER_POINTS];
            light_words.data[pool + VELK_POOL_LAST_COARSE_POINTS] = light_words.data[pool + VELK_POOL_COARSE_POINTS];
            light_words.data[pool + VELK_POOL_COARSE_POINTS] = 0u;
            light_words.data[pool + VELK_POOL_LAST_MARK_PIECES] = light_words.data[pool + VELK_POOL_MARK_PIECES];
            light_words.data[pool + VELK_POOL_LAST_MARK_DEEPEST] = light_words.data[pool + VELK_POOL_MARK_DEEPEST];
            light_words.data[pool + VELK_POOL_MARK_PIECES] = 0u;
            light_words.data[pool + VELK_POOL_MARK_DEEPEST] = 0u;
            light_words.data[pool + VELK_POOL_LAST_SPLITS] = light_words.data[pool + VELK_POOL_SPLITS];
            light_words.data[pool + VELK_POOL_SPLITS] = 0u;
            light_words.data[pool + VELK_POOL_FRAME] = light_words.data[pool + VELK_POOL_FRAME] + 1u;
            light_words.data[pool + VELK_POOL_FIRST_POINTS] = 0u;
            light_words.data[pool + VELK_POOL_LATER_POINTS] = 0u;
            light_words.data[pool + VELK_POOL_LATER_RESTARTS] = 0u;
            // The frame's budget in rays, from the steps a ray took the last frame that cast a
            // work group's worth, so that a few short rays do not set it, or any rays while it
            // has none yet; and what first traces asked for, which others leave to them.
            uint rays = light_words.data[pool + VELK_POOL_RAYS_CAST];
            if (rays >= 64u || (rays != 0u && light_words.data[pool + VELK_POOL_RAY_BUDGET] == 0u)) {
                float steps = max(float(light_words.data[pool + VELK_POOL_STEPS]), 1.0);
                light_words.data[pool + VELK_POOL_RAY_BUDGET] = uint(min(float(light_words.data[pool + VELK_POOL_STEP_BUDGET]) * float(rays) / steps, 4.0e9));
            }
            light_words.data[pool + VELK_POOL_LAST_FIRST_RAYS_ASKED] = light_words.data[pool + VELK_POOL_FIRST_RAYS_ASKED];
            light_words.data[pool + VELK_POOL_FIRST_RAYS_ASKED] = 0u;
            // The frame whose walks took the most steps since the log last read it.
            if (light_words.data[pool + VELK_POOL_STEPS] > light_words.data[pool + VELK_POOL_WORST_STEPS]) {
                light_words.data[pool + VELK_POOL_WORST_RAYS] = light_words.data[pool + VELK_POOL_RAYS_CAST];
                light_words.data[pool + VELK_POOL_WORST_STEPS] = light_words.data[pool + VELK_POOL_STEPS];
                light_words.data[pool + VELK_POOL_WORST_FIRST_RAYS] = light_words.data[pool + VELK_POOL_FIRST_RAYS];
            }
            light_words.data[pool + VELK_POOL_RAYS_CAST] = 0u;
            light_words.data[pool + VELK_POOL_STEPS] = 0u;
            light_words.data[pool + VELK_POOL_FIRST_RAYS] = 0u;
            light_words.data[pool + VELK_POOL_FREE_PAGES] = 0u;
            light_words.data[pool + VELK_POOL_PAGES_WANTED] = 0u;
            light_words.data[pool + VELK_POOL_RECORDS_PAGED] = 0u;
            light_words.data[pool + VELK_POOL_RECORDS_ONE_VALUE] = 0u;
            light_words.data[pool + VELK_POOL_PAGES_TAKEN] = 0u;
            light_words.data[pool + VELK_POOL_SEARCHED] = 0u;
            light_words.data[pool + VELK_POOL_RECORDS_LINKED] = 0u;
        }
        return;
    }
    uint pages = light_words.data[pool + VELK_POOL_PAGES];
    if (word * 32u >= pages) return;
    uint marks = light_words.data[pool + VELK_POOL_MARKS_WORD] + word;
    uint used = light_words.data[marks];
    uint free_pages = 0u;
    for (uint b = 0u; b < 32u && word * 32u + b < pages; ++b) {
        if ((used & (1u << b)) == 0u) ++free_pages;
    }
    uint at = light_words.data[pool + VELK_POOL_STACK_WORD] + atomicAdd(light_words.data[pool + VELK_POOL_FREE_PAGES], free_pages);
    for (uint b = 0u; b < 32u && word * 32u + b < pages; ++b) {
        if ((used & (1u << b)) == 0u) light_words.data[at++] = word * 32u + b;
    }
    light_words.data[marks] = 0u;
    // A free page is no piece.
    light_words.data[light_words.data[pool + VELK_POOL_PIECES_WORD] + word] &= used;
}
)";

/// A view's light pool (see kLightNone): its header with the stack, the marks and the search
/// list, and its segments.
struct LightPool
{
    ArenaRegion header;
    vector<ArenaRegion> segments;
    uint32_t max_segments = 0;     ///< As many as the device has room for.
    uint64_t check_frame = 0;      ///< When pages asked for were last read back.
    uint64_t log_frame = 0;        ///< When its counts were last logged.
    uint32_t recorded_pages = 0;   ///< What the culling recording sweeps.

    uint32_t word() const
    {
        return header.valid() ? static_cast<uint32_t>(header.offset() / sizeof(uint32_t)) : 0xFFFFFFFFu;
    }
    uint32_t pages() const { return static_cast<uint32_t>(segments.size()) * kLightSegmentPages; }
};

/// Makes @p pool's header on its first use, with a segment, and as many segments as
/// @p device_bytes of device memory has room for at most: a thirty-second of it, up to
/// kLightMaxSegments.
inline bool ensure_light_pool(LightPool& pool, IGpuArena& arena, uint64_t device_bytes)
{
    if (pool.header.valid()) {
        return true;
    }
    constexpr uint64_t segment_bytes = uint64_t(kLightSegmentPages) * kLightSegmentWordsPerPage * sizeof(uint32_t);
    const uint64_t room = device_bytes != 0 ? device_bytes / 32u / segment_bytes : 1u;
    pool.max_segments = static_cast<uint32_t>(room < 1u ? 1u : room > kLightMaxSegments ? kLightMaxSegments : room);
    // A development aid: VELK_LIGHT_POOL_SEGMENTS caps the pool, to see it run full.
    if (const char* cap = std::getenv("VELK_LIGHT_POOL_SEGMENTS")) {
        const long segments = std::strtol(cap, nullptr, 10);
        if (segments >= 1 && segments < static_cast<long>(pool.max_segments)) {
            pool.max_segments = static_cast<uint32_t>(segments);
        }
    }
    const uint32_t max_pages = pool.max_segments * kLightSegmentPages;
    const uint64_t pieces_word = uint64_t(kLightPoolHeaderWords) + max_pages + max_pages / 32u;
    const uint64_t list_word = pieces_word + max_pages / 32u;
    const uint64_t split_word = list_word + uint64_t(kLightSearchCapacity) * kLightSearchWords;
    const uint64_t trace_word = split_word + uint64_t(kLightSplitCapacity) * kLightSplitWords;
    const uint64_t points_word = trace_word + uint64_t(kLightTraceCapacity) * kLightTraceWords;
    const uint64_t point_list_words = uint64_t(kLightPointCapacity) * kLightPointWords;
    const uint64_t words = points_word + 3u * point_list_words;
    pool.header = arena.alloc(words * sizeof(uint32_t));
    if (!pool.header.valid()) {
        return false;
    }
    const uint32_t base = pool.word();
    vector<uint32_t> initial;
    initial.resize(kLightPoolHeaderWords + 2u * (max_pages / 32u), 0u);
    initial[kLightPoolStackWord] = base + kLightPoolHeaderWords;
    initial[kLightPoolMarksWord] = base + kLightPoolHeaderWords + max_pages;
    initial[kLightPoolPiecesWord] = base + static_cast<uint32_t>(pieces_word);
    initial[kLightPoolSearchWord] = base + static_cast<uint32_t>(list_word);
    initial[kLightPoolSplitWord] = base + static_cast<uint32_t>(split_word);
    initial[kLightPoolTraceWord] = base + static_cast<uint32_t>(trace_word);
    // The first frame's budget in rays, from an assumed walk, until a frame has measured one.
    initial[kLightPoolRayBudget] = kLightTraceStepsBudget / kLightAssumedSteps;
    initial[kLightPoolStepBudget] = kLightTraceStepsBudget;
    initial[kLightPoolFirstPointsWord] = base + static_cast<uint32_t>(points_word);
    initial[kLightPoolLaterPointsWord] = base + static_cast<uint32_t>(points_word + point_list_words);
    initial[kLightPoolCoarsePointsWord] = base + static_cast<uint32_t>(points_word + 2u * point_list_words);
    // The header, then the marks and the piece bits, all clear.
    arena.write_at(pool.header.offset(), initial.data(), kLightPoolHeaderWords * sizeof(uint32_t));
    arena.write_at(pool.header.offset() + (uint64_t(kLightPoolHeaderWords) + max_pages) * sizeof(uint32_t),
                   initial.data() + kLightPoolHeaderWords, uint64_t(2u * (max_pages / 32u)) * sizeof(uint32_t));
    pool.segments.clear();
    return true;
}

/// Adds @p count segments to @p pool, as many as it may have at most, and says so in its header.
/// Returns how many were added.
inline uint32_t grow_light_pool(LightPool& pool, IGpuArena& arena, uint32_t count)
{
    uint32_t added = 0;
    while (added < count && pool.segments.size() < pool.max_segments) {
        ArenaRegion segment = arena.alloc(uint64_t(kLightSegmentPages) * kLightSegmentWordsPerPage * sizeof(uint32_t));
        if (!segment.valid()) {
            break;
        }
        const auto index = static_cast<uint32_t>(pool.segments.size());
        const auto word = static_cast<uint32_t>(segment.offset() / sizeof(uint32_t));
        arena.update_at(pool.header.offset() + (uint64_t(kLightPoolSegmentWords) + index) * sizeof(uint32_t), &word, sizeof(word));
        pool.segments.push_back(std::move(segment));
        ++added;
    }
    const uint32_t pages = pool.pages();
    arena.update_at(pool.header.offset() + uint64_t(kLightPoolPages) * sizeof(uint32_t), &pages, sizeof(pages));
    const auto segments = static_cast<uint32_t>(pool.segments.size());
    arena.update_at(pool.header.offset() + uint64_t(kLightPoolSegments) * sizeof(uint32_t), &segments, sizeof(segments));
    return added;
}

} // namespace velk

#endif // VELK_RENDER_FRAME_LIGHT_POOL_H
