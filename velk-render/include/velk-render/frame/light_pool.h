#ifndef VELK_RENDER_FRAME_LIGHT_POOL_H
#define VELK_RENDER_FRAME_LIGHT_POOL_H

#include <velk/string_view.h>
#include <velk/vector.h>

#include <velk-render/frame/cull_layout.h>
#include <velk-render/interface/intf_gpu_arena.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

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
static_assert(kLightPieceDeepest <= 12u, "a flat patch's piece holds its cell's i in 12 bits (VELK_PIECE_PLACE_PATCH)");
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
/// The pool grows a quarter ahead of a frame asking when it stayed under a sixteenth free this
/// many frames, at most once every kLightPoolAheadEvery: past a full keep of the sweep's shortest
/// (VELK_PIECE_KEEP_FRAMES), so that pieces out of view were let go of first.
/// Pixels VELK_LIGHT_READ can name for the lighting to write its terms at (VELK_POOL_READ_TERMS),
/// and the words of each one's terms (VELK_LIGHT_READ_WORDS).
inline constexpr uint32_t kLightReadPixels = 4;
inline constexpr uint32_t kLightReadWords = 70;
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
    X(WarmupPoints, WARMUP_POINTS, 111)          /* points on the warm-up list this frame, likewise */               \
    X(UntracedPointsWord, UNTRACED_POINTS_WORD, 112) /* the word of the untraced list (kLightPointWords a point) */   \
    X(WarmupPointsWord, WARMUP_POINTS_WORD, 113) /* the word of the warm-up list */                                  \
    X(LastUntracedPoints, LAST_UNTRACED_POINTS, 114)                                                                 \
    X(LastWarmupPoints, LAST_WARMUP_POINTS, 115)                                                                     \
    X(SumChanged, SUM_CHANGED, 116)              /* summed: records whose light a change made due */                 \
    X(FinePoints, FINE_POINTS, 117)              /* points on the fine list this frame, past its room too */         \
    X(CoarsePoints, COARSE_POINTS, 118)          /* points on the coarse list this frame, likewise */                \
    X(CoarsePointsWord, COARSE_POINTS_WORD, 119) /* the word of the coarse list */                                   \
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
    X(FirstRaysNow, FIRST_RAYS_NOW, 181)         /* the rays this frame's never traced coarse points cast at most */ \
    X(SumLaidInherited, SUM_LAID_INHERITED, 182) /* summed: own points laid out from a coarser record's (kLightInherit) */ \
    X(SumPiecesUnread, SUM_PIECES_UNREAD, 183)   /* summed: pieces let go of as no pixel read them */                \
    X(SumPiecesMerged, SUM_PIECES_MERGED, 184)   /* summed: pieces let go of with their parent's light */            \
    X(SumProbesRead, SUM_PROBES_READ, 185)       /* summed: screen probes that read their cell without rays, that \
                                                    added rays to it, that cast rays alone, and settled cells a \
                                                    change woke: four words */                                       \
    X(LastFreePages, LAST_FREE_PAGES, 189)       /* pages the last frame left on the stack, for the pieces' keep */  \
    X(DemandFrames, DEMAND_FRAMES, 190)          /* frames in a row, up to 3, that asked for pages not there */      \
    X(PatchOutside, PATCH_OUTSIDE, 191)          /* not 0 with VELK_PATCH_OUTSIDE: the trace counts the flat patch \
                                                    points it traces outside their patch's triangles */              \
    X(SumPatchPoints, SUM_PATCH_POINTS, 192)     /* summed: flat patch points traced, and of them outside their \
                                                    triangles: two words */                                          \
    X(ReadPixels, READ_PIXELS, 194)              /* VELK_LIGHT_READ's pixels, x | y << 16, kLightNone for none: \
                                                    kLightReadPixels words */                                        \
    X(ReadTerms, READ_TERMS, 198)                /* the lighting's terms at them, kLightReadWords each \
                                                    (VELK_LIGHT_READ_*) */                                           \
    X(HeaderEnd, HEADER_END, 478)

#define VELK_LIGHT_POOL_CPP_WORD(cpp, glsl, word) inline constexpr uint32_t kLightPool##cpp = word;
VELK_LIGHT_POOL_HEADER(VELK_LIGHT_POOL_CPP_WORD)
#undef VELK_LIGHT_POOL_CPP_WORD
/// The header's words, for the shaders: a #define VELK_POOL_ each.
#define VELK_LIGHT_POOL_GLSL_WORD(cpp, glsl, word) "#define VELK_POOL_" #glsl " " #word "u\n"
#define VELK_LIGHT_POOL_HEADER_GLSL VELK_LIGHT_POOL_HEADER(VELK_LIGHT_POOL_GLSL_WORD)
inline constexpr uint32_t kLightPoolHeaderWords = kLightPoolHeaderEnd;
static_assert(kLightPoolTraces == kLightPoolSegmentWords + kLightMaxSegments, "the segments' words end where the trace's begin");
static_assert(kLightPoolReadTerms == kLightPoolReadPixels + kLightReadPixels
                  && kLightPoolHeaderEnd == kLightPoolReadTerms + kLightReadPixels * kLightReadWords,
              "the read pixels' words, then their terms, end the header");
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
/// Points of light each of the four point lists takes a frame (VELK_LIGHT_LIST_UNTRACED and the
/// others), and their words a point (VELK_POINT_ENTRY in cull_layout.h). Past the frame's budget
/// of rays they wait for a later frame.
inline constexpr uint32_t kLightPointCapacity = 65536;  ///< Mirrors VELK_LIGHT_POINT_CAPACITY.
inline constexpr uint32_t kLightPointWords = kPointEntryWords;
/// The light trace's budget in rays a frame to start with, which all of the frame's light work
/// shares by priority (velk_points_trace_point); what does not fit waits for a later frame.
/// adjust_light_rays then scales it by the GPU time the trace took against its target.
inline constexpr uint32_t kLightTraceRays = 65536;
/// The budget in rays when the trace has no time target: more than any frame lists.
inline constexpr uint32_t kLightTraceRaysUnlimited = 1u << 30;

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
// The fewest rays a never traced coarse point's first trace casts (velk_points_first_rays).
#define VELK_LIGHT_FIRST_RAYS_LEAST 1u
#define VELK_LIGHT_DIRECT_PAGES 15u
#define VELK_LIGHT_PIECED 0x40000000u
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
// A piece's point listed as its pixel's coarse level: its rays start anywhere around it, half a
// step of its grid each way, so that its light is that of the place around it and a shadow thinner
// than its spacing shows as partial light (velk_points_trace_point).
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
// points of a pixel's coarse level never traced; its stale points (the coarse level judges where
// pieces split and join, and the lighting falls back to it); points finer than it, and vertices
// only bounces read, never traced or stale; points warming up. Their counts and words in the
// pool's header.
#define VELK_LIGHT_LIST_UNTRACED 0u
#define VELK_LIGHT_LIST_WARMUP 1u
#define VELK_LIGHT_LIST_COARSE 2u
#define VELK_LIGHT_LIST_FINE 3u
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
)" R"(
// A read pixel's terms (VELK_POOL_READ_TERMS), floats as their bits: its albedo, direct light,
// indirect light, the environment's diffuse light, specular light in all, area lights' diffuse
// light and the colour before tonemapping, three words each; then its metallic, whether its points
// of light were traced and probes covered it (bits 0 and 1) with the frame that wrote them (the
// bits from 8 on, its present counter), and how well the probes matched.
#define VELK_LIGHT_READ_WORDS 70u
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

// A blocker's distance as 4 bits: 1 for 1/128 or nearer, doubling each step, 13 for 32 or more.
// 0 is a light no ray of the point went to yet, 14 one a change made stale (what the point has of
// it is a guess its next rays move from), 15 one whose rays found nothing in the way.
uint velk_light_blocker_code(float d)
{
    return uint(clamp(round(log2(max(d, 1e-6))) + 8.0, 1.0, 13.0));
}
#define VELK_LIGHT_BLOCKER_UNTRACED 0u
#define VELK_LIGHT_BLOCKER_STALE 14u
#define VELK_LIGHT_BLOCKER_CLEAR 15u

float velk_light_blocker_distance(uint code)
{
    return exp2(float(code) - 8.0);
}

// A point's trace count (its state's top 4 bits): VELK_LIGHT_WARM_TRACES traces warm it up from
// none, after which it is settled.

// The word of the piece at page `page` that holds the frame something last changed what its
// points could get (light_changes_main_src), VELK_LIGHT_NONE for never: the blocker word of the
// point past its last, which no point uses.
uint velk_piece_changed_word(uint pool, uint page)
{
    return velk_light_page_blocker(pool, page, 2u * VELK_PIECE_POINTS);
}

// The trace count of the own point of the record whose first page starts at `first` with trace
// state `state`, a point of the piece at page `piece` or VELK_LIGHT_NONE for a vertex's: none
// when its record's lights were chosen again, or something changed what it could get (its
// piece's, or its record's), since it was last listed. Such a point keeps its light word until it
// is traced again, but its light is stale: pixels read what is under it, and its next trace starts over.
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

// Flat patches (VELK_LOD_PATCH): a pieced record's root words are by light root slot. A cluster's
// patches are its first slots, and each triangle in none has one of its own after them; without
// patches a triangle's slot is its own index.

// The patches and the light roots of the cluster at word `cluster` of its table: 0 for none.
uint velk_patch_counts(uint cluster)
{
    return VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_PATCH_COUNTS];
}

// Byte `k` of the cluster's patch data from its word `at` on.
uint velk_patch_byte(uint cluster, uint at, uint k)
{
    uint word = VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_PATCHES] + at + k / 4u;
    return (velk_mesh_words.data[word] >> (8u * (k % 4u))) & 255u;
}

// The light root slot of triangle `t` of the cluster.
uint velk_patch_slot(uint cluster, uint t)
{
    return velk_patch_counts(cluster) == 0u ? t : velk_patch_byte(cluster, VELK_LOD_PATCH_SLOTS, t);
}

// Whether light root slot `slot` of the cluster is a patch.
bool velk_patch_is(uint cluster, uint slot)
{
    return slot < (velk_patch_counts(cluster) & 0xFFFFu);
}

// The triangle of light root slot `slot` of the cluster; VELK_LIGHT_NONE for a patch's.
uint velk_patch_triangle(uint cluster, uint slot)
{
    uint patches = velk_patch_counts(cluster) & 0xFFFFu;
    if (patches == 0u) return slot;
    return slot < patches ? VELK_LIGHT_NONE : velk_patch_byte(cluster, VELK_LOD_PATCH_TRIANGLES, slot - patches);
}

// The basis of patch `slot` of the cluster, in its mesh's space: its square is origin + u x + v y
// for x and y from 0 to `side`.
void velk_patch_basis(uint cluster, uint slot, out vec3 origin, out vec3 u, out vec3 v, out float side)
{
    uint w = VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_PATCHES] + VELK_LOD_PATCH_BASES + slot * VELK_LOD_PATCH_BASIS_WORDS;
    vec3 f[3];
    for (uint k = 0u; k < 3u; ++k) {
        f[k] = vec3(uintBitsToFloat(velk_mesh_words.data[w + 3u * k]), uintBitsToFloat(velk_mesh_words.data[w + 3u * k + 1u]),
                    uintBitsToFloat(velk_mesh_words.data[w + 3u * k + 2u]));
    }
    origin = f[0];
    u = f[1];
    v = f[2];
    side = uintBitsToFloat(velk_mesh_words.data[w + VELK_LOD_PATCH_BASIS_SIDE]);
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
// turned bit 26, the depth in bits 27 to 30, and bit 31 (VELK_PIECE_PLACE_EDGE) for a piece made
// where light differed rather than to bring the levels to the light spacing, which alone may be
// joined back into its parent (light_pieces_src). A root, at depth 0, is at (0, 0) and keeps its
// grid's level in bits 0 to 2 instead (velk_piece_root_place).
//
// A piece of a flat patch (VELK_PIECE_PLACE_PATCH, bit 12) is a square cell of its patch's square
// instead: at depth `depth` the square is 2^depth cells along each side, the cell at (i, j), i in
// bits 0 to 11, its grid VELK_PATCH_LEVEL steps along each side, its points row by row in j
// (velk_patch_point). Its children are its quarters, at (2i + (k & 1), 2j + (k >> 1)) for child k.
#define VELK_PIECE_PLACE_EDGE 0x80000000u
#define VELK_PIECE_PLACE_PATCH 0x1000u
#define VELK_PATCH_LEVEL 4u
#define VELK_PATCH_POINTS 25u

bool velk_piece_is_patch(uint place)
{
    return (place & VELK_PIECE_PLACE_PATCH) != 0u;
}

// Point (a, b) of a patch piece's grid.
uint velk_patch_point(uint a, uint b)
{
    return b * (VELK_PATCH_LEVEL + 1u) + a;
}
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
    if (velk_piece_is_patch(place)) return VELK_PATCH_LEVEL;
    return velk_piece_depth(place) == 0u ? clamp(place & 7u, 2u, VELK_PIECE_LEVEL) : VELK_PIECE_LEVEL;
}

// (i, j) of the piece at `place`; a root's is (0, 0).
uvec2 velk_piece_ij(uint place)
{
    return velk_piece_depth(place) == 0u ? uvec2(0u, 0u) : uvec2(place & 4095u, (place >> 13u) & 8191u);
}

// The place of a patch's root.
uint velk_patch_root_place()
{
    return VELK_PIECE_PLACE_PATCH;
}

// The place of child `k` of the patch piece at `place`: its quarter at (k & 1, k >> 1).
uint velk_patch_child_place(uint place, uint k)
{
    uvec2 ij = velk_piece_ij(place);
    return velk_piece_place_word(velk_piece_depth(place) + 1u, 2u * ij.x + (k & 1u), 2u * ij.y + (k >> 1u), false)
         | VELK_PIECE_PLACE_PATCH;
}

// Where point (a, b) of the grid of the patch piece at `place` is in its patch's square, from 0 to
// 1 along each side.
vec2 velk_patch_at(uint place, uvec2 ab)
{
    uvec2 ij = velk_piece_ij(place);
    float cells = exp2(float(velk_piece_depth(place)));
    return (vec2(ij) + vec2(ab) / float(VELK_PATCH_LEVEL)) / cells;
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
// `root_level`; of a patch's piece (`patch`), in a patch whose side is `tri_px` pixels long.
float velk_piece_spacing(bool is_patch, uint depth, uint root_level, float tri_px)
{
    uint n = is_patch ? VELK_PATCH_LEVEL : depth == 0u ? root_level : VELK_PIECE_LEVEL;
    return tri_px / (float(n) * exp2(float(depth)));
}

// The level of a root's grid for a triangle whose longest edge is `tri_px` pixels long, its points
// about `light_pixels` apart: one of VELK_PIECE_ROOT_LEVELS. A triangle at least three times
// VELK_LIGHT_COARSE_PIECES times that long gets the coarsest grid whose points are still that far
// apart, so that its root is its coarse level, whose points look for thin shadows between them
// (VELK_LIGHT_POINT_COARSE), and not its vertices. Not from twice: as many roots more cost
// moving lights about a third more trace, for no thinner shadow found.
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

// Which quarter of a patch's piece the place `l` in it ((x, y, 0), from 0 to 1 along each side)
// is in, as velk_patch_child_place numbers them; `l` becomes the place in that quarter.
uint velk_patch_step(inout vec3 l)
{
    uint k = (l.x >= 0.5 ? 1u : 0u) | (l.y >= 0.5 ? 2u : 0u);
    l = vec3(clamp(2.0 * l.xy - vec2(float(k & 1u), float(k >> 1u)), 0.0, 1.0), 0.0);
    return k;
}

// velk_piece_step, or velk_patch_step for a patch's pieces.
uint velk_way_step(bool is_patch, inout vec3 l)
{
    return is_patch ? velk_patch_step(l) : velk_piece_step(l);
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
    bool is_patch;        // its pieces are a flat patch's, `at` of them (x, y, 0) in their square
};

// Where a pixel or hit is among its record's light roots (velk_light_root_of).
struct VelkLightRoot {
    uint slot;    // its root word's, VELK_LIGHT_NONE when its record has no pieces for its triangle
    bool is_patch; // the slot is a flat patch's
    vec3 at;      // where it is in the root: its triangle's weights, or (x, y, 0) in its patch's square
    float size;   // the root's size in world units: its triangle's longest edge, or its patch's side
};

// Where a pixel or hit at weights `b` of triangle `t` (corners `corners`, its longest edge
// `tri_size` world units) of the record whose first page starts at `first` is among the record's
// light roots. `to_world` takes a vector of the mesh's space to the world, for a patch's side.
VelkLightRoot velk_light_root_of(uint first, uint t, uvec3 corners, vec3 b, float tri_size, mat3 to_world)
{
    VelkLightRoot r;
    r.slot = VELK_LIGHT_NONE;
    r.is_patch = false;
    r.at = b;
    r.size = tri_size;
    uint pieced = velk_light_pieced(first);
    if (pieced == 0u) return r;
    uint cluster = VELK_LIGHT_WORDS.data[first + VELK_PAGE_CLUSTER];
    if (t >= VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_TRIANGLES]) return r;
    uint slot = velk_patch_slot(cluster, t);
    if (slot >= pieced) return r;
    r.slot = slot;
    if (!velk_patch_is(cluster, slot)) return r;
    vec3 origin;
    vec3 u;
    vec3 v;
    float side;
    velk_patch_basis(cluster, slot, origin, u, v, side);
    uint vertex_word = VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_VERTEX];
    vec3 local = vec3(0.0);
    for (uint k = 0u; k < 3u; ++k) {
        uint vw = vertex_word + corners[k] * VELK_VERTEX3D_WORDS;
        local += b[k] * vec3(uintBitsToFloat(velk_mesh_words.data[vw]), uintBitsToFloat(velk_mesh_words.data[vw + 1u]),
                             uintBitsToFloat(velk_mesh_words.data[vw + 2u]));
    }
    vec2 xy = vec2(dot(local - origin, u), dot(local - origin, v)) / max(side, 1e-12);
    r.is_patch = true;
    r.at = vec3(clamp(xy, 0.0, 1.0), 0.0);
    r.size = side * max(length(to_world * u), length(to_world * v));
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
    w.is_patch = r.is_patch;
    if (root >= pages || tri_px <= light_pixels) return w;
    w.root_level = velk_piece_level(VELK_LIGHT_WORDS.data[velk_light_page(pool, root) + VELK_PIECE_PLACE]);
    w.parent = VELK_PIECE_VERTICES;
    w.piece = root;
    w.at = r.at;
    w.spacing = velk_piece_spacing(w.is_patch, 0u, w.root_level, root_px);
    for (uint step = 0u; step <= VELK_PIECE_DEEPEST; ++step) {
        if (w.spacing >= VELK_LIGHT_COARSE_PIECES * light_pixels) {
            w.coarse = w.piece;
            w.coarse_depth = w.depth;
            w.coarse_at = w.at;
            w.coarse_spacing = w.spacing;
        }
        if (w.spacing <= light_pixels || w.depth >= VELK_PIECE_DEEPEST) break;
        vec3 l = w.at;
        uint k = velk_way_step(w.is_patch, l);
        uint child = VELK_LIGHT_WORDS.data[velk_light_page(pool, w.piece) + VELK_PIECE_CHILDREN + k];
        if (child >= pages) break;
        w.parent = w.piece;
        w.parent_at = w.at;
        w.piece = child;
        w.at = l;
        ++w.depth;
        w.spacing = velk_piece_spacing(w.is_patch, w.depth, w.root_level, root_px);
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

// The grid places of the three points of light around place `l` ((x, y, 0), 0 to 1 along each
// side) of a patch's piece, and their weights: each square of its grid is two triangles, split
// along the diagonal from its (1, 0) corner to its (0, 1) corner, the same in its quarters.
void velk_patch_around(vec3 l, out uvec2 places[3], out vec3 weights)
{
    float n = float(VELK_PATCH_LEVEL);
    vec2 a = clamp(l.xy, 0.0, 1.0) * n;
    uvec2 ij = min(uvec2(a), uvec2(VELK_PATCH_LEVEL - 1u));
    vec2 f = clamp(a - vec2(ij), 0.0, 1.0);
    if (f.x + f.y > 1.0) {
        places[0] = ij + uvec2(1u, 1u);
        places[1] = ij + uvec2(0u, 1u);
        places[2] = ij + uvec2(1u, 0u);
        weights = vec3(f.x + f.y - 1.0, 1.0 - f.x, 1.0 - f.y);
    } else {
        places[0] = ij;
        places[1] = ij + uvec2(1u, 0u);
        places[2] = ij + uvec2(0u, 1u);
        weights = vec3(1.0 - f.x - f.y, f.x, f.y);
    }
}

// The three points of the piece at page `page` around weights `l` of it, as words of the pool,
// and their weights; for a patch's piece, around place `l` in it.
void velk_piece_around(uint pool, uint page, vec3 l, out uint words[3], out vec3 weights)
{
    uvec2 places[3];
    uint first = velk_light_page(pool, page);
    uint place = VELK_LIGHT_WORDS.data[first + VELK_PIECE_PLACE];
    if (velk_piece_is_patch(place)) {
        velk_patch_around(l, places, weights);
        for (uint k = 0u; k < 3u; ++k) words[k] = first + 2u * velk_patch_point(places[k].x, places[k].y);
        return;
    }
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

)" R"(
// What the cell of a pixel's coarse level (VelkPieceWay) around it says about the levels under
// it, in the triangle with corners `corners` of the record whose first page starts at `first`:
// 0 when it is not known yet (a point not traced, one of its record's lights not traced at a
// point, or with `fresh`, stale since a change), 1 when
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
    // The record's lights, 4 bits each: a point's blocker code 0 under one is a light not traced.
    uint lit = 0u;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        if (VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s] != VELK_LIGHT_NONE) lit |= 15u << (4u * s);
    }
    for (uint k = 0u; k < 3u; ++k) {
        uint s = VELK_LIGHT_WORDS.data[words[k]];
        if (s == VELK_LIGHT_NONE) return 0u;
        uint codes = VELK_LIGHT_WORDS.data[velk_way_blocker(pool, first, corners, way.coarse, k, words[k])];
        for (uint l = 0u; l < VELK_LIGHT_SLOTS; ++l) {
            if (((lit >> (4u * l)) & 15u) != 0u && ((codes >> (4u * l)) & 15u) == VELK_LIGHT_BLOCKER_UNTRACED) return 0u;
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
    bool is_patch = velk_piece_is_patch(place);
    uint n = velk_piece_level(place);
    uvec2 places[3];
    if (is_patch) velk_patch_around(way.coarse_at, places, w);
    else velk_light_around(n, way.coarse_at, places, w);
    // A grid point's neighbours, in either grid's triangles.
    const ivec2 kAround[6] = ivec2[6](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1), ivec2(1, -1), ivec2(-1, 1));
    for (uint k = 0u; k < 3u; ++k) {
        for (uint a = 0u; a < 6u; ++a) {
            ivec2 q = ivec2(places[k]) + kAround[a];
            if (q.x < 0 || q.y < 0 || (is_patch ? max(q.x, q.y) > int(n) : q.x + q.y > int(n))) continue;
            uint p = is_patch ? velk_patch_point(uint(q.x), uint(q.y)) : velk_piece_point(uint(q.x), uint(q.y));
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
         : list == VELK_LIGHT_LIST_WARMUP   ? VELK_POOL_WARMUP_POINTS
         : list == VELK_LIGHT_LIST_COARSE   ? VELK_POOL_COARSE_POINTS
                                            : VELK_POOL_FINE_POINTS;
}
uint velk_light_list_word_word(uint list)
{
    return list == VELK_LIGHT_LIST_UNTRACED ? VELK_POOL_UNTRACED_POINTS_WORD
         : list == VELK_LIGHT_LIST_WARMUP   ? VELK_POOL_WARMUP_POINTS_WORD
         : list == VELK_LIGHT_LIST_COARSE   ? VELK_POOL_COARSE_POINTS_WORD
                                            : VELK_POOL_FINE_POINTS_WORD;
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
)"

// Putting lights on the trace list, after VELK_LIGHT_POOL_GLSL, for the shaders that list them
// (the claim and the search); apart, since it writes the pool and shaders that only read it
// declare it read-only.
#define VELK_LIGHT_LIST_GLSL R"(
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
                          velk_light_seeded(value) ? 1u : 8u * max(lights, 1u));
}

// Puts the point of light whose words start at `word` on a point list, as `entry_first` and
// `entry_point` (VELK_POINT_ENTRY), once a frame, when it is due (velk_light_point_due); `first`
// is the first page of its record, whose changes make it stale. By the order its trace comes in
// (VELK_LIGHT_LIST_UNTRACED and the others): of a pixel's `coarse` level, never traced; of it,
// stale since a change; finer, never traced or stale; warming up. Its trace state takes the frame, and its count as
// velk_light_point_traces says, so that the trace reads which trace this is. A settled point with
// a light untraced or stale (its blocker word `bw` has a code VELK_LIGHT_BLOCKER_UNTRACED or
// VELK_LIGHT_BLOCKER_STALE) warms up again. Past the list's room it waits for a later frame.
void velk_light_list_at(uint pool, uint word, uint bw, uint first, uint entry_first, uint entry_point, uint instance,
                        uint frame, bool coarse)
{
    uint at = word + 1u;
    uint state = VELK_LIGHT_WORDS.data[at];
    uint piece = (entry_point & VELK_LIGHT_POINT_PIECE) != 0u ? entry_first : VELK_LIGHT_NONE;
    uint traces = velk_light_point_traces(pool, first, piece, state);
    if (!velk_light_point_due(traces)) {
        uint codes = VELK_LIGHT_WORDS.data[bw];
        // Any 4 bits untraced (0) or stale (14, which the xor makes 0).
        uint stale = codes ^ 0xEEEEEEEEu;
        if (((((codes - 0x11111111u) & ~codes) | ((stale - 0x11111111u) & ~stale)) & 0x88888888u) == 0u) return;
        traces = VELK_LIGHT_WARM_TRACES - 1u;
    }
    uint now = frame & 0x0FFFFFFFu;
    if (state != VELK_LIGHT_NONE && (state & 0x0FFFFFFFu) == now) return;
    if (atomicCompSwap(VELK_LIGHT_WORDS.data[at], state, (traces << 28u) | now) != state) return;
    // Coarse before fine, never traced or stale: a new view's coarse light comes in first, the
    // finer points' over it after.
    bool never = VELK_LIGHT_WORDS.data[at - 1u] == VELK_LIGHT_NONE;
    uint list = traces > 0u ? VELK_LIGHT_LIST_WARMUP
              : !coarse     ? VELK_LIGHT_LIST_FINE
              : never       ? VELK_LIGHT_LIST_UNTRACED
                            : VELK_LIGHT_LIST_COARSE;
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

// The strongest light velk_light_slots_at last left out, and the strongest it took, for the log.
float g_slots_left_out = 0.0;
float g_slots_strongest = 0.0;

// The lights that could bring the place at `at` the most, strongest first, as light indices
// from pc.lights_base, VELK_LIGHT_NONE in the slots left over.
void velk_light_slots_at(vec3 at, out uint slots[VELK_LIGHT_SLOTS])
{
    float most[VELK_LIGHT_SLOTS];
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        slots[s] = VELK_LIGHT_NONE;
        most[s] = 0.0;
    }
    g_slots_left_out = 0.0;
    uint first;
    uint candidates = velk_light_grid_count(at + VELK_GLOBALS.cam_pos.xyz, first);
    for (uint c = 0u; c < candidates; ++c) {
        uint i = velk_light_grid_at(first, c);
        float strength = velk_light_strength(velk_light_at(pc.lights_base + i), at);
        if (strength <= most[VELK_LIGHT_SLOTS - 1u]) {
            g_slots_left_out = max(g_slots_left_out, strength);
            continue;
        }
        g_slots_left_out = max(g_slots_left_out, most[VELK_LIGHT_SLOTS - 1u]);
        uint s = VELK_LIGHT_SLOTS - 1u;
        while (s > 0u && most[s - 1u] < strength) {
            most[s] = most[s - 1u];
            slots[s] = slots[s - 1u];
            --s;
        }
        most[s] = strength;
        slots[s] = i;
    }
    g_slots_strongest = most[0];
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
    uint traces = velk_light_point_traces(0u, owner, VELK_LIGHT_NONE, VELK_LIGHT_WORDS.data[word + 1u]);
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
            uint traces = velk_light_point_traces(pool, first, page == VELK_PIECE_VERTICES ? VELK_LIGHT_NONE : page,
                                                  VELK_LIGHT_WORDS.data[words[k] + 1u]);
            trust = !known ? 1.0 : min(float(traces), 1.0);
            if (trust < 1.0) full = false;
            if (trust > 0.0 && sized) {
                uint codes = VELK_LIGHT_WORDS.data[velk_way_blocker(pool, first, corners, page, k, words[k])];
                // The nearest per light: the lower of each 4 bits, 0, stale and clear counting as none.
                for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
                    uint c = (codes >> (4u * s)) & 15u;
                    uint had = (nearest >> (4u * s)) & 15u;
                    if (c != 0u && c < VELK_LIGHT_BLOCKER_STALE && c < had) nearest = (nearest & ~(15u << (4u * s))) | (c << (4u * s));
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
// known, its triangle's corners. A triangle in a flat patch reads its patch's pieces instead of
// its own (velk_light_root_of); `to_world` takes a vector of its mesh's space to the world.
bool velk_light_add_points(uint pool, uint first, uint t, uvec3 corners, vec3 b, vec3 at, float px_per_world,
                           float tri_size, mat3 to_world, inout uint lights[VELK_LIGHT_SLOTS], inout vec2 sums[VELK_LIGHT_SLOTS])
{
    bool traced = false;
    VelkLightRoot r = velk_light_root_of(first, t, corners, b, tri_size, to_world);
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
            uint k = velk_way_step(r.is_patch, down);
            page = VELK_LIGHT_WORDS.data[velk_light_page(pool, page) + VELK_PIECE_CHILDREN + k];
            l = down;
        }
        uint coarse = VELK_LIGHT_WORDS.data[first + VELK_PAGE_COARSE];
        bool known = coarse >= VELK_LIGHT_ONE_VALUE && coarse < VELK_LIGHT_PENDING && (coarse & VELK_LIGHT_ONE_TRACED) != 0u;
        float under[VELK_LIGHT_SLOTS];
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) under[s] = 1.0 - float((coarse >> (3u * s)) & 7u) / 7.0;
        uint nearest = 0xFFFFFFFFu;
        velk_piece_light(pool, best, first, corners, best_at, false, known, under, nearest);
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
                // 15 is none read: no blocker, or none known.
                uint c = (nearest >> (4u * s)) & 15u;
                if (c >= VELK_LIGHT_BLOCKER_STALE) continue;
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
// units, `light_pixels` kLightPixels. `radiance` is the word of its record's radiance, or its
// quad node's, and `radiance_dir` that of the way its pixels face (a record's only), each
// VELK_LIGHT_NONE for none, even when it returns false.
bool velk_light_direct(uint pool, uvec2 ids, vec3 b, vec3 at, float tri_size, float light_pixels,
                       out uint lights[VELK_LIGHT_SLOTS], out float through[VELK_LIGHT_SLOTS], out uint radiance,
                       out uint radiance_dir)
{
    vec2 sums[VELK_LIGHT_SLOTS];
    for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
        lights[k] = VELK_LIGHT_NONE;
        sums[k] = vec2(0.0);
        through[k] = 0.0;
    }
    radiance = VELK_LIGHT_NONE;
    radiance_dir = VELK_LIGHT_NONE;
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint record = velk_ids_record(d, ids);
    if (record == VELK_NO_RECORD) return false;
    GlobalData g = velk_globals.data[d.globals_base];
    uint entry = VELK_LIGHT_WORDS.data[record + VELK_RECORD_ENTRY];
    // Not for a whole mesh or a shape, which no cluster bounds: one value could not hold light
    // that changes across a whole floor.
    if (entry != 0xFFFFFFFFu) {
        radiance = record + VELK_RECORD_RADIANCE;
        radiance_dir = record + VELK_RECORD_RADIANCE_DIR;
    }

    if (entry == VELK_CUT_QUAD) {
        mat4 vp = g.view_projection;
        float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
        uint node = velk_light_quad_node(VELK_LIGHT_WORDS.data[record + VELK_RECORD_QUAD_TREE], VELK_LIGHT_WORDS.data[record + VELK_RECORD_QUAD_NODE],
                                         g.cam_pos.xyz, pixels_per_unit, light_pixels);
        radiance = node + VELK_QUAD_NODE_RADIANCE;
        radiance_dir = VELK_LIGHT_NONE;
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
            // From its cluster's sphere, or its whole mesh's, or a shape's box's middle.
            uint sphere = entry == 0xFFFFFFFFu ? VELK_LIGHT_WORDS.data[record + VELK_RECORD_MESH_TABLE]
                        : VELK_LIGHT_WORDS.data[entry + VELK_CLUSTER_ENTRY_TABLE] + VELK_LIGHT_WORDS.data[record + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS + VELK_LOD_CLUSTER_OWN_SPHERE;
            vec3 local = sphere == VELK_LIGHT_NONE ? vec3(0.5)
                       : vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere]), uintBitsToFloat(VELK_LIGHT_WORDS.data[sphere + 1u]),
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
            mat3 to_world = mat3(velk_sized_world(inst.world_matrix, inst.offset.xyz, inst.size.xyz));
            if (!velk_light_add_points(pool, first, velk_ids_triangle(d, ids), corners, b, at, px_per_world, tri_size, to_world,
                                       lights, sums)) {
                return false;
            }
        }
    }
    for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) through[k] = sums[k].y / max(sums[k].x, 1e-6);
    return true;
}
)" R"(
// For VELK_LIGHT_READ: what the pixel with ids `ids` at weights `b` (`at` relative to the camera,
// `tri_size` its triangle's longest edge) reads of its triangle's pieces, into `e`
// (VELK_LIGHT_EXPLAIN_*). VELK_LIGHT_NONE where it does not apply.
void velk_light_explain(uint pool, uvec2 ids, vec3 b, vec3 at, float tri_size, out uint e[VELK_LIGHT_EXPLAIN_WORDS])
{
    for (uint k = 0u; k < VELK_LIGHT_EXPLAIN_WORDS; ++k) e[k] = VELK_LIGHT_NONE;
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint record = velk_ids_record(d, ids);
    if (record == VELK_NO_RECORD) return;
    uint value = VELK_LIGHT_WORDS.data[record + VELK_RECORD_LIGHT];
    e[VELK_LIGHT_EXPLAIN_RECORD] = value;
    if (value >= VELK_LIGHT_ONE_VALUE || value >= VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES]) return;
    uint first = velk_light_page(pool, value);
    uint cluster = velk_ids_cluster(d, ids);
    if (cluster == VELK_NO_CLUSTER) return;
    uint t = velk_ids_triangle(d, ids);
    e[VELK_LIGHT_EXPLAIN_TRIANGLE] = t;
    e[VELK_LIGHT_EXPLAIN_PIECED] = velk_light_pieced(first);
    e[VELK_LIGHT_EXPLAIN_COARSE] = VELK_LIGHT_WORDS.data[first + VELK_PAGE_COARSE];
    uint word = d.index_base + velk_cluster_first_index(cluster) + t * 3u;
    uvec3 corners = uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
    ElementInstance inst = velk_instances.data[d.instances_base + velk_ids_slot(d, ids)];
    VelkLightRoot r = velk_light_root_of(first, t, corners, b, tri_size,
                                         mat3(velk_sized_world(inst.world_matrix, inst.offset.xyz, inst.size.xyz)));
    if (r.slot == VELK_LIGHT_NONE) return;
    GlobalData g = velk_globals.data[d.globals_base];
    mat4 vp = g.view_projection;
    float px_per_world = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5 / max(length(at), 1e-4);
    uint root = VELK_LIGHT_WORDS.data[velk_light_root_word(pool, first, r.slot)];
    e[VELK_LIGHT_EXPLAIN_ROOT] = root;
    VelkPieceWay way = velk_piece_way(pool, root, b, tri_size * px_per_world, r, r.size * px_per_world, VELK_LIGHT_PIXELS);
    uint cell = velk_way_cell(pool, first, corners, way, true);
    e[VELK_LIGHT_EXPLAIN_DEEPEST] = way.piece == VELK_PIECE_VERTICES ? VELK_LIGHT_NONE : way.depth;
    if (cell == 1u) velk_way_stop_at_coarse(way);
    e[VELK_LIGHT_EXPLAIN_PIECE] = way.piece;
    e[VELK_LIGHT_EXPLAIN_WAY] = way.depth | (cell << 8u) | (uint(clamp(tri_size * px_per_world, 0.0, 65535.0)) << 16u);
    // Its vertex points, then the level it reads: each of the three points around it, its light,
    // its traces and its weight in 255ths.
    for (uint level = 0u; level < 2u; ++level) {
        uint page = level == 0u ? VELK_PIECE_VERTICES : way.piece;
        uint words[3];
        vec3 w;
        velk_way_around(pool, first, corners, page, level == 0u ? b : way.at, words, w);
        uint base = level == 0u ? VELK_LIGHT_EXPLAIN_VERTICES : VELK_LIGHT_EXPLAIN_LEVEL;
        uint weights = 0u;
        for (uint k = 0u; k < 3u; ++k) {
            e[base + k] = VELK_LIGHT_WORDS.data[words[k]];
            e[base + 3u + k] = velk_light_point_traces(pool, first, page == VELK_PIECE_VERTICES ? VELK_LIGHT_NONE : page,
                                                       VELK_LIGHT_WORDS.data[words[k] + 1u]);
            weights |= uint(clamp(w[k], 0.0, 1.0) * 255.0 + 0.5) << (8u * k);
            uint state = VELK_LIGHT_WORDS.data[words[k] + 1u];
            e[VELK_LIGHT_EXPLAIN_LISTED + level * 3u + k] = state == VELK_LIGHT_NONE ? VELK_LIGHT_NONE : state & VELK_PIECE_FRAME_BITS;
        }
        e[base + 6u] = weights;
        e[VELK_LIGHT_EXPLAIN_CHANGED + level] = page == VELK_PIECE_VERTICES ? VELK_LIGHT_WORDS.data[first + VELK_PAGE_CHANGED]
                                                                             : VELK_LIGHT_WORDS.data[velk_piece_changed_word(pool, page)];
    }
    e[VELK_LIGHT_EXPLAIN_FRAME] = VELK_LIGHT_WORDS.data[pool + VELK_POOL_FRAME] & VELK_PIECE_FRAME_BITS;
    // Its corners and the pixel in the world, and the way its first light shines.
    for (uint k = 0u; k < 3u; ++k) {
        uint v = velk_cluster_vertex_word(d, cluster) + corners[k] * VELK_VERTEX3D_WORDS;
        vec3 local = vec3(uintBitsToFloat(velk_mesh_words.data[v]), uintBitsToFloat(velk_mesh_words.data[v + 1u]),
                          uintBitsToFloat(velk_mesh_words.data[v + 2u]));
        vec3 p = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g) + g.cam_pos.xyz;
        for (uint c = 0u; c < 3u; ++c) e[VELK_LIGHT_EXPLAIN_CORNERS + 3u * k + c] = floatBitsToUint(p[c]);
    }
    for (uint c = 0u; c < 3u; ++c) e[VELK_LIGHT_EXPLAIN_AT + c] = floatBitsToUint(at[c] + g.cam_pos[c]);
    uint slot = VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS];
    if (slot != VELK_LIGHT_NONE) {
        vec3 dir = velk_light_at(pc.lights_base + slot).direction.xyz;
        for (uint c = 0u; c < 3u; ++c) e[VELK_LIGHT_EXPLAIN_LIGHT + c] = floatBitsToUint(dir[c]);
    }
}
)"

/**
 * The light trace, three dispatches by VELK_LIGHT_TRACE_MODE: 1, a group that sizes the other two
 * from what the claims listed (their indirect args in the pool's header); 0, the one values on
 * the trace list (light_claim_src lists them), a group of threads each, eight rays a light from
 * its centre; 2, the points of light on the point lists: traced from none over the lanes pc.spare1
 * gives each, warm-ups a thread each (velk_points_trace_point). Built on the light grid's code
 * (light_grid_src), whose velk_light_cell_words is the culling arena; pc.spare3 is the pool.
 */
#define VELK_LIGHT_TRACE_MAIN_GLSL R"(
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
uint g_rays_charged = 0u; // the rays its point was charged against the budget, cast or not
uint g_first_rays = 0u;  // of those, for lights traced for the first time

// How much of the light from a point of `light` chosen by `seed` is kept from `p` (facing `n`,
// `lift` off its surface): 1 where something is in the way, 0 where nothing is, between for
// patches of small members on the way (VELK_CUT_RAY_QUAD). How far away what kept it was is in
// velk_ray_blocker_t, 0 when the surface faces away. With `stratum` 0 to 3, the point is in that
// quarter of the light's samples, so that four rays with each cover the light evenly; 4 for anywhere.
float velk_points_blocked(Light light, vec3 p, vec3 n, float lift, uint seed, uint stratum)
{
    velk_ray_blocker_t = 0.0;
    vec2 r = vec2(float(velk_points_hash(seed) & 0xFFFFu), float(velk_points_hash(seed ^ 0x9e3779b9u) & 0xFFFFu)) / 65535.0;
    if (stratum < 4u) r = (vec2(float(stratum & 1u), float(stratum >> 1u)) + r) * 0.5;
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

)" R"(
// The blocker codes of the slots of `slots` without a light: VELK_LIGHT_BLOCKER_CLEAR, so that a
// code VELK_LIGHT_BLOCKER_UNTRACED always means a light still to trace (velk_light_list_at).
uint velk_points_none_clear(uint slots[VELK_LIGHT_SLOTS])
{
    uint codes = 0u;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        if (slots[s] == VELK_LIGHT_NONE) codes |= VELK_LIGHT_BLOCKER_CLEAR << (4u * s);
    }
    return codes;
}

// The slot of `slots` to trace once more at `p`, chosen at random by `seed` in proportion to
// how much each could bring: among the lights no ray of the point went to yet (their codes in
// `blockers` VELK_LIGHT_BLOCKER_UNTRACED) or that a change made stale (VELK_LIGHT_BLOCKER_STALE)
// while there are any.
uint velk_points_pick(uint slots[VELK_LIGHT_SLOTS], vec3 p, uint seed, uint blockers)
{
    float strengths[VELK_LIGHT_SLOTS];
    float total = 0.0;
    float due = 0.0;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        strengths[s] = slots[s] == VELK_LIGHT_NONE ? 0.0 : velk_light_strength(velk_light_at(pc.lights_base + slots[s]), p);
        total += strengths[s];
        uint code = (blockers >> (4u * s)) & 15u;
        if (code == VELK_LIGHT_BLOCKER_UNTRACED || code == VELK_LIGHT_BLOCKER_STALE) due += strengths[s];
    }
    if (due > 0.0) {
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
            uint code = (blockers >> (4u * s)) & 15u;
            if (code != VELK_LIGHT_BLOCKER_UNTRACED && code != VELK_LIGHT_BLOCKER_STALE) strengths[s] = 0.0;
        }
        total = due;
    }
    float pick = float(velk_points_hash(seed) & 0xFFFFFFu) / 16777215.0 * total;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        if (pick < strengths[s]) return s;
        pick -= strengths[s];
    }
    return 0u;
}

// Traces the point of light at place `i` of the frame's points to trace: of the point lists in
// their order (VELK_LIGHT_LIST_UNTRACED and the others), as many as the frame's one budget in
// rays has left, each starting at a place that moves every frame, so that what waits is not
// always the same. A point never traced, or stale since a
// change, takes at most four rays, to its record's lights by how much each could bring, choosing
// them at its cluster's centre when nothing has yet; a warm-up trace one ray, to one light chosen
// the same way, moving that slot toward what the ray found.
// Where a point of light is: a vertex of a cluster (`vertex`), or weights `b` of the corners of
// triangle `triangle`, which for a coarse point (`around`) move up to half a step of its grid
// (`step_a`, `step_b`) each way, by an R2 sequence from `start`, from its trace 1 on: its first
// is on the grid point itself, as a point traced from none over and over (lights moving) would
// otherwise always sample one place half a step off, which may be under something standing there.
struct VelkPointPlace
{
    uint instance;
    uint vertex_word;
    uint first_index;
    uint triangle;
    uint vertex;
    bool around;
    vec3 b;
    vec3 step_a;
    vec3 step_b;
    vec2 start;
    // A flat patch's point instead (`patch`): `b` is where it is in its mesh's space, `step_a`
    // and `step_b` its grid's steps there, and `normal` the patch's.
    bool is_patch;
    vec3 normal;
};

// The place of `at` at its trace `trace`, relative to the camera, its normal, and how far off its
// surface its rays start: a share of the light spacing where it is, so that it does not shadow
// itself and still sees what stands on it.
void velk_point_place(VelkPointPlace at, uint trace, out vec3 p, out vec3 n, out float lift)
{
    GlobalData g = VELK_GLOBALS;
    ElementInstance inst = velk_instances.data[at.instance];
    vec3 local = vec3(0.0);
    vec3 normal = vec3(0.0);
    if (at.vertex != VELK_LIGHT_NONE) {
        uint vw = at.vertex_word + at.vertex * VELK_VERTEX3D_WORDS;
        local = vec3(uintBitsToFloat(velk_mesh_words.data[vw]), uintBitsToFloat(velk_mesh_words.data[vw + 1u]),
                     uintBitsToFloat(velk_mesh_words.data[vw + 2u]));
        normal = vec3(uintBitsToFloat(velk_mesh_words.data[vw + 3u]), uintBitsToFloat(velk_mesh_words.data[vw + 4u]),
                      uintBitsToFloat(velk_mesh_words.data[vw + 5u]));
    } else if (at.is_patch) {
        local = at.b;
        if (at.around && trace > 0u) {
            vec2 r = fract(at.start + float(trace) * vec2(0.7548777, 0.5698403)) - 0.5;
            local += r.x * at.step_a + r.y * at.step_b;
        }
        normal = at.normal;
    } else {
        vec3 b = at.b;
        if (at.around && trace > 0u) {
            vec2 r = fract(at.start + float(trace) * vec2(0.7548777, 0.5698403)) - 0.5;
            b += r.x * at.step_a + r.y * at.step_b;
            b = max(b, vec3(0.0));
            b /= max(b.x + b.y + b.z, 1e-6);
        }
        for (uint k = 0u; k < 3u; ++k) {
            uint vw = at.vertex_word + velk_mesh_words.data[at.first_index + at.triangle * 3u + k] * VELK_VERTEX3D_WORDS;
            local += b[k] * vec3(uintBitsToFloat(velk_mesh_words.data[vw]), uintBitsToFloat(velk_mesh_words.data[vw + 1u]),
                                 uintBitsToFloat(velk_mesh_words.data[vw + 2u]));
            normal += b[k] * vec3(uintBitsToFloat(velk_mesh_words.data[vw + 3u]), uintBitsToFloat(velk_mesh_words.data[vw + 4u]),
                                  uintBitsToFloat(velk_mesh_words.data[vw + 5u]));
        }
    }
    p = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g);
    n = normalize(mat3(inst.world_matrix) * (normal / max(abs(inst.size.xyz), vec3(1e-6))));
    mat4 vp = g.view_projection;
    float spacing = VELK_LIGHT_PIXELS / max(length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5, 1e-6);
    lift = max(0.05 * spacing * length(p), length(p) * 1e-4);
}

)" R"(
// The point lists in the order they are traced: the first three's points traced from none, the
// warm-up list's one ray each.
uint velk_points_list(uint k)
{
    return k == 0u ? VELK_LIGHT_LIST_UNTRACED : k == 1u ? VELK_LIGHT_LIST_COARSE : k == 2u ? VELK_LIGHT_LIST_FINE
                                                                                      : VELK_LIGHT_LIST_WARMUP;
}

// The rays a point traced from none casts this frame: a ray a lit light, four with a size
// (pc.spare6, as the path counts them), up to four; but for a coarse point never traced, as many
// as still let all those waiting be traced in the budget in rays left, to its lights that could
// bring the most, and at least VELK_LIGHT_FIRST_RAYS_LEAST: a new view's coarse points all get
// light at once, each from as many rays as there is room for, so that a light with a size is not
// fully through or fully blocked from one ray where it is soft. Its other lights stay untraced,
// and its warm-ups trace them next. A point traced again after a change takes all its rays: it
// had light, which its new rays replace at once.
uint velk_points_first_rays(uint pool)
{
    uint from_none = max(pc.spare6, 1u);
    uint budget = VELK_LIGHT_WORDS.data[pool + VELK_POOL_RAY_BUDGET];
    uint left = budget - min(VELK_LIGHT_WORDS.data[pool + VELK_POOL_RAYS_LISTED], budget);
    uint waiting = min(VELK_LIGHT_WORDS.data[pool + velk_light_list_count_word(VELK_LIGHT_LIST_UNTRACED)], VELK_LIGHT_POINT_CAPACITY);
    return clamp(left / max(waiting, 1u), min(VELK_LIGHT_FIRST_RAYS_LEAST, from_none), from_none);
}

// The points of each list (velk_points_list's order) the frame has, and those it traces: each
// list taking what it can of what the budget in rays has left after the trace list's lights
// (pool header 85 of 106) and the lists before it, a point traced from none charged its rays
// (`from_none`, the never traced coarse ones `now_rays`, velk_points_first_rays), a warm-up one.
void velk_points_takes(uint pool, uint now_rays, uint from_none, out uint counts[4], out uint takes[4])
{
    uint budget = VELK_LIGHT_WORDS.data[pool + VELK_POOL_RAY_BUDGET];
    uint left = budget - min(VELK_LIGHT_WORDS.data[pool + VELK_POOL_RAYS_LISTED], budget);
    uint taken = 0u;
    for (uint k = 0u; k < 4u; ++k) {
        counts[k] = min(VELK_LIGHT_WORDS.data[pool + velk_light_list_count_word(velk_points_list(k))], VELK_LIGHT_POINT_CAPACITY);
        uint cost = k == 3u ? 1u : k == 0u ? now_rays : from_none;
        takes[k] = min(counts[k], left / cost);
        left -= takes[k] * cost;
        taken += takes[k];
    }
    // Some every frame, so that light always moves on: the first that has any, a few hundred rays'
    // worth of its points.
    if (taken == 0u) {
        for (uint k = 0u; k < 4u; ++k) {
            if (counts[k] != 0u) {
                takes[k] = min(counts[k], clamp(256u / (k == 3u ? 1u : from_none), 8u, 64u));
                break;
            }
        }
    }
}

// The threads of a work group of the trace.
#define VELK_LIGHT_TRACE_THREADS 64u

// The most lanes a point traced from none spreads its rays over: one a ray, of its four at most.
#define VELK_LIGHT_RAY_LANES 4u

// The most points a work group traces spread, two lanes or more each.
#define VELK_LIGHT_SPREAD_POINTS (VELK_LIGHT_TRACE_THREADS / 2u)
// How much of a light one ray found blocked, in the sums below: in 4096ths.
#define VELK_LIGHT_SPREAD_UNIT 4096.0

// What a point traced from none with its rays spread over its lanes leaves for the first of them
// to store (velk_points_store_spread), by its place among the group's points: where its words are
// (VELK_LIGHT_NONE for no point) and its state; and by its light slot, what its lanes' rays to
// that light found, added up: how much was blocked (in VELK_LIGHT_SPREAD_UNIT), the nearest
// blocker (a distance's float bits, which order as the distances do), and the light's rays (0 for
// no light).
shared uint s_point_word[VELK_LIGHT_SPREAD_POINTS];
shared uint s_point_blocker_word[VELK_LIGHT_SPREAD_POINTS];
shared uint s_point_state[VELK_LIGHT_SPREAD_POINTS];
// And what its lights no ray reached keep: their light and blockers, a nibble a slot.
shared uint s_point_kept[VELK_LIGHT_SPREAD_POINTS];
shared uint s_point_kept_blockers[VELK_LIGHT_SPREAD_POINTS];
shared uint s_slot_blocked[VELK_LIGHT_SPREAD_POINTS * VELK_LIGHT_SLOTS];
shared uint s_slot_nearest[VELK_LIGHT_SPREAD_POINTS * VELK_LIGHT_SLOTS];
shared uint s_slot_rays[VELK_LIGHT_SPREAD_POINTS * VELK_LIGHT_SLOTS];

// Stores a point's light `value`, its `blockers` and its trace state, whose words start at `pw`
// and `bw`: the count it was traced to, `traces`, and the frame it was listed, from `state` as the
// listing left it.
)" R"(
void velk_points_store(uint pw, uint bw, uint value, uint blockers, uint traces, uint state, uint frame)
{
    // Never the word for untraced: all eight slots fully dark reads one step lighter.
    if (value == VELK_LIGHT_NONE) value = 0xEFFFFFFFu;
    VELK_LIGHT_WORDS.data[pw] = value;
    VELK_LIGHT_WORDS.data[bw] = blockers;
    uint listed = state == VELK_LIGHT_NONE ? frame & 0x0FFFFFFFu : state & 0x0FFFFFFFu;
    VELK_LIGHT_WORDS.data[pw + 1u] = (min(traces, 15u) << 28u) | listed;
}

// Traces the point of light at place `i` of the frame's points to trace (velk_points_takes'
// `counts` and `takes`), on lane `lane` of `lanes` the point has: alone, a lane does all of it and
// stores it; spread, a point traced from none numbers its `first_rays` rays, each lane casting
// those whose number is its lane modulo `lanes`, and adds what they found into the group's s_slot
// arrays for its place among the group's points, `point_in_group`, which velk_points_store_spread
// puts together.
void velk_points_trace_point(uint pool, uint i, uint frame, uint counts[4], uint takes[4], uint lane, uint lanes,
                             uint first_rays, uint point_in_group)
{
    GlobalData g = VELK_GLOBALS;
    uint list = VELK_LIGHT_NONE;
    uint listed_points = 0u;
    uint place = i;
    for (uint k = 0u; k < 4u; ++k) {
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
    // A coarse point never traced: as many as the sizing chose (velk_points_first_rays).
    if (list == VELK_LIGHT_LIST_UNTRACED) first_rays = clamp(VELK_LIGHT_WORDS.data[pool + VELK_POOL_FIRST_RAYS_NOW], 1u, first_rays);
    uint from_none = first_rays;
    place = (place + velk_points_hash(frame * 2654435761u + list) % listed_points) % listed_points;
    uint w = VELK_LIGHT_WORDS.data[pool + velk_light_list_word_word(list)] + place * VELK_LIGHT_POINT_WORDS;
    uint first = VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_FIRST];
    uint q = VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_POINT];
    uint instance = VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_INSTANCE];
    uint pages = VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES];
    // A piece's point: its page, and its record's first page and light root slot from its owner word.
    uint piece = VELK_LIGHT_NONE;
    uint piece_slot = 0u;
    bool around = (q & VELK_LIGHT_POINT_COARSE) != 0u;
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
    // Which trace this is, as its listing counted it.
    uint traces = state == VELK_LIGHT_NONE ? 0u : state >> 28u;
    VelkPointPlace at;
    at.instance = instance;
    at.vertex_word = vertex_word;
    at.first_index = first_index;
    at.triangle = 0u;
    at.vertex = VELK_LIGHT_NONE;
    at.around = around;
    at.b = vec3(0.0);
    at.step_a = vec3(0.0);
    at.step_b = vec3(0.0);
    at.start = vec2(0.0);
    at.is_patch = false;
    at.normal = vec3(0.0);
    if (piece == VELK_LIGHT_NONE) {
        if (q >= vertices) return;
        at.vertex = q;
    } else if (velk_patch_is(cluster, piece_slot)) {
        // A patch's point: on its square, which may reach past its triangles.
        uint place = VELK_LIGHT_WORDS.data[velk_light_page(pool, piece) + VELK_PIECE_PLACE];
        if (!velk_piece_is_patch(place) || q >= (VELK_PATCH_LEVEL + 1u) * (VELK_PATCH_LEVEL + 1u)) return;
        vec3 origin;
        vec3 u;
        vec3 v;
        float side;
        velk_patch_basis(cluster, piece_slot, origin, u, v, side);
        uvec2 ab = uvec2(q % (VELK_PATCH_LEVEL + 1u), q / (VELK_PATCH_LEVEL + 1u));
        vec2 xy = velk_patch_at(place, ab) * side;
        at.is_patch = true;
        at.b = origin + xy.x * u + xy.y * v;
        at.normal = cross(u, v);
        // VELK_PATCH_OUTSIDE: on its first trace, whether its place is in one of its patch's
        // triangles, edges counting as in.
        if (VELK_LIGHT_WORDS.data[pool + VELK_POOL_PATCH_OUTSIDE] != 0u && lane == 0u && traces == 0u) {
            bool inside = false;
            uint triangles = min(VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_TRIANGLES], 256u);
            float tolerance = 1e-6 * side * side;
            for (uint t = 0u; t < triangles && !inside; ++t) {
                if (velk_patch_slot(cluster, t) != piece_slot) continue;
                vec2 c[3];
                for (uint k = 0u; k < 3u; ++k) {
                    uint vw = vertex_word + velk_mesh_words.data[first_index + t * 3u + k] * VELK_VERTEX3D_WORDS;
                    vec3 corner = vec3(uintBitsToFloat(velk_mesh_words.data[vw]), uintBitsToFloat(velk_mesh_words.data[vw + 1u]),
                                       uintBitsToFloat(velk_mesh_words.data[vw + 2u]));
                    c[k] = vec2(dot(corner - origin, u), dot(corner - origin, v));
                }
                float d0 = (c[1].x - c[0].x) * (xy.y - c[0].y) - (c[1].y - c[0].y) * (xy.x - c[0].x);
                float d1 = (c[2].x - c[1].x) * (xy.y - c[1].y) - (c[2].y - c[1].y) * (xy.x - c[1].x);
                float d2 = (c[0].x - c[2].x) * (xy.y - c[2].y) - (c[0].y - c[2].y) * (xy.x - c[2].x);
                inside = (d0 >= -tolerance && d1 >= -tolerance && d2 >= -tolerance)
                      || (d0 <= tolerance && d1 <= tolerance && d2 <= tolerance);
            }
            atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_PATCH_POINTS], 1u);
            if (!inside) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_PATCH_POINTS + 1u], 1u);
        }
        if (around) {
            uint own = velk_points_hash(piece * 977u + q);
            at.start = vec2(float(own & 0xFFFFu), float(own >> 16u)) / 65536.0;
            float step = side / (float(VELK_PATCH_LEVEL) * exp2(float(velk_piece_depth(place))));
            at.step_a = step * u;
            at.step_b = step * v;
        }
    } else {
        at.triangle = velk_patch_triangle(cluster, piece_slot);
        if (at.triangle >= VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_TRIANGLES]) return;
        uint place = VELK_LIGHT_WORDS.data[velk_light_page(pool, piece) + VELK_PIECE_PLACE];
        uvec2 ab = velk_piece_place(q);
        if (ab.x + ab.y > velk_piece_level(place)) return;
        at.b = velk_piece_weights(place, ab);
        if (around) {
            // A coarse point: somewhere within half a step of its grid each way, a different place
            // each of its traces, spread evenly over them (an R2 sequence from a start of its own),
            // kept in its triangle.
            uint own = velk_points_hash(piece * 977u + q);
            at.start = vec2(float(own & 0xFFFFu), float(own >> 16u)) / 65536.0;
            vec3 b0 = velk_piece_weights(place, uvec2(0u, 0u));
            at.step_a = velk_piece_weights(place, uvec2(1u, 0u)) - b0;
            at.step_b = velk_piece_weights(place, uvec2(0u, 1u)) - b0;
        }
    }
    vec3 p;
    vec3 n;
    float lift;
    velk_point_place(at, traces, p, n, lift);
    // Its rays by which trace this is, not by the frame: traced again from none while things move
    // near it, it casts the same rays, so that its light changes only where what is in its way does.
    uint seed = velk_points_hash((piece == VELK_LIGHT_NONE ? first : pw) * 977u + q) ^ (traces * 0x9e3779b9u);
    uint value = 0u;
    uint bw = piece == VELK_LIGHT_NONE ? velk_light_blocker_word(pool, first, q) : velk_light_page_blocker(pool, piece, 2u * q);
    uint blockers = 0u;
    bool from_none_spread = false;
    if (traces == 0u || old == VELK_LIGHT_NONE) {
        // From none: the light it had before a change left out. Its `first_rays` rays (at most
        // four, whatever its lights: what one costs does not grow with them) go to its lights by
        // what each could bring here, a light with a size taking up to four (its four quarters,
        // and a coarse point's places of its first four traces), one without a size one. Where
        // that covers all its lights, as with one light with a size or a few without, each gets
        // them all and the point counts as traced four times, or after a change as settled:
        // traced on to more rays, it would step from those rays' light to its mean each time a
        // change comes near it, and flicker while things keep moving there. Otherwise the
        // strongest get them, and a light none reached keeps what it had before a change, or
        // starts from its record's coarse light; the point then warms up.
        // Spread, its rays numbered in order, each lane casting every lanes-th of them from its
        // own on; every lane works out the same rays.
        bool restart = old != VELK_LIGHT_NONE;
        bool four = false;
        bool spread = lanes > 1u;
        from_none_spread = spread;
        uint slot_base = point_in_group * VELK_LIGHT_SLOTS;
        // Per slot, 4 bits each: the rays its light takes in full (4 with a size, 1 without, 0 for
        // none), and those it is given.
        uint full = 0u;
        uint given = 0u;
        uint capacity = 0u;
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
            if (slots[s] == VELK_LIGHT_NONE) continue;
            Light l = velk_light_at(pc.lights_base + slots[s]);
            uint f = l.params.w > 0.0 || l.flags.x == 3u ? 4u : 1u;
            four = four || f == 4u;
            full |= f << (4u * s);
            capacity += f;
        }
)" R"(
        // Each ray's slot (low 4 bits, 15 for none) and its place among that slot's rays (the next
        // 2), 8 bits a ray.
        uint ray_table = 0xFFFFFFFFu;
        if (capacity <= first_rays) {
            // All of every light's rays, slot by slot.
            uint r = 0u;
            for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
                uint f = (full >> (4u * s)) & 15u;
                for (uint k = 0u; k < f && r < 4u; ++k, ++r) ray_table = (ray_table & ~(0xFFu << (8u * r))) | ((s | (k << 4u)) << (8u * r));
            }
            given = full;
        } else {
            // By what each could bring here, over the slots that can take another, evenly spaced
            // over the rays.
            float strengths[VELK_LIGHT_SLOTS];
            for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
                strengths[s] = slots[s] == VELK_LIGHT_NONE ? 0.0 : max(velk_light_strength(velk_light_at(pc.lights_base + slots[s]), p), 1e-12);
            }
            float jitter = float(velk_points_hash(seed ^ 0x2545f491u) & 0xFFFFFFu) / 16777216.0;
            for (uint r = 0u; r < first_rays; ++r) {
                float left = 0.0;
                for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
                    if (((given >> (4u * s)) & 15u) < ((full >> (4u * s)) & 15u)) left += strengths[s];
                }
                float pick = (float(r) + jitter) / float(first_rays) * left;
                uint chosen = 15u;
                uint last_open = 15u;
                for (uint s = 0u; s < VELK_LIGHT_SLOTS && chosen == 15u; ++s) {
                    if (((given >> (4u * s)) & 15u) >= ((full >> (4u * s)) & 15u)) continue;
                    last_open = s;
                    if (pick < strengths[s]) {
                        chosen = s;
                    } else {
                        pick -= strengths[s];
                    }
                }
                // Past the last by rounding: the last that can take one.
                if (chosen == 15u) chosen = last_open;
                if (chosen == 15u) continue;
                uint rank = (given >> (4u * chosen)) & 15u;
                ray_table = (ray_table & ~(0xFFu << (8u * r))) | ((chosen | (rank << 4u)) << (8u * r));
                given += 1u << (4u * chosen);
            }
        }
        bool fully = given == full;
        // Alone, what each ray found: blocked and the nearest blocker.
        float ray_blocked[4] = float[4](0.0, 0.0, 0.0, 0.0);
        float ray_nearest[4] = float[4](1e30, 1e30, 1e30, 1e30);
        for (uint r = 0u; r < 4u; ++r) {
            if (r >= first_rays) break;
            uint s = (ray_table >> (8u * r)) & 15u;
            if (s == 15u || (spread && r % lanes != lane)) continue;
            uint j = (ray_table >> (8u * r + 4u)) & 3u;
            uint n_s = (given >> (4u * s)) & 15u;
            if (!restart) ++g_first_rays;
            vec3 pj = p;
            vec3 nj = n;
            float lj = lift;
            if (around && j > 0u) velk_point_place(at, j, pj, nj, lj);
            // A light's rays over its quarters: all four, or two opposite ones.
            uint stratum = ((full >> (4u * s)) & 15u) == 4u ? (n_s == 4u ? j : n_s == 2u ? j * 3u : 4u) : 4u;
            float blocked = velk_points_blocked(velk_light_at(pc.lights_base + slots[s]), pj, nj, lj, seed + s * 4u + j, stratum);
            if (spread) {
                atomicAdd(s_slot_rays[slot_base + s], 1u);
                if (blocked > 0.0) {
                    atomicAdd(s_slot_blocked[slot_base + s], uint(blocked * VELK_LIGHT_SPREAD_UNIT + 0.5));
                    atomicMin(s_slot_nearest[slot_base + s], floatBitsToUint(max(velk_ray_blocker_t, 0.0)));
                }
            } else {
                ray_blocked[r] = blocked;
                if (blocked > 0.0) ray_nearest[r] = velk_ray_blocker_t;
            }
        }
)" R"(
        // What a light no ray reached keeps: after a change what it had, else its record's coarse
        // light where that is traced, else nothing in its way; and its blocker code, which
        // says whether a ray of the point ever went to it (VELK_LIGHT_BLOCKER_UNTRACED until one
        // does). A light that brings nothing here needs no ray: nothing in its way.
        uint kept_blockers = 0u;
        uint kept = 0u;
        if (!fully) {
            uint coarse = VELK_LIGHT_WORDS.data[first + VELK_PAGE_COARSE];
            bool coarse_known = coarse >= VELK_LIGHT_ONE_VALUE && coarse < VELK_LIGHT_PENDING && (coarse & VELK_LIGHT_ONE_TRACED) != 0u;
            uint had_blockers = restart ? VELK_LIGHT_WORDS.data[bw] : 0u;
            for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
                if (slots[s] == VELK_LIGHT_NONE) continue;
                uint nibble = restart ? (old >> (4u * s)) & 15u : coarse_known ? (((coarse >> (3u * s)) & 7u) * 15u + 3u) / 7u : 0u;
                uint code = (had_blockers >> (4u * s)) & 15u;
                if (code == VELK_LIGHT_BLOCKER_UNTRACED && velk_light_strength(velk_light_at(pc.lights_base + slots[s]), p) <= 0.0) {
                    nibble = 0u;
                    code = VELK_LIGHT_BLOCKER_CLEAR;
                }
                kept |= nibble << (4u * s);
                kept_blockers |= code << (4u * s);
            }
        }
        if (spread) {
            if (lane == 0u) {
                s_point_kept[point_in_group] = kept;
                s_point_kept_blockers[point_in_group] = kept_blockers | velk_points_none_clear(slots);
            }
        } else {
            for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
                uint n_s = (given >> (4u * s)) & 15u;
                if (n_s == 0u) {
                    value |= kept & (15u << (4u * s));
                    blockers |= kept_blockers & (15u << (4u * s));
                    continue;
                }
                float sum = 0.0;
                float nearest = 1e30;
                for (uint r = 0u; r < 4u; ++r) {
                    if (r >= first_rays) break;
                    if (((ray_table >> (8u * r)) & 15u) != s) continue;
                    sum += ray_blocked[r];
                    nearest = min(nearest, ray_nearest[r]);
                }
                value |= uint(sum / float(n_s) * 15.0 + 0.5) << (4u * s);
                blockers |= (nearest < 1e29 ? velk_light_blocker_code(nearest) : VELK_LIGHT_BLOCKER_CLEAR) << (4u * s);
            }
        }
        // Stored as one more.
        traces = fully && four ? (restart ? VELK_LIGHT_WARM_TRACES - 1u : 3u) : 0u;
        if (lane == 0u) {
            atomicAdd(VELK_LIGHT_WORDS.data[pool + (restart ? VELK_POOL_SUM_RETRACES : VELK_POOL_SUM_FIRST_TRACES)], 1u);
            // What the budget charged it (velk_points_trace_point's from_none), whatever it cast.
            g_rays_charged = from_none;
        }
    } else {
        // One ray: the point's first lane's.
        if (lane != 0u) return;
        g_rays_charged = 1u;
        // Warming up, a running mean of its rays; a light's first ray replaces what it kept, and
        // the rays of one a change made stale move its light from what it had as a settled
        // point's would, so that a light moving a little does not turn a soft shadow into one ray.
        float step = 1.0 / float(traces + 1u);
        value = old;
        blockers = VELK_LIGHT_WORDS.data[bw];
        uint s = velk_points_pick(slots, p, seed, blockers);
        if (slots[s] != VELK_LIGHT_NONE) {
            float blocked = velk_points_blocked(velk_light_at(pc.lights_base + slots[s]), p, n, lift, seed + 7u, traces & 3u);
            uint had = (blockers >> (4u * s)) & 15u;
            bool first_ray = had == VELK_LIGHT_BLOCKER_UNTRACED;
            if (first_ray) step = 1.0;
            if (had == VELK_LIGHT_BLOCKER_STALE) step = 1.0 / float(VELK_LIGHT_WARM_TRACES);
            if (blocked > 0.0 || first_ray || had == VELK_LIGHT_BLOCKER_STALE) {
                uint code = blocked > 0.0 ? velk_light_blocker_code(velk_ray_blocker_t) : VELK_LIGHT_BLOCKER_CLEAR;
                blockers = (blockers & ~(15u << (4u * s))) | (code << (4u * s));
            }
            float was = float((old >> (4u * s)) & 15u);
            // Rounded up as often as the fraction says, so that small steps still arrive and
            // the mean is not pulled toward the first rays.
            float dither = float(velk_points_hash(seed ^ 0x51ed270bu) & 0xFFFFu) / 65536.0;
            uint now = min(uint(mix(was, blocked * 15.0, step) + dither), 15u);
            value = (old & ~(15u << (4u * s))) | (now << (4u * s));
        }
        // Not settled while a light that brings something here has had no ray, or is stale: it
        // warms on. One that brings nothing here (moved out of reach since a change) needs none.
        for (uint t = 0u; t < VELK_LIGHT_SLOTS; ++t) {
            uint code = (blockers >> (4u * t)) & 15u;
            if (slots[t] == VELK_LIGHT_NONE || (code != VELK_LIGHT_BLOCKER_UNTRACED && code != VELK_LIGHT_BLOCKER_STALE)) continue;
            if (velk_light_strength(velk_light_at(pc.lights_base + slots[t]), p) > 0.0) {
                traces = min(traces, VELK_LIGHT_WARM_TRACES - 2u);
            } else {
                blockers |= VELK_LIGHT_BLOCKER_CLEAR << (4u * t);
            }
        }
        atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_LATER_TRACES], 1u);
    }
    // A slot without a light: nothing to trace, which its code says.
    blockers |= velk_points_none_clear(slots);
    if (from_none_spread) {
        if (lane != 0u) return;
        s_point_word[point_in_group] = pw;
        s_point_blocker_word[point_in_group] = bw;
        s_point_state[point_in_group] = (min(traces + 1u, 15u) << 28u) | (state == VELK_LIGHT_NONE ? frame & 0x0FFFFFFFu
                                                                                                   : state & 0x0FFFFFFFu);
        return;
    }
    velk_points_store(pw, bw, value, blockers, traces + 1u, state, frame);
}
)" R"(
// Puts together and stores the group's point `point` traced spread, after a barrier: each light's
// share blocked, the mean of its rays, and its nearest blocker, from what its lanes added up; a
// light no ray reached as its first lane kept it; the rest as its first lane left it. Nothing when
// it had no point.
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
        float sum = float(s_slot_blocked[at]) / VELK_LIGHT_SPREAD_UNIT;
        value |= uint(min(sum / float(rays), 1.0) * 15.0 + 0.5) << (4u * s);
        uint code = s_slot_nearest[at] != VELK_LIGHT_NONE ? velk_light_blocker_code(uintBitsToFloat(s_slot_nearest[at])) : VELK_LIGHT_BLOCKER_CLEAR;
        blockers |= code << (4u * s);
    }
    if (value == VELK_LIGHT_NONE) value = 0xEFFFFFFFu;
    VELK_LIGHT_WORDS.data[pw] = value;
    VELK_LIGHT_WORDS.data[s_point_blocker_word[point]] = blockers;
    VELK_LIGHT_WORDS.data[pw + 1u] = s_point_state[point];
}

)" R"(
shared uint s_slots[VELK_LIGHT_SLOTS];
// A one value's rays, a thread each.
shared float s_blocked[VELK_LIGHT_TRACE_THREADS];

void main()
{
    uint pool = pc.spare3;
    uint index = gl_WorkGroupID.x;
    uint lane = gl_LocalInvocationIndex;
    // The lanes a point traced from none spreads its rays over at most, a ray each: a power of
    // two up to VELK_LIGHT_RAY_LANES (pc.spare1's low byte); and its rays (the next byte).
    uint most_lanes = clamp(pc.spare1 & 0xFFu, 1u, VELK_LIGHT_RAY_LANES);
    uint first_rays = clamp((pc.spare1 >> 8u) & 0xFFu, 1u, 4u);
#if VELK_LIGHT_TRACE_MODE == 1
    // The dispatches' sizes (a group): the trace list's, a group a one value, and the point
    // lists' from the points each traces this frame (velk_points_takes), kept for them.
    {
        if (lane != 0u) return;
        uint counts[4];
        uint takes[4];
        uint now_rays = min(first_rays, velk_points_first_rays(pool));
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_FIRST_RAYS_NOW] = now_rays;
        velk_points_takes(pool, now_rays, first_rays, counts, takes);
        // Points traced from none spread their rays over as many lanes as keep the threads near
        // what fills the device (a power of two up to most_lanes): fewer threads would leave it
        // waiting on the longest of them walking its rays in a row, more only repeat each point's
        // setup.
        uint from_none_points = takes[0] + takes[1] + takes[2];
        uint fill = VELK_LIGHT_WORDS.data[pool + VELK_POOL_FILL_THREADS];
        uint lanes = 1u;
        while (lanes < most_lanes && from_none_points * lanes * 2u <= fill) lanes *= 2u;
        uint per_group = VELK_LIGHT_TRACE_THREADS / lanes;
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_SPREAD_LANES] = lanes;
        // No more points traced from none than a dispatch's groups hold, next to the warm-ups'
        // (at most VELK_LIGHT_POINT_CAPACITY / 64 groups): those past it wait, the latest lists'.
        uint room = (65535u - VELK_LIGHT_POINT_CAPACITY / VELK_LIGHT_TRACE_THREADS) * per_group;
        for (uint k = 0u; k < 3u; ++k) {
            takes[k] = min(takes[k], room);
            room -= takes[k];
        }
        for (uint k = 0u; k < 4u; ++k) {
            VELK_LIGHT_WORDS.data[pool + VELK_POOL_POINT_TAKES + k] = takes[k];
            VELK_LIGHT_WORDS.data[pool + VELK_POOL_POINT_COUNTS + k] = counts[k];
        }
        uint spread = takes[0] + takes[1] + takes[2];
        uint spread_groups = (spread + per_group - 1u) / per_group;
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_SPREAD_GROUPS] = spread_groups;
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_TRACE_ARGS] = min(VELK_LIGHT_WORDS.data[pool + VELK_POOL_TRACES], VELK_LIGHT_TRACE_CAPACITY);
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_TRACE_ARGS + 1u] = 1u;
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_TRACE_ARGS + 2u] = 1u;
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_POINT_ARGS] = spread_groups + (takes[3] + VELK_LIGHT_TRACE_THREADS - 1u) / VELK_LIGHT_TRACE_THREADS;
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_POINT_ARGS + 1u] = 1u;
        VELK_LIGHT_WORDS.data[pool + VELK_POOL_POINT_ARGS + 2u] = 1u;
    }
#elif VELK_LIGHT_TRACE_MODE == 2
    // The point lists: those traced from none first, their lights spread over the point's lanes,
    // so that no thread walks all of a point's rays alone, then the warm-ups, a thread each. A
    // group is all of one kind, so that the barrier is the whole group's.
    {
        uint counts[4];
        uint takes[4];
        for (uint k = 0u; k < 4u; ++k) {
            takes[k] = VELK_LIGHT_WORDS.data[pool + VELK_POOL_POINT_TAKES + k];
            counts[k] = VELK_LIGHT_WORDS.data[pool + VELK_POOL_POINT_COUNTS + k];
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
                    s_slot_nearest[u] = VELK_LIGHT_NONE;
                    s_slot_rays[u] = 0u;
                }
                memoryBarrierShared();
                barrier();
            }
            // By ray first: side by side, the same ray of neighbouring points, which mostly goes
            // to the same light, so that a wave's walks go one way.
            uint in_group = lane % per_group;
            uint point = group * per_group + in_group;
            if (point < spread) velk_points_trace_point(pool, point, frame, counts, takes, lane / per_group, lanes, first_rays, in_group);
            if (lanes > 1u) {
                memoryBarrierShared();
                barrier();
                if (lane < per_group) velk_points_store_spread(lane);
            }
        } else {
            uint point = spread + (group - spread_groups) * VELK_LIGHT_TRACE_THREADS + lane;
            velk_points_trace_point(pool, point, frame, counts, takes, 0u, 1u, first_rays, 0u);
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
    // The same rays each time it is traced, two in each quarter of the light, so that it changes
    // only where what is in its way does.
    uint seed = velk_points_hash(at);
    // Out of its own sphere toward each light, facing it. Traced once, and again only when
    // something changes it: a few rays a light, so that it is settled at once.
    uint s = lane / 8u;
    uint light = s_slots[s];
    float blocked = 0.0;
    if (light != VELK_LIGHT_NONE) {
        Light l = velk_light_at(pc.lights_base + light);
        vec3 toward = l.flags.x == 0u ? -l.direction.xyz : normalize(l.position.xyz - center);
        ++g_first_rays;
        blocked = velk_points_blocked(l, center, toward, radius * 1.01 + length(center) * 1e-4, seed + lane, lane & 3u);
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
#endif
}
)"

/// The light trace's three shaders (VELK_LIGHT_TRACE_MAIN_GLSL), each only its own part, so that
/// each holds only its own code: the sizes of the other two, the one values, the points.
inline constexpr string_view light_trace_args_src = "#define VELK_LIGHT_TRACE_MODE 1\n" VELK_LIGHT_TRACE_MAIN_GLSL;
inline constexpr string_view light_trace_ones_src = "#define VELK_LIGHT_TRACE_MODE 0\n" VELK_LIGHT_TRACE_MAIN_GLSL;
inline constexpr string_view light_trace_points_src = "#define VELK_LIGHT_TRACE_MODE 2\n" VELK_LIGHT_TRACE_MAIN_GLSL;

/// What changed this frame, for light_changes_main_src: VELK_CHANGE_LIST, then a VELK_CHANGE a
/// change (cull_layout.h).
inline constexpr uint32_t kLightChangeHeaderWords = kChangeListChanges;
inline constexpr uint32_t kLightChangeWords = kChangeWords;
/// The most changes a frame lists; more count as everything changed.
inline constexpr uint32_t kLightChanges = 128;
/// On a change's light (kChangeLight): it only moved, by less than its size, so that what its
/// points have of it is a good start (VELK_LIGHT_BLOCKER_STALE) rather than none.
inline constexpr uint32_t kLightChangeSmall = 0x40000000u;

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
// (VELK_CHANGE_SMALL): what the points have of those is no start for their next rays.
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

// Blocker codes `codes` with the slots of `bits` made due: those of `hard` untraced
// (VELK_LIGHT_BLOCKER_UNTRACED), their next ray replacing what they have; the rest stale
// (VELK_LIGHT_BLOCKER_STALE), those not traced yet left so.
uint velk_change_stale(uint codes, uint bits, uint hard)
{
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        if ((bits & (1u << s)) == 0u || ((codes >> (4u * s)) & 15u) == VELK_LIGHT_BLOCKER_UNTRACED) continue;
        uint code = (hard & (1u << s)) != 0u ? VELK_LIGHT_BLOCKER_UNTRACED : VELK_LIGHT_BLOCKER_STALE;
        codes = (codes & ~(15u << (4u * s))) | (code << (4u * s));
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

)" R"(
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
// reach (a coarse point's rays start around it), tested as the record was.
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
    vec3 center;
    float radius;
    if (velk_patch_is(cluster, slot)) {
        // A patch's piece: its square cell.
        vec3 origin;
        vec3 u;
        vec3 v;
        float side;
        velk_patch_basis(cluster, slot, origin, u, v, side);
        // Both diagonals, as a scaled instance may stretch one more than the other, widened by
        // what a coarse point's samples move past its edges.
        vec2 lo = velk_patch_at(place, uvec2(0u, 0u)) * side;
        vec2 hi = velk_patch_at(place, uvec2(n, n)) * side;
        vec3 mid = origin + 0.5 * (lo.x + hi.x) * u + 0.5 * (lo.y + hi.y) * v;
        center = velk_rel_position(inst.world_matrix, inst.offset.xyz + mid * inst.size.xyz, g);
        mat3 to_world = mat3(inst.world_matrix);
        vec3 a = to_world * ((hi.x - lo.x) * u * inst.size.xyz);
        vec3 b = to_world * ((hi.y - lo.y) * v * inst.size.xyz);
        radius = 0.5 * max(length(a + b), length(a - b)) * (1.0 + 1.0 / float(n));
    } else {
        uint triangle = velk_patch_triangle(cluster, slot);
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
        center = (p0 + p1 + p2) / 3.0;
        radius = max(max(length(p0 - center), length(p1 - center)), length(p2 - center)) * (1.0 + 0.5 / float(n));
    }
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
        VELK_LIGHT_WORDS.data[bw] = velk_change_stale(VELK_LIGHT_WORDS.data[bw], altered, hard);
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
        // (velk_change_stale), which keep the rest, and what they had of the small ones for
        // warm-up rays to move from; its
        // pieces likewise as far as the change is in their way (the pieces pass, pc.mode 1). Its
        // coarse light stays: its points are trusted, and a coarse light's rays (eight a light)
        // every frame a light moves would take the budget from them.
        uint now = VELK_GLOBALS.present_counter & 0x0FFFFFFFu;
        VELK_LIGHT_WORDS.data[first + VELK_PAGE_INSTANCE] = instance;
        VELK_LIGHT_WORDS.data[first + VELK_PAGE_TOUCHED] = now;
        uint points = velk_light_own_points(first);
        for (uint p = 0u; p < points; ++p) {
            uint bw = velk_light_blocker_word(pool, first, p);
            VELK_LIGHT_WORDS.data[bw] = velk_change_stale(VELK_LIGHT_WORDS.data[bw], altered, hard);
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
)";

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

// A probe's light (VELK_PROBE_LIGHT), for a shader that defines VELK_LIGHT_WORDS as the buffer
// the probes are in: read, written and seen from a normal.
#define VELK_PROBE_GLSL R"(
// A probe's light: `e` the mean radiance arriving at its own normal, weighted by cosine, and for
// each colour the L1 direction of the radiance, three times its mean times the direction.
struct VelkProbeLight {
    vec3 e;
    vec3 r;
    vec3 g;
    vec3 b;
};

VelkProbeLight velk_probe_read(uint at)
{
    vec2 h0 = unpackHalf2x16(VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT]);
    vec2 h1 = unpackHalf2x16(VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 1u]);
    vec2 h2 = unpackHalf2x16(VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 2u]);
    vec2 h3 = unpackHalf2x16(VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 3u]);
    vec2 h4 = unpackHalf2x16(VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 4u]);
    vec2 h5 = unpackHalf2x16(VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 5u]);
    VelkProbeLight p;
    p.e = vec3(h0.x, h0.y, h1.x);
    p.r = vec3(h1.y, h2.x, h2.y);
    p.g = vec3(h3.x, h3.y, h4.x);
    p.b = vec3(h4.y, h5.x, h5.y);
    return p;
}

// The probe's normal at `at`, octahedral.
vec3 velk_probe_normal(uint at)
{
    vec2 e = unpackSnorm2x16(VELK_LIGHT_WORDS.data[at + VELK_PROBE_NORMAL]);
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

vec3 velk_probe_position(uint at)
{
    return vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[at + VELK_PROBE_POSITION]),
                uintBitsToFloat(VELK_LIGHT_WORDS.data[at + VELK_PROBE_POSITION + 1u]),
                uintBitsToFloat(VELK_LIGHT_WORDS.data[at + VELK_PROBE_POSITION + 2u]));
}

// What a probe at normal `pn` with light `p` brings a surface facing `n`, unclamped: its light at
// its own normal, moved by its directions to `n`. For sums, clamped once after them.
vec3 velk_probe_eval_raw(VelkProbeLight p, vec3 pn, vec3 n)
{
    vec3 d = n - pn;
    return p.e + vec3(dot(p.r, d), dot(p.g, d), dot(p.b, d)) / 3.0;
}

// The same, never below zero.
vec3 velk_probe_eval(VelkProbeLight p, vec3 pn, vec3 n)
{
    return max(velk_probe_eval_raw(p, pn, n), vec3(0.0));
}
)"

// Writing a probe whole (VELK_PROBE), after VELK_PROBE_GLSL, for a shader whose VELK_LIGHT_WORDS
// is writable.
#define VELK_PROBE_WRITE_GLSL R"(
uint velk_probe_oct_encode(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return packSnorm2x16(e);
}

// Halves past their range would be infinite: kept within it.
uint velk_probe_halves(float a, float b)
{
    return packHalf2x16(clamp(vec2(a, b), vec2(-65000.0), vec2(65000.0)));
}

// Writes the light `p` at `at`, six words as VELK_PROBE_LIGHT holds them.
void velk_probe_write_light(uint at, VelkProbeLight p)
{
    VELK_LIGHT_WORDS.data[at] = velk_probe_halves(p.e.x, p.e.y);
    VELK_LIGHT_WORDS.data[at + 1u] = velk_probe_halves(p.e.z, p.r.x);
    VELK_LIGHT_WORDS.data[at + 2u] = velk_probe_halves(p.r.y, p.r.z);
    VELK_LIGHT_WORDS.data[at + 3u] = velk_probe_halves(p.g.x, p.g.y);
    VELK_LIGHT_WORDS.data[at + 4u] = velk_probe_halves(p.g.z, p.b.x);
    VELK_LIGHT_WORDS.data[at + 5u] = velk_probe_halves(p.b.y, p.b.z);
}

// Writes probe `at` whole: its light, where it is, its normal and its frames.
void velk_probe_write(uint at, VelkProbeLight p, vec3 pos, vec3 n, float frames)
{
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT] = velk_probe_halves(p.e.x, p.e.y);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 1u] = velk_probe_halves(p.e.z, p.r.x);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 2u] = velk_probe_halves(p.r.y, p.r.z);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 3u] = velk_probe_halves(p.g.x, p.g.y);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 4u] = velk_probe_halves(p.g.z, p.b.x);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 5u] = velk_probe_halves(p.b.y, p.b.z);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_POSITION] = floatBitsToUint(pos.x);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_POSITION + 1u] = floatBitsToUint(pos.y);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_POSITION + 2u] = floatBitsToUint(pos.z);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_NORMAL] = velk_probe_oct_encode(n);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_FRAMES] = floatBitsToUint(frames);
}
)"

// The probes' cells (kProbeCellSlots), for a shader with VELK_PROBE_GLSL and VELK_PROBE_WRITE_GLSL:
// a cell's words, and finding one.
#define VELK_PROBE_CELL_GLSL R"(
#define VELK_CELL_SLOTS 131072u
#define VELK_CELL_WORDS 12u
#define VELK_CELL_KEY 0u       // its key's hash, 0 for a free slot
#define VELK_CELL_CHECK 1u     // a second hash of its key
#define VELK_CELL_LIGHT 2u     // six words, as VELK_PROBE_LIGHT
#define VELK_CELL_SAMPLES 8u   // the rays' frames it holds, up to VELK_CELL_SETTLED
#define VELK_CELL_READ 9u      // the frame a probe last read it
#define VELK_CELL_UPDATED 10u  // the frame a probe last added its rays to it
#define VELK_CELL_NORMAL 11u   // the normal its light is at, octahedral
#define VELK_CELL_SETTLED 64u
#define VELK_CELL_WOKEN 4u
#define VELK_CELL_KEEP 240u
#define VELK_CELL_PROBES 8u

vec3 velk_cell_normal(uint cell)
{
    vec2 e = unpackSnorm2x16(VELK_LIGHT_WORDS.data[cell + VELK_CELL_NORMAL]);
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

uint velk_cell_hash(uvec3 v)
{
    uint h = (v.x * 0x8da6b343u) ^ (v.y * 0xd8163841u) ^ (v.z * 0xcb1ab31fu);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// The cell of a place at `pos` (relative to the camera) facing `n`: its key and second hash. A cell
// is a cube of the world a power of two wide, the widest no wider than a probe's square is at the
// place's distance (at least an eighth), and one of six ways a surface faces. `level` is its width
// as a power of two, from -3. At most as wide as the square, so that a cell has about one probe
// and gets as many rays as a probe while light changes.
void velk_cell_key_at(vec3 pos, vec3 n, int level, out uint key, out uint check)
{
    GlobalData g = VELK_GLOBALS;
    float size = exp2(float(level));
    // The camera's place in whole cells and what is left of it, so that far from the origin the
    // cell is found without adding a large number to a small one.
    vec3 cam = g.cam_pos.xyz;
    vec3 cam_cells = floor(cam / size);
    ivec3 c = ivec3(cam_cells) + ivec3(floor((pos + (cam - cam_cells * size)) / size));
    vec3 a = abs(n);
    uint face = a.x >= a.y && a.x >= a.z ? (n.x >= 0.0 ? 0u : 1u) : a.y >= a.z ? (n.y >= 0.0 ? 2u : 3u) : (n.z >= 0.0 ? 4u : 5u);
    uint level_face = (uint(level + 32) << 3u) | face;
    key = velk_cell_hash(uvec3(c) ^ uvec3(level_face * 0x9e3779b9u, level_face, 0u)) | 1u;
    check = velk_cell_hash(uvec3(c.yzx) + uvec3(level_face, 0x51ed270bu, level_face * 0x2545f491u));
}

void velk_cell_key(vec3 pos, vec3 n, out uint key, out uint check, out int level)
{
    GlobalData g = VELK_GLOBALS;
    mat4 vp = g.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
    float square = float(VELK_PROBE_PIXELS) * length(pos) / max(pixels_per_unit, 1e-6);
    level = min(int(floor(log2(max(square, 0.125)))), 27);
    velk_cell_key_at(pos, n, level, key, check);
}

// The cell with `key` and `check` in the table at word `table` that holds light, without taking
// a slot for it: 0xFFFFFFFF when there is none. All its slots are looked at, as one let go of
// (velk_probe_cells_age) leaves a free slot before cells that are still there.
uint velk_cell_lookup(uint table, uint key, uint check)
{
    for (uint p = 0u; p < VELK_CELL_PROBES; ++p) {
        uint cell = table + ((key + p) & (VELK_CELL_SLOTS - 1u)) * VELK_CELL_WORDS;
        uint k = VELK_LIGHT_WORDS.data[cell + VELK_CELL_KEY];
        if (k == key && VELK_LIGHT_WORDS.data[cell + VELK_CELL_CHECK] == check) {
            return VELK_LIGHT_WORDS.data[cell + VELK_CELL_SAMPLES] > 0u ? cell : 0xFFFFFFFFu;
        }
    }
    return 0xFFFFFFFFu;
}

// The cell holding light for a place at `pos` (relative to the camera) facing `n`, as wide as a
// probe there would take or the next width up: 0xFFFFFFFF when neither holds any.
uint velk_cell_light_at(uint table, vec3 pos, vec3 n)
{
    uint key;
    uint check;
    int level;
    velk_cell_key(pos, n, key, check, level);
    uint cell = velk_cell_lookup(table, key, check);
    if (cell != 0xFFFFFFFFu) return cell;
    velk_cell_key_at(pos, n, level + 1, key, check);
    return velk_cell_lookup(table, key, check);
}

// The cell with `key` and `check` in the table at word `table`, or a free slot taken for it:
// `claimed` when this call took it. 0xFFFFFFFF when its slots are all others'.
uint velk_cell_find(uint table, uint key, uint check, out bool claimed)
{
    claimed = false;
    for (uint p = 0u; p < VELK_CELL_PROBES; ++p) {
        uint cell = table + ((key + p) & (VELK_CELL_SLOTS - 1u)) * VELK_CELL_WORDS;
        uint k = VELK_LIGHT_WORDS.data[cell + VELK_CELL_KEY];
        if (k == 0u) {
            k = atomicCompSwap(VELK_LIGHT_WORDS.data[cell + VELK_CELL_KEY], 0u, key);
            if (k == 0u) {
                VELK_LIGHT_WORDS.data[cell + VELK_CELL_CHECK] = check;
                claimed = true;
                return cell;
            }
        }
        if (k == key && VELK_LIGHT_WORDS.data[cell + VELK_CELL_CHECK] == check) return cell;
    }
    return 0xFFFFFFFFu;
}
)"

/**
 * The screen probes' gather, in two dispatches. The first, a thread a probe, decides what each
 * probe does with its cell (velk_probes_decide): those that read it are done, those that cast
 * rays go to a list, which sizes the second, an indirect dispatch of four probes of the list a
 * group of kLightProbeGroupThreads, a ray a thread, kLightProbeRays a probe (velk_probes_trace),
 * so that its cost follows the probes that cast rays. The rays are spread over the hemisphere
 * around the probe's normal by cosine: each takes its own quarter of a 4 by 4 grid of directions,
 * a different cell of it each frame so that four frames cover all sixteen, staggered by the
 * probe's place so that each 2 by 2 block of probes covers them all every frame, moved within the
 * cell by a value of the probe's own. A hit brings what the light points there say reaches it and
 * its indirect light (velk_probe_hit_indirect), times its albedo, and what it gives off; a miss
 * the environment. The probe's light from its rays goes to the ray store; light_probes_filter_src
 * then filters it.
 *
 * A hit's albedo and emission come from its draw's shading summary (velk_probe_surface), so it is
 * composed by compose_rt_trace, without the materials' code. pc.shapes_base is the light pool,
 * pc.env.z the light header (the grid), pc.env.w the last frame's probe store, pc.extras the ray
 * store, the G-buffer's size (x low, y high), its world position and its normal, pc.more the
 * cells, the frame everything last changed, this frame's change list and the probe list (bit 31
 * set for the trace).
 */
inline constexpr string_view light_probes_main_src = R"(
layout(set = 1, binding = 16, std430) buffer ProbeWords { uint data[]; } probe_words;
#define VELK_LIGHT_WORDS probe_words
#define velk_light_cell_words probe_words
#define CELLS_HEADER pc.env.z
)" VELK_LIGHT_POOL_GLSL R"(
#define VELK_PROBE_PIXELS 16u
#define VELK_PROBE_RAYS 4u
)" VELK_PROBE_GLSL VELK_PROBE_WRITE_GLSL VELK_PROBE_CELL_GLSL R"(
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
#define VELK_DRAW_SHADE_METALLIC_SHIFT 26u

vec3 velk_srgb_to_linear(vec3 s)
{
    return mix(s / 12.92, pow((s + 0.055) / 1.055, vec3(2.4)), greaterThan(s, vec3(0.04045)));
}

// What a ray sees of the surface it hit, without its material's code: its draw's shading
// summary, its albedo times, where its material takes them, its instance's colour and its vertex
// colours, and on a quad the quad's colour. A lit surface reflects that albedo less its metallic
// diffusely (`albedo`), and its metal reflects the rest tinted by it (`metal`, for what it
// reflects from the way a ray would mirror off it); an unlit one gives it off, on top of its
// emission. White, lit and not metal where the hit names no draw.
void velk_probe_surface(RayHit hit, out vec3 albedo, out vec3 metal, out vec3 emission, out bool lit)
{
    albedo = vec3(1.0);
    metal = vec3(0.0);
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
    float metallic = float(look >> VELK_DRAW_SHADE_METALLIC_SHIFT) / 63.0;
    albedo = lit ? surface * (1.0 - metallic) : vec3(0.0);
    // As F0 is, the dielectric base reflecting 4% of what is not metal.
    metal = lit ? mix(vec3(0.04), surface, metallic) : vec3(0.0);
    emission = velk_rgb9e5(glow) + (lit ? vec3(0.0) : surface);
}

)" R"(
// What each ray found and which way it went, for its probe's first thread.
shared vec3 s_light[VELK_RT_GROUP_THREADS];
shared vec3 s_dir[VELK_RT_GROUP_THREADS];
// The deepest any thread's walk stack got, for one write to the log's word a group.
shared uint s_deepest;
// A probe's rays alone, its cell read without rays, or its rays added to its cell.
#define VELK_CELL_OWN 0u
#define VELK_CELL_READ_ONLY 1u
#define VELK_CELL_ADD 2u

// Whether a change in the change list at word `changes` (light_changes_main_src's, VELK_CHANGE_LIST)
// could alter the light bouncing to a cell `level` wide (velk_cell_key) at `pos` (relative to the
// camera): one that wakes cells that wide (VELK_CHANGE_WAKES), and is a light that reaches half
// again as far as its range (its light reaches what bounces it here), or any light without a
// range, or a box within its own size (what moved shadows and lights its surroundings by about as
// much).
bool velk_cell_changed(uint changes, vec3 pos, int level)
{
    if (changes == 0xFFFFFFFFu) return false;
    vec3 cam = VELK_GLOBALS.cam_pos.xyz;
    uint bit = 1u << uint(clamp(level + 3, 0, 31));
    uint count = min(probe_words.data[changes + VELK_CHANGE_LIST_COUNT], 128u);
    for (uint k = 0u; k < count; ++k) {
        uint w = changes + VELK_CHANGE_LIST_CHANGES + k * VELK_CHANGE_WORDS;
        if ((probe_words.data[w + VELK_CHANGE_WAKES] & bit) == 0u) continue;
        vec3 lo = vec3(uintBitsToFloat(probe_words.data[w + VELK_CHANGE_MIN]), uintBitsToFloat(probe_words.data[w + VELK_CHANGE_MIN + 1u]),
                       uintBitsToFloat(probe_words.data[w + VELK_CHANGE_MIN + 2u])) - cam;
        vec3 hi = vec3(uintBitsToFloat(probe_words.data[w + VELK_CHANGE_MAX]), uintBitsToFloat(probe_words.data[w + VELK_CHANGE_MAX + 1u]),
                       uintBitsToFloat(probe_words.data[w + VELK_CHANGE_MAX + 2u])) - cam;
        uint word = probe_words.data[w + VELK_CHANGE_LIGHT];
        if (word != 0xFFFFFFFFu) {
            // Its light, without the small move's mark (kLightChangeSmall).
            float range = velk_light_at(pc.lights_base + (word & ~0x40000000u)).params.x;
            if (range <= 0.0 || distance(pos, lo) <= 1.5 * range) return true;
            continue;
        }
        if (distance(pos, clamp(pos, lo, hi)) <= max(length(hi - lo), 1.0)) return true;
    }
    return false;
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

// Where a hit at `at` keeps its light: the word of its light, of its radiance and of the way the
// pixels that gave that face (a record's only; a quad faces one way), and the place its one
// value's lights are chosen at. Its record's, at the hit; for a quad, the node as wide as
// kLightPixels over it, at the node's centre, as the lighting pass finds it. All VELK_LIGHT_NONE
// for none. `ask` is whether a hit may ask for its one value to be traced (velk_probe_ask): not
// for a record for light only, which light_search_src lays out only from none.
void velk_probe_hit_words(RayHit hit, vec3 at, out uint light, out uint radiance, out uint radiance_dir, out vec3 center,
                          out bool ask)
{
    light = VELK_LIGHT_NONE;
    radiance = VELK_LIGHT_NONE;
    radiance_dir = VELK_LIGHT_NONE;
    center = at;
    ask = false;
    if (hit.record == VELK_NO_RECORD) return;
    uint entry = probe_words.data[hit.record + VELK_RECORD_ENTRY];
    if (entry == VELK_CUT_QUAD) {
        GlobalData g = VELK_GLOBALS;
        mat4 vp = g.view_projection;
        float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
        uint node = velk_light_quad_node(probe_words.data[hit.record + VELK_RECORD_QUAD_TREE],
                                         probe_words.data[hit.record + VELK_RECORD_QUAD_NODE], g.cam_pos.xyz, pixels_per_unit,
                                         VELK_LIGHT_PIXELS);
        light = node + VELK_QUAD_NODE_LIGHT;
        radiance = node + VELK_QUAD_NODE_RADIANCE;
        center = vec3(uintBitsToFloat(probe_words.data[node + VELK_QUAD_NODE_CENTER]),
                      uintBitsToFloat(probe_words.data[node + VELK_QUAD_NODE_CENTER + 1u]),
                      uintBitsToFloat(probe_words.data[node + VELK_QUAD_NODE_CENTER + 2u])) - g.cam_pos.xyz;
        ask = true;
        return;
    }
    if (entry == VELK_CUT_RAY_QUAD || entry == VELK_CUT_STALE) return;
    light = hit.record + VELK_RECORD_LIGHT;
    // Kept indirect light only on a cluster, which bounds it (velk_light_direct).
    if (entry != 0xFFFFFFFFu) {
        radiance = hit.record + VELK_RECORD_RADIANCE;
        radiance_dir = hit.record + VELK_RECORD_RADIANCE_DIR;
    }
    ask = !velk_cut_light_only(entry);
}

// Asks for the light at `word` to be traced, for a hit: none yet becomes an untraced one value,
// and an untraced one value is marked asked, which the next frame's mark lists for tracing
// (light_mark_src). Light with pages, or being given them, is left as it is.
void velk_probe_ask(uint word)
{
    uint value = probe_words.data[word];
    if (value == VELK_LIGHT_NONE) {
        atomicCompSwap(probe_words.data[word], VELK_LIGHT_NONE, VELK_LIGHT_ONE_VALUE | VELK_LIGHT_ONE_ASKED);
        return;
    }
    if (value < VELK_LIGHT_ONE_VALUE || value >= VELK_LIGHT_PENDING) return;
    if ((value & (VELK_LIGHT_ONE_TRACED | VELK_LIGHT_ONE_ASKED)) == 0u) atomicOr(probe_words.data[word], VELK_LIGHT_ONE_ASKED);
}

// The direct light a surface at `at`, facing `n`, gets as its light points there say, for a hit
// whose light is at `word`: its one value, its lights chosen at `center`, or its record's points:
// down its triangle's pieces where the hit knows its triangle (RayHit::triangle,
// velk_light_add_points), else at its triangle's corners; each light by how much of it gets
// through.
vec3 velk_probe_direct(RayHit hit, uint word, vec3 center, vec3 at, vec3 n)
{
    uint pool = pc.shapes_base;
    if (word == VELK_LIGHT_NONE || pool == 0xFFFFFFFFu) return vec3(0.0);
    uint value = probe_words.data[word];
    if (value >= VELK_LIGHT_PENDING) return vec3(0.0);
    uint lights[VELK_LIGHT_SLOTS];
    vec2 sums[VELK_LIGHT_SLOTS];
    for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
        lights[k] = VELK_LIGHT_NONE;
        sums[k] = vec2(0.0);
    }
    if (value >= VELK_LIGHT_ONE_VALUE) {
        if ((value & VELK_LIGHT_ONE_TRACED) == 0u) return vec3(0.0);
        velk_light_add_one(value, center, lights, sums);
    } else {
        if (value >= probe_words.data[pool + VELK_POOL_PAGES] || hit.corners.x == VELK_NO_CORNERS) return vec3(0.0);
        vec3 b = vec3(1.0 - hit.weights.x - hit.weights.y, hit.weights.x, hit.weights.y);
        // Where the hit knows its triangle: down its pieces to the deepest whose points around
        // it are traced and fresh (velk_light_add_points). Else its triangle's corners.
        uint first = velk_light_page(pool, value);
        uint t = hit.triangle != VELK_NO_CORNERS ? hit.triangle : 0xFFFFFFFFu;
        if (!velk_light_add_points(pool, first, t, hit.corners, b, at, 0.0, 0.0, mat3(1.0), lights, sums)) {
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

// The environment along `rd`, `blur` of the way (0 to 1) from its sharpest mip level to its
// blurriest. Black without one.
vec3 velk_probe_env(vec3 rd, float blur)
{
    if (pc.env.y == 0u) return vec3(0.0);
    float c = cos(pc.env_params.y);
    float s = sin(pc.env_params.y);
    vec3 dir = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
    vec2 uv = vec2(atan(dir.z, dir.x) / 6.28318531 + 0.5, asin(clamp(dir.y, -1.0, 1.0)) / 3.14159265 + 0.5);
    float last = float(textureQueryLevels(velk_textures[nonuniformEXT(pc.env.y)])) - 1.0;
    return textureLod(velk_textures[nonuniformEXT(pc.env.y)], uv, clamp(blur, 0.0, 1.0) * last).rgb * pc.env_params.x;
}

// The environment along `rd`, as a probe's miss sees it: its blurriest level.
vec3 velk_probe_sky(vec3 rd)
{
    return velk_probe_env(rd, 1.0);
}

// The indirect light a hit at `at` facing `n` gets, as the lighting pass would light it: its
// probes' cell in the table at word `cells` (VELK_PROBE_CELL_GLSL), as wide as a probe there takes
// or the next width up, so that light bounces once more each frame and stays where the camera
// left it; else the last frame's probe where the hit was on screen, when that probe was on the
// same surface; else what its record or quad node keeps at `radiance` from the pixels that showed
// it, where those faced about as it does (`radiance_dir`, their way on average, VELK_LIGHT_NONE
// for a quad's); else the environment around its normal, unshadowed. A record seen all round
// averages to a short way, which no hit agrees with.
vec3 velk_probe_hit_indirect(uint cells, vec3 at, vec3 n, uint radiance, uint radiance_dir, uint width, uint height, uint across)
{
    GlobalData g = VELK_GLOBALS;
    if (cells != 0xFFFFFFFFu) {
        uint cell = velk_cell_light_at(cells, at, n);
        if (cell != 0xFFFFFFFFu) return velk_probe_eval(velk_probe_read(cell + VELK_CELL_LIGHT), velk_cell_normal(cell), n);
    }
    uint was_store = pc.env.w;
    vec4 clip = g.prev_view_projection * vec4(at + g.cam_moved, 1.0);
    if (was_store != 0xFFFFFFFFu && clip.w > 1e-5) {
        vec2 p = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(width), float(height));
        if (all(greaterThanEqual(p, vec2(0.0))) && p.x < float(width) && p.y < float(height)) {
            uint before = was_store + ((uint(p.y) / VELK_PROBE_PIXELS) * across + uint(p.x) / VELK_PROBE_PIXELS) * VELK_PROBE_WORDS;
            if (uintBitsToFloat(probe_words.data[before + VELK_PROBE_FRAMES]) > 0.0) {
                vec3 then_pos = velk_probe_position(before) - g.cam_moved;
                vec3 then_n = velk_probe_normal(before);
                if (dot(then_n, n) > 0.9 && abs(dot(then_n, at - then_pos)) < 0.05 * length(at) + 1e-3) {
                    return velk_probe_eval(velk_probe_read(before), then_n, n);
                }
            }
        }
    }
    if (radiance != VELK_LIGHT_NONE) {
        uint kept = probe_words.data[radiance];
        bool facing = radiance_dir == VELK_LIGHT_NONE || dot(n, unpackSnorm4x8(probe_words.data[radiance_dir]).xyz) > 0.5;
        if (kept != VELK_LIGHT_NONE && facing) return velk_rgb9e5(kept);
    }
    return velk_probe_sky(n);
}

// The direction of ray `ray` of probe `probe`, at `square` on the probe grid, in frame `frame`,
// around the normal `n`: over the hemisphere by cosine, in a cell of a 4 by 4 grid of directions
// (the squared sine to the normal one way, the turn about it the other). Each frame a probe's four
// rays take one band each way, each of the four rows and each of the four columns; over four
// frames a probe takes all sixteen cells, and every 2 by 2 block of probes takes all sixteen each
// frame. The place in the cell is the probe's own and moves every four frames.
vec3 velk_probe_ray_dir(uint probe, uvec2 square, uint ray, uint frame, vec3 n)
{
    uint s = (frame & 3u) ^ ((square.x & 1u) | ((square.y & 1u) << 1u));
    vec2 cell = vec2(float(ray), float(((ray & 1u) * 2u + (ray >> 1u)) ^ s));
    vec2 jitter = fract(vec2(float(probe) * 0.7548777, float(probe) * 0.5698403)
                        + float(frame >> 2u) * vec2(0.6180340, 0.3819660));
    vec2 uv = (cell + jitter) * 0.25;
    // By cosine: as many rays as the light they carry, few grazing.
    float z = sqrt(max(1.0 - uv.x, 0.0));
    float r = sqrt(uv.x);
    float phi = 6.2831853 * uv.y;
    vec3 a = normalize(abs(n.y) < 0.9 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(n, a);
    return normalize(a * (r * cos(phi)) + b * (r * sin(phi)) + n * z);
}

)" R"(
// The probe list's words (kProbeListHeaderWords, kProbeListEntryWords): how many probes cast rays,
// the trace's dispatch arguments (a group for each four), then each probe's: its index, with its
// cell's samples before (bits 24 to 30) and bit 31 when its rays go into its cell; and the cell.
#define VELK_PROBE_LIST_COUNT 0u
#define VELK_PROBE_LIST_ARGS 1u
#define VELK_PROBE_LIST_ENTRIES 4u
#define VELK_PROBE_LIST_ADD 0x80000000u

// A probe's surface, the middle pixel of its square at `square`: false where there is none.
bool velk_probe_place(uvec2 square, uint width, uint height, out vec3 pos, out vec3 n)
{
    ivec2 pixel = ivec2(int(min(square.x * VELK_PROBE_PIXELS + VELK_PROBE_PIXELS / 2u, width - 1u)),
                        int(min(square.y * VELK_PROBE_PIXELS + VELK_PROBE_PIXELS / 2u, height - 1u)));
    pos = texelFetch(velk_textures[nonuniformEXT(pc.extras.z)], pixel, 0).xyz;
    vec3 n4 = texelFetch(velk_textures[nonuniformEXT(pc.extras.w)], pixel, 0).xyz;
    n = dot(n4, n4) > 1e-6 ? normalize(n4) : vec3(0.0, 1.0, 0.0);
    return dot(n4, n4) > 1e-6;
}

// What each probe does this frame, a thread a probe, before the trace (velk_probes_trace): with
// no surface, nothing; its cell (pc.more.x the table, pc.more.y the frame everything last changed,
// pc.more.z this frame's change list) settled and nothing changed since, read without rays; else
// it adds its rays to it unless another does this frame, which leaves it to be read, or casts its
// rays alone while it has none yet. A change near it (velk_cell_changed), or everything changing,
// starts a settled cell moving again from VELK_CELL_WOKEN samples. The probes that cast rays go to
// the list at pc.more.w, so that the trace's groups are all of probes that do.
void velk_probes_decide(uint probe)
{
    uint width = pc.extras.y & 0xFFFFu;
    uint height = pc.extras.y >> 16u;
    uint across = (width + VELK_PROBE_PIXELS - 1u) / VELK_PROBE_PIXELS;
    uint down = (height + VELK_PROBE_PIXELS - 1u) / VELK_PROBE_PIXELS;
    if (probe >= across * down) return;
    uvec2 square = uvec2(probe % across, probe / across);
    uint raw = pc.extras.x + probe * VELK_PROBE_WORDS;
    vec3 pos;
    vec3 n;
    if (!velk_probe_place(square, width, height, pos, n)) {
        for (uint w = 0u; w < VELK_PROBE_WORDS; ++w) probe_words.data[raw + w] = 0u;
        return;
    }
    uint mode = VELK_CELL_OWN;
    uint cell = 0xFFFFFFFFu;
    uint had = 0u;
    if (pc.more.x != 0xFFFFFFFFu) {
        uint key;
        uint check;
        int level;
        velk_cell_key(pos, n, key, check, level);
        bool claimed;
        cell = velk_cell_find(pc.more.x, key, check, claimed);
        if (cell != 0xFFFFFFFFu) {
            uint now = VELK_GLOBALS.present_counter;
            probe_words.data[cell + VELK_CELL_READ] = now;
            uint samples = claimed ? 0u : probe_words.data[cell + VELK_CELL_SAMPLES];
            uint updated = probe_words.data[cell + VELK_CELL_UPDATED];
            bool woken = updated < pc.more.y || velk_cell_changed(pc.more.z, pos, level);
            had = woken ? min(samples, VELK_CELL_WOKEN) : samples;
            if (samples >= VELK_CELL_SETTLED && !woken) {
                mode = VELK_CELL_READ_ONLY;
            } else if (updated != now && atomicCompSwap(probe_words.data[cell + VELK_CELL_UPDATED], updated, now) == updated) {
                mode = VELK_CELL_ADD;
            } else if (samples > 0u) {
                mode = VELK_CELL_READ_ONLY;
            }
            if (woken && samples >= VELK_CELL_SETTLED && pc.shapes_base != 0xFFFFFFFFu) {
                atomicAdd(probe_words.data[pc.shapes_base + VELK_POOL_SUM_PROBES_READ + 3u], 1u);
            }
        }
    }
    if (pc.shapes_base != 0xFFFFFFFFu) {
        atomicAdd(probe_words.data[pc.shapes_base + VELK_POOL_SUM_PROBES_READ + (mode == VELK_CELL_READ_ONLY ? 0u : mode == VELK_CELL_ADD ? 1u : 2u)], 1u);
    }
    if (mode == VELK_CELL_READ_ONLY) {
        velk_probe_write(raw, velk_probe_read(cell + VELK_CELL_LIGHT), pos, velk_cell_normal(cell),
                         float(max(probe_words.data[cell + VELK_CELL_SAMPLES], 1u)));
        return;
    }
    uint list = pc.more.w & 0x7FFFFFFFu;
    uint slot = atomicAdd(probe_words.data[list + VELK_PROBE_LIST_COUNT], 1u);
    if (slot % 4u == 0u) atomicAdd(probe_words.data[list + VELK_PROBE_LIST_ARGS], 1u);
    uint e = list + VELK_PROBE_LIST_ENTRIES + slot * 2u;
    probe_words.data[e] = probe | (min(had, 127u) << 24u) | (mode == VELK_CELL_ADD ? VELK_PROBE_LIST_ADD : 0u);
    probe_words.data[e + 1u] = cell;
}

// The trace: four probes of the list a group of 16 threads, a ray a thread, each probe's light
// from its rays into the ray store, and into its cell where it adds to it.
void velk_probes_trace(uint lane)
{
    uint width = pc.extras.y & 0xFFFFu;
    uint height = pc.extras.y >> 16u;
    uint across = (width + VELK_PROBE_PIXELS - 1u) / VELK_PROBE_PIXELS;
    uint list = pc.more.w & 0x7FFFFFFFu;
    uint local = lane / VELK_PROBE_RAYS;
    uint ray = lane % VELK_PROBE_RAYS;
    uint slot = gl_WorkGroupID.x * 4u + local;
    bool live = slot < probe_words.data[list + VELK_PROBE_LIST_COUNT];
    uint entry = live ? probe_words.data[list + VELK_PROBE_LIST_ENTRIES + slot * 2u] : 0u;
    uint cell = live ? probe_words.data[list + VELK_PROBE_LIST_ENTRIES + slot * 2u + 1u] : 0xFFFFFFFFu;
    uint probe = entry & 0xFFFFFFu;
    uint had = (entry >> 24u) & 0x7Fu;
    bool add = (entry & VELK_PROBE_LIST_ADD) != 0u;
    uvec2 square = uvec2(probe % across, probe / across);
    uint raw = pc.extras.x + probe * VELK_PROBE_WORDS;
    GlobalData g = VELK_GLOBALS;
    vec3 pos = vec3(0.0);
    vec3 n = vec3(0.0, 1.0, 0.0);
    if (live) live = velk_probe_place(square, width, height, pos, n);

    if (lane == 0u) s_deepest = 0u;
    memoryBarrierShared();
    barrier();

    vec3 light = vec3(0.0);
    vec3 dir = n;
    if (live) {
        dir = velk_probe_ray_dir(probe, square, ray, g.present_counter, n);
        Ray rr;
        rr.origin = pos + n * max(length(pos) * 1e-3, 1e-3);
        rr.dir = dir;
        RayHit hit;
        if (trace_closest_hit(rr, 1e30, hit)) {
            vec3 at = rr.origin + dir * hit.t;
            vec3 hn = normalize(hit.normal);
            if (dot(hn, dir) > 0.0) hn = -hn;
            vec3 albedo;
            vec3 metal;
            vec3 emission;
            bool lit;
            velk_probe_surface(hit, albedo, metal, emission, lit);
            uint light_word;
            uint radiance;
            uint radiance_dir;
            vec3 center;
            bool ask;
            velk_probe_hit_words(hit, at, light_word, radiance, radiance_dir, center, ask);
            if (lit && ask && pc.shapes_base != 0xFFFFFFFFu) velk_probe_ask(light_word);
            // As the lighting pass lights a pixel: albedo times each light's N.L and strength, and
            // its indirect light.
            vec3 indirect = lit ? velk_probe_hit_indirect(pc.more.x, at, hn, radiance, radiance_dir, width, height, across) : vec3(0.0);
            light = albedo * (velk_probe_direct(hit, light_word, center, at, hn) + indirect) + emission;
        } else {
            light = velk_probe_sky(dir);
        }
    }
    s_light[lane] = light;
    s_dir[lane] = dir;
    // The deepest the walk's stack got, for the log (light pool header 101): the group's deepest,
    // written once.
    if (velk_ray_deepest > 0) atomicMax(s_deepest, uint(velk_ray_deepest));
    memoryBarrierShared();
    barrier();
    if (lane == 0u && pc.shapes_base != 0xFFFFFFFFu && s_deepest > 0u) {
        atomicMax(probe_words.data[pc.shapes_base + VELK_POOL_DEEPEST_PROBES], s_deepest);
    }
    if (!live || ray != 0u) return;

    // By cosine: the mean radiance weighted by cosine, as the lighting pass takes it, is the mean
    // of what the rays found. Each colour's direction: along the normal one and a half times
    // that, across it four times the mean of what a ray found times its direction's part across
    // the normal, which for light changing evenly over the hemisphere is its L1 direction.
    VelkProbeLight p;
    p.e = vec3(0.0);
    p.r = vec3(0.0);
    p.g = vec3(0.0);
    p.b = vec3(0.0);
    for (uint k = 0u; k < VELK_PROBE_RAYS; ++k) {
        vec3 l = s_light[lane + k];
        vec3 w = s_dir[lane + k];
        vec3 across_n = w - dot(w, n) * n;
        p.e += l;
        p.r += l.r * across_n;
        p.g += l.g * across_n;
        p.b += l.b * across_n;
    }
    p.e /= float(VELK_PROBE_RAYS);
    p.r = 1.5 * p.e.r * n + p.r * (4.0 / float(VELK_PROBE_RAYS));
    p.g = 1.5 * p.e.g * n + p.g * (4.0 / float(VELK_PROBE_RAYS));
    p.b = 1.5 * p.e.b * n + p.b * (4.0 / float(VELK_PROBE_RAYS));
    if (add) {
        // Into its cell, at the cell's normal: the mean of all the rays it holds, up to
        // VELK_CELL_SETTLED frames of them; its count written last, so that a probe reading it
        // finds the light it counts.
        vec3 cn = had == 0u ? n : velk_cell_normal(cell);
        p.e = velk_probe_eval_raw(p, n, cn);
        if (had > 0u) {
            VelkProbeLight c = velk_probe_read(cell + VELK_CELL_LIGHT);
            float w = 1.0 / float(had + 1u);
            p.e = mix(c.e, p.e, w);
            p.r = mix(c.r, p.r, w);
            p.g = mix(c.g, p.g, w);
            p.b = mix(c.b, p.b, w);
        } else {
            probe_words.data[cell + VELK_CELL_NORMAL] = velk_probe_oct_encode(cn);
        }
        velk_probe_write_light(cell + VELK_CELL_LIGHT, p);
        memoryBarrierBuffer();
        uint samples = min(had + 1u, VELK_CELL_SETTLED);
        probe_words.data[cell + VELK_CELL_SAMPLES] = samples;
        velk_probe_write(raw, p, pos, cn, float(samples));
        return;
    }
    velk_probe_write(raw, p, pos, n, 1.0);
}

)" R"(
#ifdef VELK_REFLECT
// Roughness from which a surface reflects nothing traced (the lighting's own lobe instead), and
// below which its reflection is a mirror's.
#define VELK_REFLECT_CUTOFF 0.3
#define VELK_REFLECT_SHARP 0.05

// Reflections, as emit_reflection_pass runs this shader with VELK_REFLECT: a thread a 2 by 2
// block of pixels, a group 8 by 4 blocks, and each frame one pixel of each block, the next of
// the four the next frame. A lit pixel smoother than VELK_REFLECT_CUTOFF sends a ray along its
// reflection, the mirror one below VELK_REFLECT_SHARP, else one of its GGX lobe. What the ray
// hits is lit as a probe's ray finds it (velk_probe_surface, velk_probe_direct,
// velk_probe_hit_indirect), and its metal reflects the environment; a miss is the environment,
// as sharp as the reflection is. Into the samples image (pc.extras.x), a texel a block, every one
// written each frame: the light, and how far the ray went, -1 for a miss, 0 for no reflection.
// pc.extras.y is the G-buffer's size (x low, y high), z its positions, w its normals, pc.more.x
// its material parameters.
void velk_reflect_trace(uint lane)
{
    uint width = pc.extras.y & 0xFFFFu;
    uint height = pc.extras.y >> 16u;
    uint across = (width + VELK_PROBE_PIXELS - 1u) / VELK_PROBE_PIXELS;
    uvec2 blocks = uvec2((width + 1u) / 2u, (height + 1u) / 2u);
    uint groups_across = (blocks.x + 7u) / 8u;
    uvec2 block = uvec2((gl_WorkGroupID.x % groups_across) * 8u + lane % 8u, (gl_WorkGroupID.x / groups_across) * 4u + lane / 8u);
    if (block.x >= blocks.x || block.y >= blocks.y) return;
    GlobalData g = VELK_GLOBALS;
    uint k = g.present_counter & 3u;
    ivec2 pixel = min(ivec2(block * 2u + uvec2(k & 1u, k >> 1u)), ivec2(int(width) - 1, int(height) - 1));
    vec4 mat = texelFetch(velk_textures[nonuniformEXT(pc.more.x)], pixel, 0);
    float roughness = clamp(mat.g, 0.04, 1.0);
    uint mode = uint(mat.b * 255.0 + 0.5);
    vec3 n4 = texelFetch(velk_textures[nonuniformEXT(pc.extras.w)], pixel, 0).xyz;
    if (mode == 0u || dot(n4, n4) < 1e-6 || roughness >= VELK_REFLECT_CUTOFF) {
        imageStore(gStorageImagesF16[nonuniformEXT(pc.extras.x)], ivec2(block), vec4(0.0));
        return;
    }
    vec3 pos = texelFetch(velk_textures[nonuniformEXT(pc.extras.z)], pixel, 0).xyz;
    vec3 n = normalize(n4);
    vec3 v = -normalize(pos);
    vec3 dir = reflect(-v, n);
    if (roughness >= VELK_REFLECT_SHARP) {
        rng_init(uvec2(pixel), g.present_counter);
        vec3 sampled = ggx_sample_reflect(v, n, roughness, rng_next_vec2());
        if (dot(sampled, n) > 0.0) dir = sampled;
    }
    Ray rr;
    rr.origin = pos + n * max(length(pos) * 1e-3, 1e-3);
    rr.dir = dir;
    RayHit hit;
    vec3 light = vec3(0.0);
    float t = -1.0;
    if (trace_closest_hit(rr, 1e30, hit)) {
        vec3 at = rr.origin + dir * hit.t;
        vec3 hn = normalize(hit.normal);
        if (dot(hn, dir) > 0.0) hn = -hn;
        vec3 albedo;
        vec3 metal;
        vec3 emission;
        bool lit;
        velk_probe_surface(hit, albedo, metal, emission, lit);
        uint light_word;
        uint radiance;
        uint radiance_dir;
        vec3 center;
        bool ask;
        velk_probe_hit_words(hit, at, light_word, radiance, radiance_dir, center, ask);
        if (lit && ask && pc.shapes_base != 0xFFFFFFFFu) velk_probe_ask(light_word);
        vec3 indirect = lit ? velk_probe_hit_indirect(pc.more.w, at, hn, radiance, radiance_dir, width, height, across) : vec3(0.0);
        // Its metal: what it mirrors of the environment, half blurred, its own roughness not known.
        light = albedo * (velk_probe_direct(hit, light_word, center, at, hn) + indirect) + emission
              + metal * velk_probe_env(reflect(dir, hn), 0.5);
        // Within what half floats hold.
        t = clamp(hit.t, 1e-4, 65000.0);
    } else {
        // As the lighting blurs the environment for a lobe; a sampled ray is one of the lobe already.
        light = velk_probe_env(dir, roughness < VELK_REFLECT_SHARP ? roughness : 0.0);
    }
    imageStore(gStorageImagesF16[nonuniformEXT(pc.extras.x)], ivec2(block), vec4(min(light, vec3(65000.0)), t));
}
#endif

void main()
{
#ifdef VELK_REFLECT
    velk_reflect_trace(gl_LocalInvocationIndex);
#else
    if ((pc.more.w & 0x80000000u) != 0u) {
        velk_probes_trace(gl_LocalInvocationIndex);
    } else {
        velk_probes_decide(gl_WorkGroupID.x * gl_WorkGroupSize.x + gl_LocalInvocationIndex);
    }
#endif
}
)";

/**
 * The reflections' resolve, after their trace (light_probes_main_src with VELK_REFLECT): a thread a
 * pixel, 8 by 8 a group. The trace left a sample a 2 by 2 block (pc.extras.x), from the block's
 * pixel of this frame. A pixel that reflects finds the last frame's result where its reflection
 * was then: through its image along the view ray, behind the surface by how far the ray went, or
 * for a miss the view ray's direction; else where the surface was. A mirror takes its own sample
 * where it is the pixel traced, else that last result, else its block's sample. A glossy surface
 * takes the samples of the 3 by 3 blocks around, by how much the surfaces they were traced from
 * agree with its own, blended with the last result kept to their spread. Into this frame's result
 * (pc.more.z): the light, and how many frames it holds, 0 for no reflection. Traces nothing:
 * composed after rt_compute_prelude_src alone. pc.extras.y is the G-buffer's size (x low, y
 * high), z its positions, w its normals; pc.more.x its material parameters, y the last frame's
 * result, w not 0 when there is none; pc.env.w the most frames a glossy result blends
 * (kReflectMovingFrames, kReflectSettledFrames).
 */
inline constexpr string_view reflect_resolve_src = R"(
#define VELK_REFLECT_CUTOFF 0.3
#define VELK_REFLECT_SHARP 0.05

// The last frame's result where the place `at` (relative to the camera, or with `w` 0 a direction)
// was on screen; no frames off screen.
vec4 velk_reflect_last(vec3 at, float w, uint width, uint height)
{
    GlobalData g = VELK_GLOBALS;
    vec4 clip = g.prev_view_projection * vec4(w > 0.5 ? at + g.cam_moved : at, w);
    if (clip.w <= 1e-5) return vec4(0.0);
    vec2 p = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(width), float(height));
    if (any(lessThan(p, vec2(0.0))) || p.x >= float(width) || p.y >= float(height)) return vec4(0.0);
    return texelFetch(velk_textures[nonuniformEXT(pc.more.y)], ivec2(p), 0);
}

void main()
{
    uint width = pc.extras.y & 0xFFFFu;
    uint height = pc.extras.y >> 16u;
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(width) || coord.y >= int(height)) return;
    vec4 mat = texelFetch(velk_textures[nonuniformEXT(pc.more.x)], coord, 0);
    float roughness = clamp(mat.g, 0.04, 1.0);
    vec3 n4 = texelFetch(velk_textures[nonuniformEXT(pc.extras.w)], coord, 0).xyz;
    if (uint(mat.b * 255.0 + 0.5) == 0u || dot(n4, n4) < 1e-6 || roughness >= VELK_REFLECT_CUTOFF) {
        imageStore(gStorageImagesF16[nonuniformEXT(pc.more.z)], coord, vec4(0.0));
        return;
    }
    vec3 n = normalize(n4);
    vec3 pos = texelFetch(velk_textures[nonuniformEXT(pc.extras.z)], coord, 0).xyz;
    ivec2 last = ivec2(int(width) - 1, int(height) - 1);
    ivec2 blocks_last = ivec2(int((width + 1u) / 2u) - 1, int((height + 1u) / 2u) - 1);
    uint k = VELK_GLOBALS.present_counter & 3u;
    ivec2 phase = ivec2(int(k & 1u), int(k >> 1u));
    ivec2 own_block = coord / 2;
    bool sharp = roughness < VELK_REFLECT_SHARP;
    // This frame's samples: the own block's for a mirror, the 3 by 3 blocks' for a glossy surface.
    int reach = sharp ? 0 : 1;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    vec3 lo = vec3(1e30);
    vec3 hi = vec3(-1e30);
    vec4 own = vec4(0.0);
    float t_any = 0.0;
    for (int dy = -reach; dy <= reach; ++dy) {
        for (int dx = -reach; dx <= reach; ++dx) {
            ivec2 b = clamp(own_block + ivec2(dx, dy), ivec2(0), blocks_last);
            vec4 s = texelFetch(velk_textures[nonuniformEXT(pc.extras.x)], b, 0);
            if (s.a == 0.0) continue;
            ivec2 q = min(b * 2 + phase, last);
            vec3 qn = texelFetch(velk_textures[nonuniformEXT(pc.extras.w)], q, 0).xyz;
            if (dot(qn, qn) < 1e-6) continue;
            vec3 qp = texelFetch(velk_textures[nonuniformEXT(pc.extras.z)], q, 0).xyz;
            float w = pow(max(dot(normalize(qn), n), 0.0), 8.0) * exp(-abs(dot(n, qp - pos)) / (0.02 * length(pos) + 1e-3));
            if (w <= 1e-4) continue;
            sum += w * s.rgb;
            total += w;
            lo = min(lo, s.rgb);
            hi = max(hi, s.rgb);
            if (dx == 0 && dy == 0) own = s;
            if (t_any == 0.0) t_any = s.a;
        }
    }
    // The last result, through the reflection's image, else where the surface was.
    float t = own.a != 0.0 ? own.a : t_any;
    vec4 was = vec4(0.0);
    if (pc.more.w == 0u) {
        vec3 view = normalize(pos);
        if (t != 0.0) was = t > 0.0 ? velk_reflect_last(pos + view * t, 1.0, width, height) : velk_reflect_last(view, 0.0, width, height);
        if (was.a <= 0.0) was = velk_reflect_last(pos, 1.0, width, height);
    }
    if (sharp) {
        bool traced = coord == min(own_block * 2 + phase, last);
        vec3 result = vec3(0.0);
        float frames = 1.0;
        if (traced && own.a != 0.0) result = own.rgb;
        else if (was.a > 0.0) result = was.rgb;
        else if (total > 0.0) result = sum / total;
        else frames = 0.0;
        imageStore(gStorageImagesF16[nonuniformEXT(pc.more.z)], coord, vec4(result, frames));
        return;
    }
    if (total <= 0.0) {
        imageStore(gStorageImagesF16[nonuniformEXT(pc.more.z)], coord, was.a > 0.0 ? was : vec4(0.0));
        return;
    }
    vec3 now = sum / total;
    vec3 history = now;
    float frames = 0.0;
    if (was.a > 0.0) {
        vec3 spread = (hi - lo) * 0.25 + vec3(0.02);
        history = clamp(was.rgb, lo - spread, hi + spread);
        frames = was.a;
    }
    frames = min(frames + 1.0, float(pc.env.w));
    imageStore(gStorageImagesF16[nonuniformEXT(pc.more.z)], coord, vec4(mix(history, now, 1.0 / frames), frames));
}
)";

/**
 * The screen probes' filter, after the gather (light_probes_main_src): a thread a probe. Its light
 * is the mean of its own and its 3 by 3 neighbours' light, each by how near its surface is to the
 * probe's plane and the probe to its, and how much their normals agree. What a probe brings over
 * frames is its cell's (VELK_PROBE_CELL_GLSL), which keeps it in the world while the camera
 * moves. With pc.extras.w 1 it instead lets go of cells no probe read for VELK_CELL_KEEP frames, a
 * thread a slot, after the filter, so that the next gather only takes free slots. Traces nothing:
 * composed after rt_compute_prelude_src alone. pc.extras.x is the ray store, pc.extras.y the
 * G-buffer's size (x low, y high), pc.extras.z this frame's probe store, pc.env.w the cells,
 * pc.more.w the gather's probe list, which it empties.
 */
inline constexpr string_view light_probes_filter_src = R"(
layout(set = 1, binding = 16, std430) buffer ProbeWords { uint data[]; } probe_words;
#define VELK_LIGHT_WORDS probe_words
#define VELK_PROBE_PIXELS 16u
)" VELK_PROBE_GLSL VELK_PROBE_WRITE_GLSL VELK_PROBE_CELL_GLSL R"(
// The filter: a thread a probe, from the ray store into this frame's.
void velk_probes_filter()
{
    uint width = pc.extras.y & 0xFFFFu;
    uint height = pc.extras.y >> 16u;
    uint across = (width + VELK_PROBE_PIXELS - 1u) / VELK_PROBE_PIXELS;
    uint down = (height + VELK_PROBE_PIXELS - 1u) / VELK_PROBE_PIXELS;
    uint probe = gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex;
    if (probe >= across * down) return;
    // The probe list at pc.more.w empty again for the next frame's gather: its count and its
    // trace's groups.
    if (probe == 0u && pc.more.w != 0xFFFFFFFFu) {
        probe_words.data[pc.more.w] = 0u;
        probe_words.data[pc.more.w + 1u] = 0u;
    }
    uint raw_store = pc.extras.x;
    uint mine = raw_store + probe * VELK_PROBE_WORDS;
    uint now = pc.extras.z + probe * VELK_PROBE_WORDS;
    float frames = uintBitsToFloat(probe_words.data[mine + VELK_PROBE_FRAMES]);
    if (frames <= 0.0) {
        for (uint w = 0u; w < VELK_PROBE_WORDS; ++w) probe_words.data[now + w] = 0u;
        return;
    }
    vec3 pos = velk_probe_position(mine);
    vec3 n = velk_probe_normal(mine);
    float near = 0.02 * length(pos) + 1e-3;

    // Its own and its 3 by 3 neighbours' light, each by how near the two surfaces are to each
    // other's planes and how much their normals agree.
    ivec2 cell = ivec2(int(probe % across), int(probe / across));
    VelkProbeLight light;
    light.e = vec3(0.0);
    light.r = vec3(0.0);
    light.g = vec3(0.0);
    light.b = vec3(0.0);
    float total = 0.0;
    for (int k = 0; k < 9; ++k) {
        ivec2 c = cell + ivec2(k % 3 - 1, k / 3 - 1);
        if (c.x < 0 || c.y < 0 || c.x >= int(across) || c.y >= int(down)) continue;
        uint at = raw_store + (uint(c.y) * across + uint(c.x)) * VELK_PROBE_WORDS;
        if (uintBitsToFloat(probe_words.data[at + VELK_PROBE_FRAMES]) <= 0.0) continue;
        vec3 at_pos = velk_probe_position(at);
        vec3 at_n = velk_probe_normal(at);
        float off = max(abs(dot(n, at_pos - pos)), abs(dot(at_n, pos - at_pos)));
        float w = exp(-off / near) * pow(max(dot(n, at_n), 0.0), 4.0);
        if (w <= 1e-4) continue;
        VelkProbeLight q = velk_probe_read(at);
        light.e += w * velk_probe_eval_raw(q, at_n, n);
        light.r += w * q.r;
        light.g += w * q.g;
        light.b += w * q.b;
        total += w;
    }
    // Its own probe is always among them, with weight 1.
    light.e = max(light.e / total, vec3(0.0));
    light.r /= total;
    light.g /= total;
    light.b /= total;
    velk_probe_write(now, light, pos, n, frames);
}

// Lets go of the cell in slot `slot` when no probe read it for VELK_CELL_KEEP frames.
void velk_probe_cells_age(uint slot)
{
    if (slot >= VELK_CELL_SLOTS || pc.env.w == 0xFFFFFFFFu) return;
    uint cell = pc.env.w + slot * VELK_CELL_WORDS;
    if (probe_words.data[cell + VELK_CELL_KEY] == 0u) return;
    uint now = VELK_GLOBALS.present_counter;
    if (now - probe_words.data[cell + VELK_CELL_READ] <= VELK_CELL_KEEP) return;
    probe_words.data[cell + VELK_CELL_SAMPLES] = 0u;
    probe_words.data[cell + VELK_CELL_UPDATED] = 0u;
    probe_words.data[cell + VELK_CELL_CHECK] = 0u;
    probe_words.data[cell + VELK_CELL_KEY] = 0u;
}

void main()
{
    if (pc.extras.w == 1u) {
        velk_probe_cells_age(gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex);
        return;
    }
    velk_probes_filter();
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
// The claim's own code, shared by its pass over the visibility buffer's pixels (light_claim_src)
// and its pass over blended surfaces' fragments (light_claim_blended_src). Who includes it has
// `light_words` writable as VELK_LIGHT_WORDS, push constants `pc` with pool_word, light_pixels,
// lights and again, and says before it where the pixel is in its triangle (velk_claim_weights),
// how long its triangle's longest edge is on screen (velk_claim_tri_px) and its instance's world
// (velk_claim_world).
#define VELK_LIGHT_CLAIM_GLSL R"(
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
// times `light_pixels` apart, in a root `tri_px` pixels across whose pieces' grids are of level
// `grid`; `from` itself when none deeper is.
uint velk_piece_coarse_depth(float tri_px, uint grid, float light_pixels, uint from)
{
    float over = tri_px / (float(grid) * VELK_LIGHT_COARSE_PIECES * light_pixels);
    uint deepest = over >= 2.0 ? uint(min(floor(log2(over)), float(VELK_PIECE_DEEPEST))) : 0u;
    return max(from, deepest);
}

// The piece starting at word `at` read in the pool's frame `now`, once a frame.
void velk_light_piece_read(uint at, uint now)
{
    uint read = light_words.data[at + VELK_PIECE_READ];
    if ((read & VELK_PIECE_FRAME_BITS) != now) {
        atomicCompSwap(light_words.data[at + VELK_PIECE_READ], read, now | (read & VELK_PIECE_SAME_BITS));
    }
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
// longest edge on screen where the pixel is. Its root is `r` (velk_light_root_of), its triangle's
// or its flat patch's, `root_px` pixels across.
void velk_light_list_way(uint first, uvec3 corners, VelkLightRoot r, vec3 b, float tri_px, float root_px, uint instance,
                         uint frame)
{
    uint pool = pc.pool_word;
    uint pages = light_words.data[pool + VELK_POOL_PAGES];
    uint root_word = velk_light_root_word(pool, first, r.slot);
    uint root = light_words.data[root_word];
    VelkPieceWay way = velk_piece_way(pool, root, b, tri_px, r, root_px, pc.light_pixels);
    // Where its coarse cell is exact, or not known yet, it goes no further down: judged by its
    // points' light even when a change made them stale, so that the finer points an edge needs
    // are traced while it moves; the lighting judges it again with what is traced.
    uint cell = velk_way_cell(pool, first, corners, way, false);
    uint deepest = way.piece;
    vec3 deepest_at = way.at;
    // Exact only by points a change made stale: the trace this frame may find them differing, and
    // the lighting then reads the level under them, so that level's points are listed too where it
    // is there already.
    bool stale_exact = cell == 1u && deepest != way.coarse && deepest != VELK_PIECE_VERTICES
                    && velk_way_cell(pool, first, corners, way, true) == 0u;
    if (cell < 2u) velk_way_stop_at_coarse(way);
    bool on_piece = way.piece != VELK_PIECE_VERTICES;
    uint piece_first = on_piece ? velk_light_page(pool, way.piece) : 0u;
    uint now = light_words.data[pool + VELK_POOL_FRAME] & VELK_PIECE_FRAME_BITS;
    if (on_piece) velk_light_piece_read(piece_first, now);
    // The pieces under a coarse cell it stopped at stay while it is in view, which bounds them,
    // and the pool is not short (an eighth free, where it grows): a shadow's edge sweeping back
    // and forth (a light swaying) finds them, and their light, instead of making them again from
    // none. Short of room, they go as unread.
    if (deepest != way.piece && deepest != VELK_PIECE_VERTICES
        && light_words.data[pool + VELK_POOL_FREE_PAGES] * 8u > light_words.data[pool + VELK_POOL_PAGES]) {
        velk_light_piece_read(velk_light_page(pool, deepest), now);
    }

    // The vertex points: its coarse level when it is them, else read only by bounces.
    for (uint k = 0u; k < 3u; ++k) {
        velk_light_list_point(pool, first, velk_way_vertex(first, corners[k]), instance, frame,
                              way.coarse == VELK_PIECE_VERTICES);
    }
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
    if (stale_exact && deepest != way.piece && deepest != VELK_PIECE_VERTICES) {
        uint deepest_first = velk_light_page(pool, deepest);
        velk_light_piece_read(deepest_first, now);
        uint under_words[3];
        vec3 under_w;
        velk_way_around(pool, first, corners, deepest, deepest_at, under_words, under_w);
        for (uint k = 0u; k < 3u; ++k) {
            if (under_w[k] > 0.0) {
                velk_light_list_piece_point(pool, deepest, (under_words[k] - deepest_first) / 2u, first, instance, frame, false);
            }
        }
    }

    if (pc.again != 0u) return;
    if (way.spacing <= pc.light_pixels) return;
    // Nothing under it yet: a piece without children, or the vertex points without a root.
    if (on_piece && (way.depth >= VELK_PIECE_DEEPEST || light_words.data[piece_first + VELK_PIECE_CHILDREN] < pages)) return;
    if (!on_piece && (root != VELK_LIGHT_NONE || tri_px <= 2.0 * pc.light_pixels)) return;
    uint level = on_piece ? way.root_level : r.is_patch ? VELK_PATCH_LEVEL : velk_piece_root_level(tri_px, pc.light_pixels);
    uint next = on_piece ? way.depth + 1u : 0u;
    bool edge = velk_piece_spacing(r.is_patch, next, level, root_px) < VELK_LIGHT_COARSE_PIECES * pc.light_pixels;
    uint depth = next;
    if (!edge) {
        uint last = on_piece ? way.depth + VELK_PIECE_LEVELS_AT_ONCE : VELK_PIECE_LEVELS_AT_ONCE;
        uint grid = r.is_patch ? VELK_PATCH_LEVEL : VELK_PIECE_LEVEL;
        depth = min(velk_piece_coarse_depth(root_px, grid, pc.light_pixels, next), min(last, VELK_PIECE_DEEPEST));
    } else {
        uint s0 = light_words.data[words[0]];
        uint s1 = light_words.data[words[1]];
        uint s2 = light_words.data[words[2]];
        if (s0 == VELK_LIGHT_NONE || s1 == VELK_LIGHT_NONE || s2 == VELK_LIGHT_NONE) return;
        // Every light's four bits all clear or all set, the same at all three.
        if (s0 == s1 && s1 == s2 && s0 == (s0 & 0x11111111u) * 15u) return;
    }
    // The pixel's way down the levels made at once: which child it is in at each, 2 bits a level
    // (VELK_SPLIT_PATH_SHIFT), so that only the pieces along it are split further.
    vec3 l = on_piece ? way.at : r.at;
    uint path = 0u;
    for (uint k = 0u; k + 1u < VELK_PIECE_LEVELS_AT_ONCE; ++k) path |= velk_way_step(r.is_patch, l) << (2u * k);
    path <<= VELK_SPLIT_PATH_SHIFT;
    if (edge) path |= VELK_LIGHT_SPLIT_EDGE;
    if (on_piece) {
        uint before = atomicOr(light_words.data[piece_first + VELK_PIECE_READ], VELK_PIECE_ASKED);
        if ((before & VELK_PIECE_ASKED) == 0u) velk_light_split(way.piece, (depth << 8u) | path);
    } else if (atomicCompSwap(light_words.data[root_word], VELK_LIGHT_NONE, VELK_LIGHT_PENDING) == VELK_LIGHT_NONE) {
        if (!velk_light_split(light_words.data[first + VELK_PAGE_FIRST], VELK_LIGHT_SPLIT_ROOT | r.slot | (depth << 8u) | (level << 16u) | path)) {
            light_words.data[root_word] = VELK_LIGHT_NONE;
        }
    }
}

)" R"(
// Lists the points of light the pixel with ids `ids`, of draw `d`, reads from the record whose
// first page starts at `first`, those it reads with any weight, when they are due
// (velk_light_list_point). Where the pixel is in its triangle, and its triangle's size, are as
// its lighting finds them too (velk_claim_weights, velk_claim_tri_px), so that both read the same
// points.
void velk_light_list_pixel(uint first, VelkDrawData d, uvec2 ids, uint instance, uint frame, GlobalData g)
{
    uint cluster = velk_ids_cluster(d, ids);
    if (cluster == VELK_NO_CLUSTER) return;
    uint triangle = velk_ids_triangle(d, ids);
    uint word = d.index_base + velk_cluster_first_index(cluster) + triangle * 3u;
    uvec3 corners = uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
    vec3 b = velk_claim_weights();
    // In pieces: the pixel's way down its triangle's levels, from its vertex points.
    uint pool = pc.pool_word;
    VelkLightRoot r = velk_light_root_of(first, triangle, corners, b, velk_claim_tri_size(), mat3(velk_claim_world(d, ids)));
    if (r.slot != VELK_LIGHT_NONE) {
        float px_per_world = velk_claim_px_per_world(g);
        float tri_px = velk_claim_tri_size() * px_per_world;
        g_listed_by = tri_px < 8.0 ? VELK_LIGHT_LISTED_SMALL : tri_px < 64.0 ? VELK_LIGHT_LISTED_MID : VELK_LIGHT_LISTED_LARGE;
        velk_light_list_way(first, corners, r, b, tri_px, r.size * px_per_world, instance, frame);
        return;
    }
    g_listed_by = VELK_LIGHT_LISTED_VERTICES;
    // A point per vertex, its own or those it links to: a vertex's point is coarse.
    uint words[9];
    uint points[9];
    float weights[9];
    uint owners[9];
    uint read = velk_light_pixel_points(pool, first, corners, b, words, points, weights, owners);
    for (uint k = 0u; k < read; ++k) {
        if (weights[k] > 0.0) {
            velk_light_list_point(pc.pool_word, owners[k], points[k], instance, frame, true);
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

// After the search, the claim again only when it laid records out or made pieces this frame.
bool velk_light_claim_idle()
{
    return pc.again != 0u && light_words.data[pc.pool_word + VELK_POOL_SEARCHED] == 0u
        && light_words.data[pc.pool_word + VELK_POOL_SPLITS] == 0u;
}

// Gives the record `record` that the pixel with ids `ids`, of draw `d`, shows its light, and
// lists the points it reads.
void velk_light_claim(VelkDrawData d, uvec2 ids, uint record)
{
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
    // A whole mesh or a shape drawn by its own pass: one value, traced from the mesh's sphere, or
    // a shape without one from its instance's box.
    if (entry == 0xFFFFFFFFu) {
        if (was == VELK_LIGHT_NONE) {
            atomicCompSwap(light_words.data[record + VELK_RECORD_LIGHT], VELK_LIGHT_NONE, VELK_LIGHT_ONE_VALUE);
        } else {
            velk_light_list_one(record + VELK_RECORD_LIGHT, VELK_LIGHT_TRACE_MESH, record, instance,
                                light_words.data[record + VELK_RECORD_MESH_TABLE], frame);
        }
        return;
    }
    uint clusters = light_words.data[entry + VELK_CLUSTER_ENTRY_TABLE];
    // A cluster: as wide on screen as it is now, and its largest triangle.
    uint cluster = clusters + light_words.data[record + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS;
    uint vertices = light_words.data[cluster + VELK_LOD_CLUSTER_VERTICES];
    mat4 world = velk_claim_world(d, ids);
    float scale = max(max(length(world[0].xyz), length(world[1].xyz)), length(world[2].xyz));
    vec3 own = vec3(uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE]),
                    uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 1u]),
                    uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 2u]));
    float radius = uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale;
    float away = length(mat3(world) * own + (world[3].xyz - g.cam_pos.xyz));
    float across = 2.0 * radius * pixels_per_unit / max(away, 1e-3);

    // Finer detail where a record of coarser detail had points (kLightInherit): its own laid out
    // from those (light_search_src), or a one value when it is small.
    if (velk_light_inherits(was)) {
        if (pc.again != 0u) return;
        if (atomicCompSwap(light_words.data[record + VELK_RECORD_LIGHT], was, VELK_LIGHT_PENDING) != was) return;
        if (vertices == 0u || across < pc.light_pixels) {
            light_words.data[record + VELK_RECORD_LIGHT] = VELK_LIGHT_ONE_VALUE;
            return;
        }
        velk_light_search(record, was, instance);
        return;
    }
    // Pages are laid out once; pieces follow how near the camera is. The points this pixel reads
    // are traced when due, and the coarse light of the records they are in: its own, or those of
    // the records for light only it links to at this pixel's corners.
    if (was != VELK_LIGHT_NONE && was < VELK_LIGHT_ONE_VALUE) {
        if (was >= light_words.data[pc.pool_word + VELK_POOL_PAGES]) return;
        uint first = velk_light_page(pc.pool_word, was);
        if (light_words.data[first + VELK_PAGE_KIND] != VELK_LIGHT_LINKED) {
            velk_light_list_record(was, instance, frame);
            velk_light_list_pixel(first, d, ids, instance, frame, g);
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
        velk_light_list_pixel(first, d, ids, instance, frame, g);
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
)"

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

uvec2 g_coord;  // the thread's pixel

// As the resolve left them for the lighting: where the pixel is in its triangle, and from the
// G-buffer's positions, its distance and its triangle's longest edge.
vec3 velk_claim_weights() { return velk_pixel_weights(pc.ids_word, pc.width, ivec2(g_coord)); }

// Its triangle's longest edge in world units, and the pixels a world unit is where it is.
float velk_claim_tri_size()
{
    return texelFetch(velk_textures[nonuniformEXT(pc.position_id)], ivec2(g_coord), 0).w;
}

float velk_claim_px_per_world(GlobalData g)
{
    vec4 position = texelFetch(velk_textures[nonuniformEXT(pc.position_id)], ivec2(g_coord), 0);
    mat4 vp = g.view_projection;
    return length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5 / max(length(position.xyz), 1e-4);
}

float velk_claim_tri_px(GlobalData g)
{
    return velk_claim_tri_size() * velk_claim_px_per_world(g);
}

mat4 velk_claim_world(VelkDrawData d, uvec2 ids)
{
    LightInstance li = light_instances.data[d.instances_base + velk_ids_slot(d, ids)];
    return velk_sized_world(li.world_matrix, li.offset.xyz, li.size.xyz);
}
)" VELK_LIGHT_POOL_GLSL VELK_LIGHT_LIST_GLSL VELK_LIGHT_CLAIM_GLSL R"(
void main()
{
    uvec2 coord = gl_GlobalInvocationID.xy;
    // The scene's lights, for what the next frame's mark lists (light_mark_src).
    if (coord.x == 0u && coord.y == 0u && pc.again == 0u) light_words.data[pc.pool_word + VELK_POOL_LIGHTS] = pc.lights;
    if (coord.x >= pc.width || coord.y >= pc.height) return;
    if (velk_light_claim_idle()) return;
    g_coord = coord;
    uvec2 ids = velk_pixel_ids(pc.ids_word, pc.width, ivec2(coord));
    if (ids.x == 0u) return;
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint record = velk_ids_record(d, ids);
    if (record == VELK_NO_RECORD) return;
    velk_light_claim(d, ids, record);
}
)";

// Where a fragment of a draw by cluster is in its triangle, for shaders over blended surfaces'
// fragments, which the visibility buffer does not hold: from its triangle's corners, from the
// camera as its vertices are (velk_default_varyings). Needs velk-ui.glsl.
#define VELK_FRAGMENT_TRIANGLE_GLSL R"(
// For the fragment at `rel_pos` with ids `ids` (as VELK_WRITE_VISIBILITY writes them) of draw
// `d`: where it is in its triangle, as weights of its corners, and its triangle's longest edge,
// in world units. A third each and no edge when it is not of a cluster.
void velk_fragment_triangle(VelkDrawData d, uvec2 ids, vec3 rel_pos, out vec3 weights, out float longest)
{
    weights = vec3(1.0 / 3.0);
    longest = 0.0;
    uint cluster = velk_ids_cluster(d, ids);
    if (cluster == VELK_NO_CLUSTER) return;
    uint word = d.index_base + velk_cluster_first_index(cluster) + velk_ids_triangle(d, ids) * 3u;
    uint base = velk_cluster_vertex_word(d, cluster);
    ElementInstance inst = velk_draw_instance(d, velk_ids_slot(d, ids));
    GlobalData g = velk_globals.data[d.globals_base];
    vec3 p0 = velk_rel_position(inst.world_matrix, inst.offset.xyz
        + velk_unpack_vertex3d(base + velk_mesh_words.data[word] * VELK_VERTEX3D_WORDS).position * inst.size.xyz, g);
    vec3 p1 = velk_rel_position(inst.world_matrix, inst.offset.xyz
        + velk_unpack_vertex3d(base + velk_mesh_words.data[word + 1u] * VELK_VERTEX3D_WORDS).position * inst.size.xyz, g);
    vec3 p2 = velk_rel_position(inst.world_matrix, inst.offset.xyz
        + velk_unpack_vertex3d(base + velk_mesh_words.data[word + 2u] * VELK_VERTEX3D_WORDS).position * inst.size.xyz, g);
    vec3 e0 = p1 - p0;
    vec3 e1 = p2 - p0;
    vec3 e2 = rel_pos - p0;
    float d00 = dot(e0, e0);
    float d01 = dot(e0, e1);
    float d11 = dot(e1, e1);
    float d20 = dot(e2, e0);
    float d21 = dot(e2, e1);
    float denom = d00 * d11 - d01 * d01;
    if (denom > 0.0) {
        float v = (d11 * d20 - d01 * d21) / denom;
        float w = (d00 * d21 - d01 * d20) / denom;
        vec3 b = max(vec3(1.0 - v - w, v, w), vec3(0.0));
        weights = b / max(b.x + b.y + b.z, 1e-6);
    }
    longest = sqrt(max(max(d00, d11), dot(p2 - p1, p2 - p1)));
}
)"

/**
 * The claim (light_claim_src) for the fragments of blended surfaces, which the visibility buffer
 * does not hold: a raster pass over their clusters' draws against the visibility buffer's depth,
 * with no colour, whose fragment shader this is, before the claim's search and again after it.
 * A fragment's ids are those the visibility pass would write (VELK_WRITE_VISIBILITY); where it is
 * in its triangle comes from its triangle's corners. Its push constants are the draw's, then the
 * claim's.
 */
inline constexpr string_view light_claim_blended_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(early_fragment_tests) in;

VELK_VARYINGS_IN

layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant, std430) uniform VelkPC {
    uint draw_base;
    uint pool_word;
    float light_pixels;
    uint lights;
    uint again;
} pc;

#define VELK_LIGHT_WORDS light_words

uvec2 g_ids;       // the fragment's, as the visibility buffer would hold them
vec3 g_weights;    // where it is in its triangle
float g_longest;   // its triangle's longest edge, in world units
)" VELK_FRAGMENT_TRIANGLE_GLSL R"(
vec3 velk_claim_weights() { return g_weights; }

float velk_claim_tri_size()
{
    return g_longest;
}

float velk_claim_px_per_world(GlobalData g)
{
    mat4 vp = g.view_projection;
    return length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5 / max(length(v_rel_pos), 1e-4);
}

float velk_claim_tri_px(GlobalData g)
{
    return velk_claim_tri_size() * velk_claim_px_per_world(g);
}

mat4 velk_claim_world(VelkDrawData d, uvec2 ids)
{
    ElementInstance inst = velk_draw_instance(d, velk_ids_slot(d, ids));
    return velk_sized_world(inst.world_matrix, inst.offset.xyz, inst.size.xyz);
}
)" VELK_LIGHT_POOL_GLSL VELK_LIGHT_LIST_GLSL VELK_LIGHT_CLAIM_GLSL R"(
void main()
{
    // A helper invocation's atomics give nothing back to go by.
    if (gl_HelperInvocation || velk_light_claim_idle()) return;
    VelkDrawData d = velk_draw_data.data[pc.draw_base];
    g_ids = uvec2(pc.draw_base + 1u, (v_instance << d.triangle_bits) | uint(gl_PrimitiveID));
    uint record = velk_ids_record(d, g_ids);
    if (record == VELK_NO_RECORD) return;
    velk_fragment_triangle(d, g_ids, v_rel_pos, g_weights, g_longest);
    velk_light_claim(d, g_ids, record);
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
 * group an ask: a triangle's root, or a piece's children, down to the depth asked along the
 * asking pixel's way (VELK_SPLIT_PATH_SHIFT): at each level the four children of the piece it is
 * in, so that every piece has its four children or none. Their points start untraced, but for a root's corners, which take the vertices' points'
 * light. They are linked in last, so that nothing reads them before they are written.
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
    light_words.data[first + VELK_PAGE_TOUCHED] = VELK_LIGHT_NONE;
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
shared uint s_path;         // the asking pixel's way down (VELK_SPLIT_PATH_SHIFT), 2 bits a level
shared uint s_edge;         // VELK_PIECE_PLACE_EDGE for pieces asked where light differs, else 0
shared uint s_patch;        // not 0 for a flat patch's pieces; s_triangle is then its light root slot

// Which child of the path's piece at level `level` (1 and on) the path goes on in: the one whose
// four children make the next level.
uint velk_split_digit(uint level)
{
    return (s_path >> (2u * (level - 1u))) & 3u;
}

// The pieces made: the root at level 0 when it is new, then at each level the four children of the
// path's piece of the level above.
uint velk_split_count()
{
    return (s_from == 0u ? 1u : 0u) + 4u * s_to;
}

// The page of piece `q` (0 to 3, the child of the path's piece above it) at level `level` of the
// pieces being made, or the top piece at level 0.
uint velk_split_page(uint level, uint q)
{
    if (level == 0u) return s_top;
    return velk_light_taken(pc.pool_word, s_base, (s_from == 0u ? 1u : 0u) + 4u * (level - 1u) + q);
}

// The place of piece `q` at level `level` under the top piece: down the path to the level above,
// then its child `q`.
uint velk_split_place(uint level, uint q)
{
    if (level == 0u) return s_top_place;
    uint place = s_top_place;
    for (uint r = 1u; r < level && r <= VELK_PIECE_LEVELS_AT_ONCE; ++r) {
        place = s_patch != 0u ? velk_patch_child_place(place, velk_split_digit(r)) : velk_piece_child_place(place, velk_split_digit(r));
    }
    return s_patch != 0u ? velk_patch_child_place(place, q) : velk_piece_child_place(place, q);
}

// Writes piece `q` at level `level` of the pieces being made, its points untraced but for a root's
// corners, which the vertices' points give. A child traces even the points it shares with its
// parent at its own places: its parent's may be light sampled around them (VELK_LIGHT_POINT_COARSE),
// and the parent shows under it until they are traced.
void velk_split_write(uint level, uint q, uint frame)
{
    uint pool = pc.pool_word;
    uint page = velk_split_page(level, q);
    uint at = velk_light_page(pool, page);
    uint place = velk_split_place(level, q);
    // Children only on the path: all four or none.
    bool on_path = level < s_to && (level == 0u || q == velk_split_digit(level));
    for (uint k = 0u; k < 4u; ++k) {
        light_words.data[at + VELK_PIECE_CHILDREN + k] = on_path ? velk_split_page(level + 1u, k) : VELK_LIGHT_NONE;
    }
    light_words.data[at + VELK_PIECE_PLACE] = place | s_edge;
    light_words.data[at + VELK_PIECE_OWNER] = s_record | (s_triangle << 24u);
    light_words.data[at + VELK_PIECE_PARENT] = level == 0u ? VELK_LIGHT_NONE
                                             : velk_split_page(level - 1u, level == 1u ? 0u : velk_split_digit(level - 1u));
    light_words.data[at + VELK_PIECE_READ] = frame;
    for (uint p = 0u; p < VELK_PIECE_POINTS; ++p) {
        uint shade = VELK_LIGHT_NONE;
        uint state = VELK_LIGHT_NONE;
        uint blockers = 0u;
        uvec2 ab = velk_piece_place(p);
        uint from_word = VELK_LIGHT_NONE;
        uint from_blocker = VELK_LIGHT_NONE;
        if (level == 0u && s_from == 0u && s_patch == 0u) {
            // A root's corners are its triangle's vertices. A patch's root starts untraced.
            uint n = velk_piece_level(place);
            uint corner = ab == uvec2(0u, 0u) ? 0u : ab == uvec2(n, 0u) ? 1u : ab == uvec2(0u, n) ? 2u : 3u;
            uint index = light_words.data[s_record_first + VELK_PAGE_INDEX];
            uint cluster = light_words.data[s_record_first + VELK_PAGE_CLUSTER];
            uint triangle = velk_patch_triangle(cluster, s_triangle);
            if (corner < 3u && index != VELK_LIGHT_NONE && triangle < light_words.data[cluster + VELK_LOD_CLUSTER_TRIANGLES]) {
                uint v = velk_mesh_words.data[index + light_words.data[cluster + VELK_LOD_CLUSTER_INDEX] + triangle * 3u + corner];
                if (v < light_words.data[s_record_first + VELK_PAGE_VERTICES]) {
                    from_word = velk_light_own_word(pool, s_record_first, v);
                    from_blocker = velk_light_blocker_word(pool, s_record_first, v);
                }
            }
        }
        // Not a stale vertex point's: the root's own changes start from none, so it would read
        // as fresh.
        if (from_word != VELK_LIGHT_NONE
            && velk_light_point_traces(pool, s_record_first, VELK_LIGHT_NONE, light_words.data[from_word + 1u]) > 0u) {
            shade = light_words.data[from_word];
            state = light_words.data[from_word + 1u];
            blockers = light_words.data[from_blocker];
        }
        light_words.data[at + 2u * p] = shade;
        light_words.data[at + 2u * p + 1u] = state;
        light_words.data[velk_light_page_blocker(pool, page, 2u * p)] = blockers;
    }
    // Nothing has changed in its way yet.
    light_words.data[velk_piece_changed_word(pool, page)] = VELK_LIGHT_NONE;
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
        s_path = (how >> VELK_SPLIT_PATH_SHIFT) & 0xFFu;
        s_edge = (how & VELK_LIGHT_SPLIT_EDGE) != 0u ? VELK_PIECE_PLACE_EDGE : 0u;
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
                        s_patch = velk_patch_is(light_words.data[s_record_first + VELK_PAGE_CLUSTER], s_triangle) ? 1u : 0u;
                        s_top_place = s_patch != 0u ? velk_patch_root_place()
                                                    : velk_piece_root_place(level == 0u ? VELK_PIECE_LEVEL : level);
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
                    s_patch = velk_piece_is_patch(top_place) ? 1u : 0u;
                    s_record = owner & 0xFFFFFFu;
                    s_record_first = velk_light_page(pool, s_record);
                    s_triangle = owner >> 24u;
                    s_from = 1u;
                    s_to = clamp(depth, top_depth + 1u, min(top_depth + VELK_PIECE_LEVELS_AT_ONCE, VELK_PIECE_DEEPEST)) - top_depth;
                }
            }
        }
        if (s_split != 0u) {
            uint count = velk_split_count();
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
    // All at once: no piece takes anything from another.
    uint made = velk_split_count();
    uint root_made = s_from == 0u ? 1u : 0u;
    for (uint i = lane; i < made; i += 64u) {
        if (i < root_made) {
            velk_split_write(0u, 0u, frame);
        } else {
            velk_split_write((i - root_made) / 4u + 1u, (i - root_made) % 4u, frame);
        }
    }
    memoryBarrierBuffer();
    barrier();
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
            // A root word a light root slot: a flat patch's, or a triangle's in none.
            uint index_base = draw == 0xFFFFFFFFu ? VELK_LIGHT_NONE : velk_draw_data.data[draw].index_base;
            uint counts = velk_patch_counts(cluster);
            uint triangles = min(counts != 0u ? counts >> 16u : light_words.data[cluster + VELK_LOD_CLUSTER_TRIANGLES], kMaxTriangles);
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
            // Grown from a traced one value: its coarse light starts as that one value, from the
            // same sphere and lights (velk_light_seeded), so that its pixels show what they showed
            // until its points are traced.
            if (was >= VELK_LIGHT_ONE_VALUE && was < VELK_LIGHT_PENDING && (was & VELK_LIGHT_ONE_TRACED) != 0u) {
                light_words.data[s_first + VELK_PAGE_COARSE] = VELK_LIGHT_ONE_VALUE | (was & VELK_LIGHT_ONE_SHADE_BITS);
            }
            // Finer detail where a record of coarser detail had points (kLightInherit): its lights
            // and its coarse light, chosen and traced around the same place, which the light mark
            // kept for this.
            uint parent = was & ~VELK_LIGHT_INHERIT;
            if (velk_light_inherits(was) && parent < light_words.data[pool + VELK_POOL_PAGES]) {
                uint parent_first = velk_light_page(pool, parent);
                uint coarse = light_words.data[parent_first + VELK_PAGE_COARSE];
                if (light_words.data[parent_first + VELK_PAGE_KIND] != VELK_LIGHT_LINKED
                    && light_words.data[parent_first + VELK_PAGE_SLOTS] != VELK_LIGHT_NONE && coarse >= VELK_LIGHT_ONE_VALUE
                    && coarse < VELK_LIGHT_PENDING && (coarse & VELK_LIGHT_ONE_TRACED) != 0u) {
                    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
                        light_words.data[s_first + VELK_PAGE_SLOTS + s] = light_words.data[parent_first + VELK_PAGE_SLOTS + s];
                    }
                    light_words.data[s_first + VELK_PAGE_COARSE] = VELK_LIGHT_ONE_VALUE | (coarse & VELK_LIGHT_ONE_SHADE_BITS);
                }
            }
            light_words.data[record + VELK_RECORD_LIGHT] = s_page;
            velk_light_trace_now(s_page, instance);
            // For the log, why: no light before, one value before, or a coarser record's points.
            uint why = was == VELK_LIGHT_NONE ? VELK_POOL_SUM_LAID_NEW
                     : velk_light_inherits(was) ? VELK_POOL_SUM_LAID_INHERITED : VELK_POOL_SUM_LAID_FROM_ONE;
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
 * vertices link to, and counts its records with pages and with one value. Lists the one values a
 * screen probe's ray asked for (kLightOneAsked) that are due, a record's or a quad's node's, as
 * later traces. A thread per record of the cut's pool, a group a page up to the last in use
 * (VELK_CUT_DRAW_GROUPS). Runs after the sweep's begin, so before the claim lists this frame's.
 */
inline constexpr string_view light_mark_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint cut_word;
    uint pool_word;
    uint instances_base;  // the cut's, as instance_cut_choose_src has them
    uint items_word;
    uint globals_base;    // the view's
} pc;

#define VELK_LIGHT_WORDS light_words
)" VELK_LIGHT_POOL_GLSL R"(
// Lists the one value at `at`, of kind `kind` (VELK_LIGHT_TRACE_*), when a probe's ray asked for
// it and it is due, as a later trace: of the rays the last frame's first traces left, at most
// half the frame's, and at most half the trace list, so that the claim's first traces after it
// always have room. Its frame goes into it, so that nothing lists it again this frame; when it
// finds no room it is given back, so that a pixel's claim can still list it, and it is listed
// again on a later frame.
void velk_light_list_asked(uint at, uint kind, uint target, uint instance, uint table, uint frame)
{
    uint pool = pc.pool_word;
    uint value = light_words.data[at];
    if (value < VELK_LIGHT_ONE_VALUE || value >= VELK_LIGHT_PENDING) return;
    if ((value & VELK_LIGHT_ONE_ASKED) == 0u || !velk_light_one_due(value)) return;
    uint listed = (value & ~(31u << VELK_LIGHT_ONE_QUEUED_SHIFT)) | ((frame & 31u) << VELK_LIGHT_ONE_QUEUED_SHIFT);
    if (listed == value || atomicCompSwap(light_words.data[at], value, listed) != value) return;
    uint rays = 8u * clamp(light_words.data[pool + VELK_POOL_LIGHTS], 1u, VELK_LIGHT_SLOTS);
    uint budget = light_words.data[pool + VELK_POOL_RAY_BUDGET];
    uint most = min(budget - min(light_words.data[pool + VELK_POOL_LAST_FIRST_RAYS_ASKED], budget), budget / 2u);
    bool room = light_words.data[pool + VELK_POOL_TRACES] < VELK_LIGHT_TRACE_CAPACITY / 2u;
    if (room) {
        uint before = atomicAdd(light_words.data[pool + VELK_POOL_RAYS_LISTED], rays);
        if (before + rays > most) {
            atomicAdd(light_words.data[pool + VELK_POOL_RAYS_LISTED], 0u - rays);
            room = false;
        }
    }
    if (room) {
        // Every place under the count is written, as velk_light_list_entry writes them.
        uint i = atomicAdd(light_words.data[pool + VELK_POOL_TRACES], 1u);
        if (i < VELK_LIGHT_TRACE_CAPACITY) {
            uint w = light_words.data[pool + VELK_POOL_TRACE_WORD] + i * VELK_LIGHT_TRACE_WORDS;
            light_words.data[w + VELK_TRACE_ENTRY_HOW] = kind;
            light_words.data[w + VELK_TRACE_ENTRY_AT] = target;
            light_words.data[w + VELK_TRACE_ENTRY_INSTANCE] = instance;
            light_words.data[w + VELK_TRACE_ENTRY_TABLE] = table;
            return;
        }
        atomicAdd(light_words.data[pool + VELK_POOL_RAYS_LISTED], 0u - rays);
    }
    atomicCompSwap(light_words.data[at], listed, value);
}

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
    uint entry = light_words.data[record + VELK_RECORD_ENTRY];
    GlobalData g = velk_globals.data[pc.globals_base];
    uint frame = g.present_counter;
    // A quad's light is on the node over it as wide as the light spacing, as the claim finds it.
    if (entry == VELK_CUT_QUAD) {
        mat4 vp = g.view_projection;
        float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
        uint node = velk_light_quad_node(light_words.data[record + VELK_RECORD_QUAD_TREE], light_words.data[record + VELK_RECORD_QUAD_NODE],
                                         g.cam_pos.xyz, pixels_per_unit, VELK_LIGHT_PIXELS);
        velk_light_list_asked(node + VELK_QUAD_NODE_LIGHT, VELK_LIGHT_TRACE_NODE, node, 0u, 0u, frame);
        return;
    }
    uint light = light_words.data[record + VELK_RECORD_LIGHT];
    if (light == VELK_LIGHT_NONE || light == VELK_LIGHT_PENDING) return;
    // Inheriting (kLightInherit): the coarser record's pages it lays its own out from are kept.
    if (velk_light_inherits(light)) {
        uint parent = light & ~VELK_LIGHT_INHERIT;
        if (parent < light_words.data[pc.pool_word + VELK_POOL_PAGES]) velk_light_mark_record(parent);
        return;
    }
    if (light >= VELK_LIGHT_ONE_VALUE) {
        atomicAdd(light_words.data[pc.pool_word + VELK_POOL_RECORDS_ONE_VALUE], 1u);
        // Asked for by a probe's ray: traced as the claim traces it, from its cluster's sphere or
        // its whole mesh's.
        if ((light & VELK_LIGHT_ONE_ASKED) == 0u || entry == VELK_CUT_RAY_QUAD || entry == VELK_CUT_STALE
            || velk_cut_light_only(entry)) {
            return;
        }
        uint slot = light_words.data[record + VELK_RECORD_SLOT];
        uint instance = pc.instances_base + slot;
        if (pc.items_word != VELK_LIGHT_NONE) instance = light_words.data[pc.items_word + slot * VELK_PLAIN_ITEM_WORDS];
        if (instance == VELK_LIGHT_NONE) return;
        if (entry == 0xFFFFFFFFu) {
            uint table = light_words.data[record + VELK_RECORD_MESH_TABLE];
            velk_light_list_asked(record + VELK_RECORD_LIGHT, VELK_LIGHT_TRACE_MESH, record, instance, table, frame);
        } else {
            uint cluster = light_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + light_words.data[record + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS;
            velk_light_list_asked(record + VELK_RECORD_LIGHT, VELK_LIGHT_TRACE_CLUSTER, record, instance, cluster, frame);
        }
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
 * children without children of their own, made where light differed (VELK_PIECE_PLACE_EDGE), whose
 * light has been the same as the piece's for kLightPieceSameFrames and one more (velk_pieces_same), so that pixels read the piece again and
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

// Frames an unread piece is kept: VELK_PIECE_KEEP_FRAMES when the pool is short of pages, longer
// the more room it has, so that turning back to what went out of view finds its pieces and their
// light. It shortens a step at a time as the pool fills, so that a burst of new pieces while the
// camera turns does not let every piece out of view go at once. The room is what the last frame
// left free: this runs before the sweep refills the stack. A pool asked for pages it did not have
// keeps them VELK_PIECE_KEEP_FRAMES, so that what out-of-view pieces were let keep goes to what is
// in view before the pool grows; one that cannot grow, half as long for each such frame in a row,
// down to an eighth, so that the pieces read longest ago make room for those in view.
uint velk_pieces_keep_frames(uint pool)
{
    uint free_pages = light_words.data[pool + VELK_POOL_LAST_FREE_PAGES];
    uint pages = light_words.data[pool + VELK_POOL_PAGES];
    uint demand = light_words.data[pool + VELK_POOL_DEMAND_FRAMES];
    // The free stack has room for the most pages the pool may have: all of them are there.
    bool at_most = pages == light_words.data[pool + VELK_POOL_MARKS_WORD] - light_words.data[pool + VELK_POOL_STACK_WORD];
    if (demand > 0u) return at_most ? VELK_PIECE_KEEP_FRAMES >> min(demand, 3u) : VELK_PIECE_KEEP_FRAMES;
    if (free_pages * 2u > pages) return 32u * VELK_PIECE_KEEP_FRAMES;
    if (free_pages * 4u > pages) return 8u * VELK_PIECE_KEEP_FRAMES;
    if (free_pages * 8u > pages) return 4u * VELK_PIECE_KEEP_FRAMES;
    if (free_pages * 16u > pages) return 2u * VELK_PIECE_KEEP_FRAMES;
    return VELK_PIECE_KEEP_FRAMES;
}

// Whether the piece starting at word `at`, which has no children, has not been read for `keep`
// frames, the pool's frame being `now`.
bool velk_pieces_unread(uint at, uint now, uint keep)
{
    uint read = light_words.data[at + VELK_PIECE_READ] & VELK_PIECE_FRAME_BITS;
    return ((now - read) & VELK_PIECE_FRAME_BITS) > keep;
}

// Whether the piece starting at word `at` and its four children all carry the same light at
// every point of their grids, each light fully through or fully blocked, as their points'
// stored light says whether stale or not; a point not traced yet says no.
bool velk_pieces_same(uint pool, uint at, uint pages)
{
    uint shade = light_words.data[at];
    if (shade == VELK_LIGHT_NONE || shade != (shade & 0x11111111u) * 15u) return false;
    uint place = light_words.data[at + VELK_PIECE_PLACE];
    uint level = velk_piece_level(place);
    bool is_patch = velk_piece_is_patch(place);
    uint points = is_patch ? VELK_PATCH_POINTS : VELK_PIECE_POINTS;
    for (uint q = 1u; q < points; ++q) {
        uvec2 ab = velk_piece_place(q);
        if ((is_patch || ab.x + ab.y <= level) && light_words.data[at + 2u * q] != shade) return false;
    }
    for (uint k = 0u; k < 4u; ++k) {
        uint child = light_words.data[at + VELK_PIECE_CHILDREN + k];
        if (child >= pages) return false;
        uint child_at = velk_light_page(pool, child);
        for (uint q = 0u; q < points; ++q) {
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
        uint keep = velk_pieces_keep_frames(pool);
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
                } else if (!velk_pieces_unread(child_at, now, keep)) {
                    unread = false;
                }
            }
            bool merge = false;
            // Only children made where light differed (VELK_PIECE_PLACE_EDGE): those made to bring
            // the levels to the light spacing would be asked for again at once.
            bool edge = (light_words.data[velk_light_page(pool, child0) + VELK_PIECE_PLACE] & VELK_PIECE_PLACE_EDGE) != 0u;
            if (!unread && leaves && edge) {
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
                atomicAdd(light_words.data[pool + (unread ? VELK_POOL_SUM_PIECES_UNREAD : VELK_POOL_SUM_PIECES_MERGED)], 4u);
            }
        } else if (parent == VELK_LIGHT_NONE && velk_pieces_unread(at, now, keep)) {
            if (atomicCompSwap(light_words.data[velk_light_root_word(pool, first, triangle)], page, VELK_LIGHT_NONE) == page) {
                atomicAdd(light_words.data[pool + VELK_POOL_SUM_PIECES_UNREAD], 1u);
            }
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
            // What the frame's work was charged: the lights listed for tracing and the points.
            const uint charged = light_words.data[pool + VELK_POOL_RAYS_LISTED] + light_words.data[pool + VELK_POOL_POINT_RAYS_CHARGED];
            light_words.data[pool + VELK_POOL_RAYS_LISTED] = 0u;
            light_words.data[pool + VELK_POOL_POINT_RAYS_CHARGED] = 0u;
            light_words.data[pool + VELK_POOL_LAST_UNTRACED_POINTS] = light_words.data[pool + VELK_POOL_UNTRACED_POINTS];
            light_words.data[pool + VELK_POOL_LAST_COARSE_POINTS] = light_words.data[pool + VELK_POOL_COARSE_POINTS];
            light_words.data[pool + VELK_POOL_LAST_FINE_POINTS] = light_words.data[pool + VELK_POOL_FINE_POINTS];
            light_words.data[pool + VELK_POOL_LAST_WARMUP_POINTS] = light_words.data[pool + VELK_POOL_WARMUP_POINTS];
            light_words.data[pool + VELK_POOL_LAST_MARK_PIECES] = light_words.data[pool + VELK_POOL_MARK_PIECES];
            light_words.data[pool + VELK_POOL_LAST_MARK_DEEPEST] = light_words.data[pool + VELK_POOL_MARK_DEEPEST];
            light_words.data[pool + VELK_POOL_MARK_PIECES] = 0u;
            light_words.data[pool + VELK_POOL_MARK_DEEPEST] = 0u;
            light_words.data[pool + VELK_POOL_LAST_SPLITS] = light_words.data[pool + VELK_POOL_SPLITS];
            light_words.data[pool + VELK_POOL_SPLITS] = 0u;
            light_words.data[pool + VELK_POOL_FRAME] = light_words.data[pool + VELK_POOL_FRAME] + 1u;
            light_words.data[pool + VELK_POOL_UNTRACED_POINTS] = 0u;
            light_words.data[pool + VELK_POOL_COARSE_POINTS] = 0u;
            light_words.data[pool + VELK_POOL_FINE_POINTS] = 0u;
            light_words.data[pool + VELK_POOL_WARMUP_POINTS] = 0u;
            // For the budget in rays, which the CPU sets from the trace's time (adjust_light_rays):
            // whether the frame's work used it.
            light_words.data[pool + VELK_POOL_LAST_RAYS_CHARGED] = charged;
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
            light_words.data[pool + VELK_POOL_LAST_FREE_PAGES] = light_words.data[pool + VELK_POOL_FREE_PAGES]
                - min(light_words.data[pool + VELK_POOL_PAGES_TAKEN], light_words.data[pool + VELK_POOL_FREE_PAGES]);
            light_words.data[pool + VELK_POOL_FREE_PAGES] = 0u;
            // Kept for the pool's growth, which looks only now and then.
            light_words.data[pool + VELK_POOL_MOST_PAGES_WANTED] = max(light_words.data[pool + VELK_POOL_MOST_PAGES_WANTED],
                                                                       light_words.data[pool + VELK_POOL_PAGES_WANTED]);
            light_words.data[pool + VELK_POOL_DEMAND_FRAMES] = light_words.data[pool + VELK_POOL_PAGES_WANTED] > 0u
                ? min(light_words.data[pool + VELK_POOL_DEMAND_FRAMES] + 1u, 3u) : 0u;
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
    uint64_t low_since = 0;        ///< Since when it has been under a sixteenth free, 0 while not.
    uint64_t ahead_frame = 0;      ///< When it last grew ahead of a frame asking (kLightPoolAheadFrames).
    uint32_t short_checks = 0;     ///< Checks in a row that found pages asked for and not there.
    uint64_t grew_frame = 0;       ///< When it last grew for pages asked for.
    bool reads = false;            ///< Whether VELK_LIGHT_READ named pixels, logged every frame.
    uint64_t log_frame = 0;        ///< When its counts were last logged.
    uint32_t recorded_pages = 0;   ///< What the culling recording sweeps.
    float rays = kLightTraceRays;  ///< The trace's budget in rays a frame (adjust_light_rays).
    /// Threads that fill the device (IRenderBackend::compute_fill_threads): a budget of fewer
    /// rays leaves it idle and takes about as long, so it is the least the budget goes to.
    uint32_t fill = 4096;
    float trace_ms = 0.f;          ///< The GPU time of its trace's last measured frame.
    float most_trace_ms = 0.f;     ///< And the most of it since its counts were last logged.
    uint32_t written_rays = kLightTraceRays; ///< What the header holds of it.

    uint32_t word() const
    {
        return header.valid() ? static_cast<uint32_t>(header.offset() / sizeof(uint32_t)) : 0xFFFFFFFFu;
    }
    uint32_t pages() const { return static_cast<uint32_t>(segments.size()) * kLightSegmentPages; }
};

/// Makes @p pool's header on its first use, with a segment, and as many segments as
/// @p device_bytes of device memory has room for at most: a thirty-second of it, up to
/// kLightMaxSegments. @p fill_threads is IRenderBackend::compute_fill_threads.
inline bool ensure_light_pool(LightPool& pool, IGpuArena& arena, uint64_t device_bytes, uint32_t fill_threads)
{
    if (pool.header.valid()) {
        return true;
    }
    pool.fill = fill_threads;
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
    const uint64_t words = points_word + 4u * point_list_words;
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
    initial[kLightPoolRayBudget] = kLightTraceRays;
    initial[kLightPoolFillThreads] = fill_threads;
    initial[kLightPoolUntracedPointsWord] = base + static_cast<uint32_t>(points_word);
    initial[kLightPoolWarmupPointsWord] = base + static_cast<uint32_t>(points_word + point_list_words);
    initial[kLightPoolCoarsePointsWord] = base + static_cast<uint32_t>(points_word + 2u * point_list_words);
    initial[kLightPoolFinePointsWord] = base + static_cast<uint32_t>(points_word + 3u * point_list_words);
    // A development aid: with VELK_PATCH_OUTSIDE set, the trace counts the flat patch points it
    // traces outside their patch's triangles, for the log.
    initial[kLightPoolPatchOutside] = std::getenv("VELK_PATCH_OUTSIDE") ? 1u : 0u;
    // A development aid: VELK_LIGHT_READ=x,y;x,y;... names up to kLightReadPixels pixels whose
    // lighting terms the lighting writes and the log prints.
    for (uint32_t k = 0; k < kLightReadPixels; ++k) initial[kLightPoolReadPixels + k] = kLightNone;
    if (const char* read = std::getenv("VELK_LIGHT_READ")) {
        uint32_t k = 0;
        for (const char* at = read; at && *at && k < kLightReadPixels;) {
            char* end = nullptr;
            const unsigned long x = std::strtoul(at, &end, 10);
            if (end && *end == ',') {
                const unsigned long y = std::strtoul(end + 1, &end, 10);
                if (x < 65536u && y < 65536u) {
                    initial[kLightPoolReadPixels + k++] = uint32_t(x) | (uint32_t(y) << 16u);
                    pool.reads = true;
                }
            }
            at = std::strchr(at, ';');
            if (at) ++at;
        }
    }
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

/// Moves @p pool's budget in rays a frame toward what @p target_ms of GPU time would take,
/// by @p trace_ms, what its trace took the last frame measured (0 when not known); no limit when
/// @p target_ms is 0 or less. A quarter of the way up a frame, half of it down, at most twice or a
/// quarter as much, as the time comes a frame or two late. It grows only while the frame's work
/// used the budget, so that frames needing little do not leave a budget that heavier work would
/// spend all at once. Never under the threads that fill the device (LightPool::fill): fewer rays
/// take about the time of the longest walk among them, so cutting them saves nothing.
inline void adjust_light_rays(LightPool& pool, IGpuArena& arena, float trace_ms, float target_ms)
{
    if (!pool.header.valid()) {
        return;
    }
    if (trace_ms > 0.f) {
        pool.trace_ms = trace_ms;
        pool.most_trace_ms = trace_ms > pool.most_trace_ms ? trace_ms : pool.most_trace_ms;
    }
    uint32_t budget = kLightTraceRaysUnlimited;
    if (target_ms > 0.f) {
        if (trace_ms > 0.f) {
            uint32_t charged = 0;
            arena.read_at(pool.header.offset() + uint64_t(kLightPoolLastRaysCharged) * sizeof(uint32_t), &charged,
                          sizeof(charged));
            float scale = target_ms / trace_ms;
            scale = scale < 0.25f ? 0.25f : scale > 2.f ? 2.f : scale;
            if (scale > 1.f && float(charged) < 0.9f * pool.rays) {
                scale = 1.f;
            }
            // Half the way down, a quarter up: a frame far over its time is cut back in a few.
            pool.rays *= 1.f + (scale < 1.f ? 0.5f : 0.25f) * (scale - 1.f);
            const float least = float(pool.fill);
            pool.rays = pool.rays < least ? least : pool.rays > float(kLightTraceRaysUnlimited) ? float(kLightTraceRaysUnlimited) : pool.rays;
        }
        budget = static_cast<uint32_t>(pool.rays);
    }
    if (budget != pool.written_rays) {
        arena.update_at(pool.header.offset() + uint64_t(kLightPoolRayBudget) * sizeof(uint32_t), &budget, sizeof(budget));
        pool.written_rays = budget;
    }
}

} // namespace velk

#endif // VELK_RENDER_FRAME_LIGHT_POOL_H
