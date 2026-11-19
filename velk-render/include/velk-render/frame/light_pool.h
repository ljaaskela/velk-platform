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
/// free pages, then a bit per page marking those in use, both for as many pages as the pool may
/// grow to, then the search list, and its pages in segments of kLightSegmentPages, each a region
/// of its own, so that growing moves no page.
///
/// A record's first page starts with its header (VELK_LIGHT_PAGE_HEADER), then for own points
/// with a grid the word of each triangle's grid. Its point words follow, from an even word
/// on, then those of its other pages in order: with its own points two words a point, a point per
/// vertex, then each triangle's grid points in turn: how much of each slot's light it does not get
/// (4 bits a slot, the first lowest, 15 none, kLightNone until first traced), then how many times
/// it has been traced (top 4 bits) and the frame it was last listed (the rest; kLightNone before
/// that); with kLightLinked three words per vertex: the first page of the record for light only
/// whose points it reads, their numbers there (8 bits each, the first lowest), and the weights of
/// the second and third (unorm 16 bits each, the second lowest; the first's is what is left of 1).
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
    X(Kind, KIND, 1)                  /* own points: kLightOwn, or kLightGridded with its triangles; or kLightLinked */ \
    X(PointWords, POINT_WORDS, 2)     /* how many point words it has */                                              \
    X(Pages, PAGES, 3)                /* its other pages from the second, kLightNone for none; past                  \
                                         kLightDirectPages of them, its index pages of 64 pages each */              \
    X(Changed, CHANGED, 17)           /* the frame something last changed what its points could get, or kLightNone */ \
    X(Cluster, CLUSTER, 18)           /* its cluster's word in its hierarchy's table */                              \
    X(Reset, RESET, 19)               /* the frame its lights were last chosen again, or kLightNone */               \
    X(Slots, SLOTS, 20)               /* its lights, a light index a slot (kLightSlots), kLightNone for none */      \
    X(Index, INDEX, 28)               /* its draw's first index word */                                              \
    X(Across, ACROSS, 29)             /* how wide on screen it was laid out for, a float */                          \
    X(Coarse, COARSE, 30)             /* its coarse light: a one value from its cluster's sphere with its points'    \
                                         lights, read under its points until they are trusted */                     \
    X(Read, READ, 31)                 /* not 0 when a pixel read its points since the last mark; for the log */      \
    X(Grids, GRIDS, 32)               /* for own points with a grid, the first of the triangles' grid words */

#define VELK_LIGHT_PAGE_CPP_WORD(cpp, glsl, word) inline constexpr uint32_t kLightPage##cpp = word;
VELK_LIGHT_PAGE_HEADER(VELK_LIGHT_PAGE_CPP_WORD)
#undef VELK_LIGHT_PAGE_CPP_WORD
/// The page header's words, for the shaders: a #define VELK_PAGE_ each.
#define VELK_LIGHT_PAGE_GLSL_WORD(cpp, glsl, word) "#define VELK_PAGE_" #glsl " " #word "u\n"
#define VELK_LIGHT_PAGE_HEADER_GLSL VELK_LIGHT_PAGE_HEADER(VELK_LIGHT_PAGE_GLSL_WORD)
/// The header's words: the triangles' grid words follow it.
inline constexpr uint32_t kLightPageHeaderWords = kLightPageGrids;
static_assert(kLightPageChanged == kLightPagePages + 14u, "the header names 14 pages after the first");
static_assert(kLightPageIndex == kLightPageSlots + 8u, "eight light slots");
/// The finest grid of light points in a triangle: that many along each of its edges, as many as
/// its level's 8 bits hold. A record's grids are as fine as its pages hold (kLightMaxPages).
inline constexpr uint32_t kLightMaxGrid = 255;
/// Own points with a grid in their triangles have this in their header's word 1, with their
/// triangle count under it: a word per triangle follows the header, its grid's level (bits 24 up)
/// and where its grid points start after the vertices' points.
inline constexpr uint32_t kLightGridded = 0x40000000u;
/// Pages a record's header names itself; past that, it names index pages of 64 pages each.
inline constexpr uint32_t kLightDirectPages = 15;
/// Traces a point of light takes, one a frame while a pixel reads it, after which it is traced
/// again only when something changes it (light_changes_main_src).
inline constexpr uint32_t kLightWarmTraces = 8;
inline constexpr uint32_t kLightMaxPages = 1u + 14u * 64u;
inline constexpr uint32_t kLightSlots = 8;
inline constexpr uint32_t kLightSegmentPages = 16384;
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
    X(CoarsePoints, COARSE_POINTS, 118)          /* points on the coarse trace list this frame, past its room too */ \
    X(CoarsePointsWord, COARSE_POINTS_WORD, 119) /* the word of the coarse trace list */                             \
    X(LastCoarsePoints, LAST_COARSE_POINTS, 120)                                                                     \
    X(SumRetraces, SUM_RETRACES, 121)            /* summed: later traces of points traced again after a change */    \
    X(SumLaidNew, SUM_LAID_NEW, 122)             /* summed: own points laid out for records without light */         \
    X(SumLaidGrown, SUM_LAID_GROWN, 123)         /* summed: for records laid out again, having grown */              \
    X(SumLaidFromOne, SUM_LAID_FROM_ONE, 124)    /* summed: for records grown from one value */                      \
    X(SumLaidLightOnly, SUM_LAID_LIGHT_ONLY, 125) /* summed: for records for light only */                           \
    X(MarkPoints, MARK_POINTS, 126)              /* this frame's mark, of records with own points: their points */   \
    X(MarkReadPoints, MARK_READ_POINTS, 127)     /* the points of those a pixel read since the last mark */          \
    X(MarkMostPoints, MARK_MOST_POINTS, 128)     /* the most one has */                                              \
    X(MarkBuckets, MARK_BUCKETS, 129)            /* records with under 256, 1024, 4096, 16384 points, and more */    \
    X(MarkBigPoints, MARK_BIG_POINTS, 134)       /* the points of those with 4096 or more */                         \
    X(LastMark, LAST_MARK, 135)                  /* MARK_POINTS to MARK_BIG_POINTS of the last frame */              \
    X(HeaderEnd, HEADER_END, 144)

#define VELK_LIGHT_POOL_CPP_WORD(cpp, glsl, word) inline constexpr uint32_t kLightPool##cpp = word;
VELK_LIGHT_POOL_HEADER(VELK_LIGHT_POOL_CPP_WORD)
#undef VELK_LIGHT_POOL_CPP_WORD
/// The header's words, for the shaders: a #define VELK_POOL_ each.
#define VELK_LIGHT_POOL_GLSL_WORD(cpp, glsl, word) "#define VELK_POOL_" #glsl " " #word "u\n"
#define VELK_LIGHT_POOL_HEADER_GLSL VELK_LIGHT_POOL_HEADER(VELK_LIGHT_POOL_GLSL_WORD)
/// The mark's counts, MARK_POINTS to MARK_BIG_POINTS.
inline constexpr uint32_t kLightPoolMarkWords = kLightPoolLastMark - kLightPoolMarkPoints;
inline constexpr uint32_t kLightPoolHeaderWords = kLightPoolHeaderEnd;
static_assert(kLightPoolTraces == kLightPoolSegmentWords + kLightMaxSegments, "the segments' words end where the trace's begin");
static_assert(kLightPoolHeaderEnd == kLightPoolLastMark + kLightPoolMarkWords, "the last mark's counts end the header");
/// Records the search list takes a frame; more wait for the next.
inline constexpr uint32_t kLightSearchCapacity = 1024;  ///< Mirrors VELK_LIGHT_SEARCH_CAPACITY.
inline constexpr uint32_t kLightSearchWords = kSearchEntryWords;
/// Lights the trace list takes a frame, and its words a light (VELK_TRACE_ENTRY in
/// cull_layout.h). Room for the first trace of the largest record (kLightMaxPages of points,
/// eight points an entry).
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
#define VELK_LIGHT_POOL_GLSL VELK_LIGHT_POOL_HEADER_GLSL VELK_LIGHT_PAGE_HEADER_GLSL R"(
#define VELK_LIGHT_NONE 0xFFFFFFFFu
#define VELK_LIGHT_PENDING 0xFFFFFFFEu
#define VELK_LIGHT_ONE_VALUE 0x80000000u
#define VELK_LIGHT_OWN 1u
#define VELK_LIGHT_LINKED 0x80000000u
#define VELK_LIGHT_PAGE_WORDS 64u
#define VELK_LIGHT_MAX_GRID 255u
#define VELK_LIGHT_WARM_TRACES 8u
#define VELK_LIGHT_DIRECT_PAGES 15u
#define VELK_LIGHT_MAX_PAGES 897u
#define VELK_LIGHT_GRIDDED 0x40000000u
#define VELK_LIGHT_SLOTS 8u
#define VELK_LIGHT_ONE_TRACED 0x40000000u
#define VELK_LIGHT_ONE_SHADE_BITS 0x00FFFFFFu
#define VELK_LIGHT_ONE_QUEUED_SHIFT 24u
// kLightSearchCapacity, kLightTraceCapacity, kLightTraceWords.
#define VELK_LIGHT_SEARCH_CAPACITY 1024u
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

// Words before the points of a record with `table` words after its header: an even number, so
// that a point's two words are on the same page.
uint velk_light_skip_for(uint table)
{
    return VELK_PAGE_GRIDS + ((table + 1u) & ~1u);
}

// Words before the points of the record whose first page starts at `first`: its header, and for
// own points with a grid (VELK_LIGHT_GRIDDED) a word per triangle saying its grid.
uint velk_light_skip(uint first)
{
    uint kind = VELK_LIGHT_WORDS.data[first + VELK_PAGE_KIND];
    return velk_light_skip_for((kind & VELK_LIGHT_GRIDDED) != 0u && kind != VELK_LIGHT_LINKED ? kind & 0xFFFFu : 0u);
}

// The pages the record whose first page starts at `first` has.
uint velk_light_page_count(uint first)
{
    return (velk_light_skip(first) + VELK_LIGHT_WORDS.data[first + VELK_PAGE_POINT_WORDS] + VELK_LIGHT_PAGE_WORDS - 1u) / VELK_LIGHT_PAGE_WORDS;
}

// Page `k` of the record whose first page starts at `first`, k from 1: named in its header, or
// with more than VELK_LIGHT_DIRECT_PAGES, in the index pages its header names.
uint velk_light_nth_page(uint pool, uint first, uint k, uint pages)
{
    if (pages <= VELK_LIGHT_DIRECT_PAGES) return VELK_LIGHT_WORDS.data[first + VELK_PAGE_PAGES - 1u + k];
    uint index = VELK_LIGHT_WORDS.data[first + VELK_PAGE_PAGES + (k - 1u) / VELK_LIGHT_PAGE_WORDS];
    return VELK_LIGHT_WORDS.data[velk_light_page(pool, index) + (k - 1u) % VELK_LIGHT_PAGE_WORDS];
}

// The arena word of word `at` of the record whose first page starts at `first`, counted from
// its first page's first word across its pages.
uint velk_light_word(uint pool, uint first, uint at)
{
    uint k = at / VELK_LIGHT_PAGE_WORDS;
    if (k == 0u) return first + at;
    return velk_light_page(pool, velk_light_nth_page(pool, first, k, velk_light_page_count(first))) + at % VELK_LIGHT_PAGE_WORDS;
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

// A point's trace count (its state's top 4 bits): VELK_LIGHT_WARM_TRACES traces warm it up from
// none, after which it is settled; a change after its first trace makes it VELK_LIGHT_RETRACE,
// from which it is traced again up to VELK_LIGHT_RETRACED, moving its light toward the new rays
// while pixels keep showing it, so that rays from before the change do not stay in its light.
#define VELK_LIGHT_RETRACE 9u
#define VELK_LIGHT_RETRACED 15u

// The trace count of the own point of the record whose first page starts at `first` with trace
// state `state`: none when its record's lights were chosen again since it was last listed;
// VELK_LIGHT_RETRACE when it was traced and something else changed what it could get since.
uint velk_light_point_traces(uint first, uint state)
{
    if (state == VELK_LIGHT_NONE) return 0u;
    uint listed = state & 0x0FFFFFFFu;
    uint reset = VELK_LIGHT_WORDS.data[first + VELK_PAGE_RESET];
    if (reset != VELK_LIGHT_NONE && listed < reset) return 0u;
    uint traces = state >> 28u;
    uint changed = VELK_LIGHT_WORDS.data[first + VELK_PAGE_CHANGED];
    if (changed != VELK_LIGHT_NONE && listed < changed && traces != 0u) return VELK_LIGHT_RETRACE;
    return traces;
}

// Whether a point with trace count `traces` is due a trace: warming up, or traced again after a
// change.
bool velk_light_point_due(uint traces)
{
    return traces < VELK_LIGHT_WARM_TRACES || (traces >= VELK_LIGHT_RETRACE && traces < VELK_LIGHT_RETRACED);
}

// The grid of triangle `t` of own points with a grid: its level (bits 24 up) and where its grid
// points start after the vertices' (the rest).
uint velk_light_triangle_grid(uint pool, uint first, uint t)
{
    return VELK_LIGHT_WORDS.data[velk_light_word(pool, first, VELK_PAGE_GRIDS + t)];
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

// Points a triangle's grid of level `n` adds to its corners, which are its vertices' points.
uint velk_light_grid_points(uint n)
{
    return n < 2u ? 0u : (n + 1u) * (n + 2u) / 2u - 3u;
}

// The point at place (i, j) of a triangle's grid of level `n` whose grid points start `base`
// after the vertices' points, with i + j at most n: at weights (1 - (i + j) / n, i / n, j / n) of
// its corners, whose points are `corners`. The grid's places go row by row in j, each row in i,
// the three corners left out.
uint velk_light_grid_point(uint vertices, uint n, uint base, uvec3 corners, uint i, uint j)
{
    if (j == 0u && i == 0u) return corners.x;
    if (j == 0u && i == n) return corners.y;
    if (j == n) return corners.z;
    uint along = j * (n + 1u) - (j * (j - 1u)) / 2u + i;
    return vertices + base + along - (j == 0u ? 1u : 2u);
}

// The place (i, j) of grid point `q` of a triangle's grid of level `n`, counted as
// velk_light_grid_point counts them.
uvec2 velk_light_grid_place(uint n, uint q)
{
    for (uint j = 0u; j <= n; ++j) {
        uint first = j == 0u ? 1u : 0u;
        uint last = j == 0u ? n - 1u : n - j;
        if (j == n) break;
        uint in_row = last + 1u - first;
        if (q < in_row) return uvec2(first + q, j);
        q -= in_row;
    }
    return uvec2(0u, 0u);
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

// The coarse level of a triangle's grid of level `n`: a grid of the same points at every 4th
// place, about four times the light spacing apart; its corners alone when it has fewer.
uint velk_light_coarse_level(uint n)
{
    return max(n / 4u, 1u);
}

// Place `c` of the coarse grid of level `m` as a place of the grid of level `n` it is in.
uvec2 velk_light_coarse_to_fine(uint n, uint m, uvec2 c)
{
    return uvec2(c.x * n / m, c.y * n / m);
}

// Whether place `f` of a triangle's grid of level `n` is one of its coarse level's.
bool velk_light_is_coarse(uint n, uvec2 f)
{
    uint m = velk_light_coarse_level(n);
    if (n < 2u || m >= n) return true;
    uvec2 c = uvec2((f.x * m + n - 1u) / n, (f.y * m + n - 1u) / n);
    return c.x + c.y <= m && velk_light_coarse_to_fine(n, m, c) == f;
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

// The points of light a pixel at weights `b` of triangle `t` (corners `corners`) of the record
// whose first page starts at `first` reads, up to nine: the words they are in, their numbers
// there, their weights and the first pages of the records they are in, whose lights their slots
// are. Returns how many.
uint velk_light_pixel_points(uint pool, uint first, uint t, uvec3 corners, vec3 b, out uint words[9], out uint ids[9],
                             out float weights[9], out uint owners[9])
{
    uint count = 0u;
    uint kind = VELK_LIGHT_WORDS.data[first + VELK_PAGE_KIND];
    if (kind != VELK_LIGHT_LINKED && (kind & VELK_LIGHT_GRIDDED) != 0u && t < (kind & 0xFFFFu)) {
        uint grid = velk_light_triangle_grid(pool, first, t);
        uint n = grid >> 24u;
        uvec2 places[3];
        vec3 w;
        velk_light_around(n, b, places, w);
        for (uint k = 0u; k < 3u; ++k) {
            uint p = velk_light_grid_point(VELK_LIGHT_WORDS.data[first + VELK_PAGE_VERTICES], n, grid & 0xFFFFFFu, corners, places[k].x, places[k].y);
            words[count] = velk_light_own_word(pool, first, p);
            ids[count] = p;
            weights[count] = w[k];
            owners[count] = first;
            ++count;
        }
        return count;
    }
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

// Puts own point `p` of the record whose first page starts at `first` on a point list, once a
// frame, when it is due (velk_light_point_due): on the first trace list when it counts as never
// traced, the coarse one when it is also a `coarse` point (a corner, or on its grid's coarse
// level), else on the later one. Its trace state takes the frame, and its count as
// velk_light_point_traces says, so that the trace reads which trace this is. Past the list's
// room it waits for a later frame.
void velk_light_list_point(uint pool, uint first, uint p, uint instance, uint frame, bool coarse)
{
    if (p >= VELK_LIGHT_WORDS.data[first + VELK_PAGE_POINT_WORDS] / 2u) return;
    uint at = velk_light_own_word(pool, first, p) + 1u;
    uint state = VELK_LIGHT_WORDS.data[at];
    uint traces = velk_light_point_traces(first, state);
    if (!velk_light_point_due(traces)) return;
    uint now = frame & 0x0FFFFFFFu;
    if (state != VELK_LIGHT_NONE && (state & 0x0FFFFFFFu) == now) return;
    if (atomicCompSwap(VELK_LIGHT_WORDS.data[at], state, (traces << 28u) | now) != state) return;
    uint list = traces != 0u ? VELK_LIGHT_LIST_LATER : coarse ? VELK_LIGHT_LIST_COARSE : VELK_LIGHT_LIST_FIRST;
    uint i = atomicAdd(VELK_LIGHT_WORDS.data[pool + velk_light_list_count_word(list)], 1u);
    if (i >= VELK_LIGHT_POINT_CAPACITY) return;
    uint w = VELK_LIGHT_WORDS.data[pool + velk_light_list_word_word(list)] + i * VELK_LIGHT_POINT_WORDS;
    VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_FIRST] = first;
    VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_POINT] = p;
    VELK_LIGHT_WORDS.data[w + VELK_POINT_ENTRY_INSTANCE] = instance;
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

// The lights of the points a pixel or hit at weights `b` of triangle `t` (corners `corners`) of
// the record whose first page starts at `first` reads, as velk_light_pixel_points finds them,
// added one at a time. False when none of them is traced yet.
//
// In a triangle's grid, each level over the one under it: the record's coarse light, then the
// grid's coarse level (velk_light_coarse_level), whose points count once traced, then the grid,
// whose points count by their traces. A point not traced yet shows what is under it.
bool velk_light_add_points(uint pool, uint first, uint t, uvec3 corners, vec3 b, inout uint lights[VELK_LIGHT_SLOTS],
                           inout vec2 sums[VELK_LIGHT_SLOTS])
{
    bool traced = false;
    uint kind = VELK_LIGHT_WORDS.data[first + VELK_PAGE_KIND];
    if (kind != VELK_LIGHT_LINKED && (kind & VELK_LIGHT_GRIDDED) != 0u && t < (kind & 0xFFFFu)) {
        uint grid = velk_light_triangle_grid(pool, first, t);
        uint n = grid >> 24u;
        uint base = grid & 0xFFFFFFu;
        uint vertices = VELK_LIGHT_WORDS.data[first + VELK_PAGE_VERTICES];
        uint coarse = VELK_LIGHT_WORDS.data[first + VELK_PAGE_COARSE];
        bool known = coarse >= VELK_LIGHT_ONE_VALUE && coarse < VELK_LIGHT_PENDING && (coarse & VELK_LIGHT_ONE_TRACED) != 0u;
        float under[VELK_LIGHT_SLOTS];
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) under[s] = 1.0 - float((coarse >> (3u * s)) & 7u) / 7.0;
        uint m = velk_light_coarse_level(n);
        for (uint level = n < 2u || m >= n ? 1u : 0u; level < 2u; ++level) {
            uvec2 places[3];
            vec3 w;
            velk_light_around(level == 0u ? m : n, b, places, w);
            float sum[VELK_LIGHT_SLOTS];
            for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) sum[s] = 0.0;
            float weight = 0.0;
            for (uint k = 0u; k < 3u; ++k) {
                if (w[k] <= 0.0) continue;
                uvec2 f = level == 0u ? velk_light_coarse_to_fine(n, m, places[k]) : places[k];
                uint word = velk_light_own_word(pool, first, velk_light_grid_point(vertices, n, base, corners, f.x, f.y));
                uint shade = VELK_LIGHT_WORDS.data[word];
                if (shade == VELK_LIGHT_NONE && !known) continue;
                float trust = 0.0;
                if (shade != VELK_LIGHT_NONE) {
                    uint traces = velk_light_point_traces(first, VELK_LIGHT_WORDS.data[word + 1u]);
                    trust = !known || level == 0u ? 1.0 : min(float(traces), VELK_LIGHT_TRUSTED_TRACES) / VELK_LIGHT_TRUSTED_TRACES;
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
        }
        if (!known) return false;
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
// how much of each gets through. False when it has no traced light yet. `light_pixels` is
// kLightPixels.
bool velk_light_direct(uint pool, uvec2 ids, vec3 at, float light_pixels, out uint lights[VELK_LIGHT_SLOTS],
                       out float through[VELK_LIGHT_SLOTS])
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
            // Where the pixel is in its triangle.
            uint word = d.index_base + velk_cluster_first_index(cluster) + velk_ids_triangle(d, ids) * 3u;
            uvec3 corners = uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
            vec3 p[3];
            for (int k = 0; k < 3; ++k) {
                uint v = velk_cluster_vertex_word(d, cluster) + corners[k] * VELK_VERTEX3D_WORDS;
                vec3 local = vec3(uintBitsToFloat(velk_mesh_words.data[v]), uintBitsToFloat(velk_mesh_words.data[v + 1u]),
                                  uintBitsToFloat(velk_mesh_words.data[v + 2u]));
                p[k] = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g);
            }
            vec3 e1 = p[1] - p[0];
            vec3 e2 = p[2] - p[0];
            vec3 q = at - p[0];
            float d11 = dot(e1, e1);
            float d12 = dot(e1, e2);
            float d22 = dot(e2, e2);
            float det = d11 * d22 - d12 * d12;
            vec3 b = vec3(1.0, 0.0, 0.0);
            if (abs(det) > 1e-20) {
                float b1 = (d22 * dot(q, e1) - d12 * dot(q, e2)) / det;
                float b2 = (d11 * dot(q, e2) - d12 * dot(q, e1)) / det;
                b = clamp(vec3(1.0 - b1 - b2, b1, b2), 0.0, 1.0);
                b /= max(b.x + b.y + b.z, 1e-6);
            }
            if (!velk_light_add_points(pool, first, velk_ids_triangle(d, ids), corners, b, lights, sums)) return false;
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
// patches of small members on the way (VELK_CUT_RAY_QUAD).
float velk_points_blocked(Light light, vec3 p, vec3 n, float lift, uint seed)
{
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
// every frame, so that what waits is not always the same. A point never traced takes a ray for
// each of its record's lights, choosing them at its cluster's centre when nothing has yet; a
// later trace one ray, to one light chosen by how much each could bring, moving that slot
// toward what the ray found.
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
    uint take_laters = min(laters, budget - min(used, budget));
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
    uint kind = VELK_LIGHT_WORDS.data[first + VELK_PAGE_KIND];
    if (kind == VELK_LIGHT_LINKED || q >= VELK_LIGHT_WORDS.data[first + VELK_PAGE_POINT_WORDS] / 2u) return;
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

    // A vertex, or a grid point in a triangle at weights of its corners.
    uint vertex_word = VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_VERTEX];
    uint table = (kind & VELK_LIGHT_GRIDDED) != 0u ? min(kind & 0xFFFFu, 256u) : 0u;
    uint first_index = VELK_LIGHT_WORDS.data[first + VELK_PAGE_INDEX] + VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_INDEX];
    vec3 local;
    vec3 normal;
    if (q < vertices) {
        uint vw = vertex_word + q * VELK_VERTEX3D_WORDS;
        local = vec3(uintBitsToFloat(velk_mesh_words.data[vw]), uintBitsToFloat(velk_mesh_words.data[vw + 1u]),
                     uintBitsToFloat(velk_mesh_words.data[vw + 2u]));
        normal = vec3(uintBitsToFloat(velk_mesh_words.data[vw + 3u]), uintBitsToFloat(velk_mesh_words.data[vw + 4u]),
                      uintBitsToFloat(velk_mesh_words.data[vw + 5u]));
    } else {
        // Its triangle: the last whose grid starts at or before it.
        uint r = q - vertices;
        uint lo = 0u;
        uint hi = table;
        while (lo < hi) {
            uint mid = (lo + hi) / 2u;
            if ((velk_light_triangle_grid(pool, first, mid) & 0xFFFFFFu) <= r) lo = mid + 1u;
            else hi = mid;
        }
        if (lo == 0u) return;
        uint t = lo - 1u;
        uint tri_grid = velk_light_triangle_grid(pool, first, t);
        uint grid = tri_grid >> 24u;
        if (grid < 2u) return;
        uvec2 place = velk_light_grid_place(grid, r - (tri_grid & 0xFFFFFFu));
        vec3 b = vec3(float(grid - place.x - place.y), float(place.x), float(place.y)) / float(grid);
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

    uint pw = velk_light_own_word(pool, first, q);
    uint old = VELK_LIGHT_WORDS.data[pw];
    uint state = VELK_LIGHT_WORDS.data[pw + 1u];
    // Which trace this is, as its listing counted it.
    uint traces = state == VELK_LIGHT_NONE ? 0u : state >> 28u;
    uint seed = velk_points_hash(first * 977u + q) ^ (frame * 0x9e3779b9u);
    uint value = 0u;
    if (traces == 0u || old == VELK_LIGHT_NONE) {
        for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
            if (slots[s] == VELK_LIGHT_NONE) continue;
            ++g_first_rays;
            float blocked = velk_points_blocked(velk_light_at(pc.lights_base + slots[s]), p, n, lift, seed + s);
            value |= uint(blocked * 15.0 + 0.5) << (4u * s);
        }
        traces = 0u;
        atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_FIRST_TRACES], 1u);
    } else {
        // Warming up, a running mean of its rays. Traced again after a change, steps of 1/2, 1/2,
        // 1/3, 1/3, 1/4 and 1/4, the first new ray moving it halfway, so that it follows what
        // changed in a frame or two without jumping to one ray's guess, and the light it had
        // keeps a sixteenth.
        float step = traces < VELK_LIGHT_WARM_TRACES ? 1.0 / float(traces + 1u)
                   : traces >= VELK_LIGHT_RETRACE ? 1.0 / float((traces - VELK_LIGHT_RETRACE) / 2u + 2u) : 0.25;
        if (traces >= VELK_LIGHT_RETRACE) atomicAdd(VELK_LIGHT_WORDS.data[pool + VELK_POOL_SUM_RETRACES], 1u);
        value = old;
        uint s = velk_points_pick(slots, p, seed);
        if (slots[s] != VELK_LIGHT_NONE) {
            float blocked = velk_points_blocked(velk_light_at(pc.lights_base + slots[s]), p, n, lift, seed + 7u);
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
 * thread per record of the cut at pc.spare2 of the top tree at pc.spare1: own points traced
 * again from their first trace, a one value traced again, a quad's on its node and the nodes
 * above it. A change of a light alters what uses that light; a box alters what it could shadow
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
    // A record a thread, 64 a group: the prelude's groups are 8 by 8.
    uint index = gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex;
    // The records placed in the current store, all before this.
    if (index >= VELK_LIGHT_WORDS.data[cut + VELK_CUT_PLACED]) return;
    uint record = VELK_LIGHT_WORDS.data[cut + VELK_CUT_STORES + VELK_LIGHT_WORDS.data[cut + VELK_CUT_PARITY]] + index * VELK_RECORD_END;
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
        // Its coarse light traced again at once, and its settled points traced again as pixels
        // next read them (velk_light_point_traces), moving toward what changed while pixels keep
        // showing them, so nothing jumps. When everything changed, from their first trace again,
        // their lights chosen again: what changed may be one of them.
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
 * with the probe the last frame had where this one was, when that was on the same surface.
 * Composed with the materials by compose_rt_compute. pc.shapes_base is the light pool,
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
        if (!velk_light_add_points(pool, velk_light_page(pool, value), 0xFFFFFFFFu, hit.corners, b, lights, sums)) return vec3(0.0);
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
            uint material;
            EvalContext ectx = velk_hit_context(hit, dir, at, hn, material);
            MaterialEval e = velk_eval_material(material, ectx);
            bool lit = e.lighting_mode == VELK_LIGHTING_STANDARD;
            vec3 albedo = lit ? e.color.rgb * (1.0 - clamp(e.metallic, 0.0, 1.0)) : vec3(0.0);
            vec3 emission = e.emissive + (lit ? vec3(0.0) : e.color.rgb);
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
 * grown to twice the light spacing goes on the list too. A thread per pixel. `light_pixels` is
 * kLightPixels.
 *
 * Run again after the search with `again` set when the search had records: it lists the points
 * of the records just laid out, so that their coarse points are traced this frame, and puts
 * nothing on the search list, which the next frame's search would not see.
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

// Lists the points of light the pixel at `coord` (ids `ids`, of draw `d`) reads from the record
// whose first page starts at `first`, those it reads with any weight, when they are due
// (velk_light_list_point). Where the pixel is in its triangle comes from its corners on screen,
// corrected for perspective, as the lighting finds it from the pixel's place.
void velk_light_list_pixel(uint first, VelkDrawData d, uvec2 ids, uvec2 coord, uint instance, uint frame, GlobalData g)
{
    uint cluster = velk_ids_cluster(d, ids);
    if (cluster == VELK_NO_CLUSTER) return;
    uint triangle = velk_ids_triangle(d, ids);
    uint word = d.index_base + velk_cluster_first_index(cluster) + triangle * 3u;
    uvec3 corners = uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
    LightInstance inst = light_instances.data[instance];
    vec2 size = vec2(float(pc.width), float(pc.height));
    vec2 p[3];
    float inv_w[3];
    for (int k = 0; k < 3; ++k) {
        uint at = velk_cluster_vertex_word(d, cluster) + corners[k] * VELK_VERTEX3D_WORDS;
        vec3 local = vec3(uintBitsToFloat(velk_mesh_words.data[at]), uintBitsToFloat(velk_mesh_words.data[at + 1u]),
                          uintBitsToFloat(velk_mesh_words.data[at + 2u]));
        vec4 clip = g.view_projection * vec4(velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g), 1.0);
        if (clip.w <= 1e-6) return;
        inv_w[k] = 1.0 / clip.w;
        p[k] = (clip.xy * inv_w[k] * 0.5 + 0.5) * size;
    }
    vec2 e1 = p[1] - p[0];
    vec2 e2 = p[2] - p[0];
    float det = e1.x * e2.y - e1.y * e2.x;
    vec3 b = vec3(1.0 / 3.0);
    if (abs(det) > 1e-9) {
        vec2 q = vec2(coord) + 0.5 - p[0];
        float b1 = (q.x * e2.y - q.y * e2.x) / det;
        float b2 = (e1.x * q.y - e1.y * q.x) / det;
        vec3 s = clamp(vec3(1.0 - b1 - b2, b1, b2), 0.0, 1.0) * vec3(inv_w[0], inv_w[1], inv_w[2]);
        b = s / max(s.x + s.y + s.z, 1e-12);
    }
    // In a triangle's grid: the points around the pixel, and those of the grid's coarse level
    // around it, which the lighting reads under them until they are traced.
    uint kind = light_words.data[first + VELK_PAGE_KIND];
    if (kind != VELK_LIGHT_LINKED && (kind & VELK_LIGHT_GRIDDED) != 0u && triangle < (kind & 0xFFFFu)) {
        uint grid = velk_light_triangle_grid(pc.pool_word, first, triangle);
        uint n = grid >> 24u;
        uint vertices = light_words.data[first + VELK_PAGE_VERTICES];
        uvec2 places[3];
        vec3 w;
        velk_light_around(n, b, places, w);
        for (uint k = 0u; k < 3u; ++k) {
            if (w[k] <= 0.0) continue;
            uint p = velk_light_grid_point(vertices, n, grid & 0xFFFFFFu, corners, places[k].x, places[k].y);
            velk_light_list_point(pc.pool_word, first, p, instance, frame, velk_light_is_coarse(n, places[k]));
        }
        uint m = velk_light_coarse_level(n);
        if (n < 2u || m >= n) return;
        velk_light_around(m, b, places, w);
        for (uint k = 0u; k < 3u; ++k) {
            if (w[k] <= 0.0) continue;
            uvec2 f = velk_light_coarse_to_fine(n, m, places[k]);
            uint p = velk_light_grid_point(vertices, n, grid & 0xFFFFFFu, corners, f.x, f.y);
            velk_light_list_point(pc.pool_word, first, p, instance, frame, true);
        }
        return;
    }
    // A point per vertex, its own or those it links to: a vertex's point is coarse.
    uint words[9];
    uint points[9];
    float weights[9];
    uint owners[9];
    uint read = velk_light_pixel_points(pc.pool_word, first, triangle, corners, b, words, points, weights, owners);
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
    // Again after the search only when it laid records out this frame.
    if (pc.again != 0u && light_words.data[pc.pool_word + VELK_POOL_SEARCHED] == 0u) return;
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
    mat4 world = light_instances.data[d.instances_base + velk_ids_slot(d, ids)].world_matrix;
    float scale = max(max(length(world[0].xyz), length(world[1].xyz)), length(world[2].xyz));
    vec3 own = vec3(uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE]),
                    uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 1u]),
                    uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 2u]));
    float radius = uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale;
    float away = length(mat3(world) * own + (world[3].xyz - g.cam_pos.xyz));
    float across = 2.0 * radius * pixels_per_unit / max(away, 1e-3);

    // Pages are laid out once, and own points again when the record has grown to twice the size
    // they were laid out for, whose grids are then too coarse; the search decides how. The points
    // this pixel reads are traced when due, and the coarse light of the records they are in: its
    // own, or those of the records for light only it links to at this pixel's corners.
    if (was != VELK_LIGHT_NONE && was < VELK_LIGHT_ONE_VALUE) {
        if (was >= light_words.data[pc.pool_word + VELK_POOL_PAGES]) return;
        uint first = velk_light_page(pc.pool_word, was);
        if (light_words.data[first + VELK_PAGE_KIND] != VELK_LIGHT_LINKED) {
            float laid_for = uintBitsToFloat(light_words.data[first + VELK_PAGE_ACROSS]);
            if (pc.again == 0u && across > 2.0 * laid_for
                && atomicCompSwap(light_words.data[record + VELK_RECORD_LIGHT], was, VELK_LIGHT_PENDING) == was) {
                velk_light_search(record, was, instance);
                return;
            }
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
 * enough and gets a point per vertex (kLightOwn). Else each of them that has no light yet gets
 * its own points, and each vertex of the record names the three points of the nearest of their
 * triangles, with its weights there (kLightLinked), a thread per vertex. A record that cannot be
 * laid out this frame (no room, or a record for light only another group is laying out) gets
 * its light back and asks again.
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
// How far along the store to look for an instance's records for light only, each way.
const uint kScan = 256u;

// The words of the records placed in the current store of the kept cut `record` is in, from
// `lo` up to `hi`: what the scan for records for light only stays within, since records past
// them are left from earlier frames. False when no cut of the view's top tree holds it.
bool velk_light_store_of(uint record, out uint lo, out uint hi)
{
    lo = 0u;
    hi = 0u;
    if (pc.top_word == 0xFFFFFFFFu) return false;
    uint cuts = light_words.data[pc.top_word];
    for (uint c = 0u; c < cuts; ++c) {
        uint cut = light_words.data[pc.top_word + 4u + c * 8u];
        uint start = light_words.data[cut + VELK_CUT_STORES + light_words.data[cut + VELK_CUT_PARITY]];
        uint end = start + light_words.data[cut + VELK_CUT_PLACED] * 4u;
        if (record >= start && record < end) {
            lo = start;
            hi = end;
            return true;
        }
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
shared uint s_table;        // its triangles' grid words, 0 for none
shared uint s_points;
shared uint s_grid[kMaxTriangles];

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
// its pages named, through index pages past VELK_LIGHT_DIRECT_PAGES. Its points are left to the
// caller. False, and the pages asked for, when there is no room.
bool velk_light_lay_out(uint vertices, uint cluster, uint kind, uint table, uint points, uint index_base, float across,
                        out uint first_page)
{
    uint pool = pc.pool_word;
    uint count = (velk_light_skip_for(table) + points + VELK_LIGHT_PAGE_WORDS - 1u) / VELK_LIGHT_PAGE_WORDS;
    first_page = VELK_LIGHT_NONE;
    if (count > VELK_LIGHT_MAX_PAGES) return false;
    uint index_pages = count > VELK_LIGHT_DIRECT_PAGES ? (count - 1u + VELK_LIGHT_PAGE_WORDS - 1u) / VELK_LIGHT_PAGE_WORDS : 0u;
    uint base;
    if (!velk_light_take(pool, count + index_pages, base)) {
        atomicAdd(light_words.data[pool + VELK_POOL_PAGES_WANTED], count + index_pages);
        return false;
    }
    uint page0 = velk_light_taken(pool, base, 0u);
    uint first = velk_light_page(pool, page0);
    light_words.data[first + VELK_PAGE_VERTICES] = vertices;
    light_words.data[first + VELK_PAGE_KIND] = kind;
    light_words.data[first + VELK_PAGE_POINT_WORDS] = points;
    for (uint k = 1u; k < VELK_LIGHT_DIRECT_PAGES; ++k) {
        light_words.data[first + VELK_PAGE_PAGES - 1u + k] = index_pages == 0u && k < count ? velk_light_taken(pool, base, k) : VELK_LIGHT_NONE;
    }
    for (uint i = 0u; i < index_pages; ++i) {
        uint index = velk_light_taken(pool, base, count + i);
        light_words.data[first + VELK_PAGE_PAGES + i] = index;
        uint at = velk_light_page(pool, index);
        for (uint k = 0u; k < VELK_LIGHT_PAGE_WORDS; ++k) {
            uint nth = 1u + i * VELK_LIGHT_PAGE_WORDS + k;
            light_words.data[at + k] = nth < count ? velk_light_taken(pool, base, nth) : VELK_LIGHT_NONE;
        }
    }
    light_words.data[first + VELK_PAGE_CHANGED] = VELK_LIGHT_NONE;
    light_words.data[first + VELK_PAGE_CLUSTER] = cluster;
    light_words.data[first + VELK_PAGE_RESET] = VELK_LIGHT_NONE;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) light_words.data[first + VELK_PAGE_SLOTS + s] = VELK_LIGHT_NONE;
    light_words.data[first + VELK_PAGE_INDEX] = index_base;
    light_words.data[first + VELK_PAGE_ACROSS] = floatBitsToUint(across);
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

// The grid level of each of `cluster`'s triangles (corners from `first_index`, its vertices at
// `vertex_word`), from how wide each is on screen over the light spacing, into s_grid with where
// its grid points start; as fine as fits the pages a record may have. Returns the grid points.
uint velk_light_grids(uint cluster, uint first_index, uint vertex_word, mat4 world, vec4 offset, vec4 size, GlobalData g)
{
    mat4 vp = g.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
    uint triangles = min(light_words.data[cluster + VELK_LOD_CLUSTER_TRIANGLES], kMaxTriangles);
    uint vertices = light_words.data[cluster + VELK_LOD_CLUSTER_VERTICES];
    for (uint t = 0u; t < triangles; ++t) {
        vec3 p[3];
        for (uint k = 0u; k < 3u; ++k) {
            uint v = velk_mesh_words.data[first_index + t * 3u + k];
            uint vw = vertex_word + v * VELK_VERTEX3D_WORDS;
            vec3 local = vec3(uintBitsToFloat(velk_mesh_words.data[vw]), uintBitsToFloat(velk_mesh_words.data[vw + 1u]),
                              uintBitsToFloat(velk_mesh_words.data[vw + 2u]));
            p[k] = velk_rel_position(world, offset.xyz + local * size.xyz, g);
        }
        float edge = max(max(length(p[1] - p[0]), length(p[2] - p[1])), length(p[0] - p[2]));
        float near = max(min(min(length(p[0]), length(p[1])), length(p[2])), 1e-3);
        float wide = edge * pixels_per_unit / near;
        s_grid[t] = uint(clamp(ceil(wide / pc.light_pixels), 1.0, float(VELK_LIGHT_MAX_GRID)));
    }
    // As fine as fits, two words a point: the finest level that does, found by halving, since
    // fewer points come with every level lower.
    uint room = (VELK_LIGHT_MAX_PAGES * VELK_LIGHT_PAGE_WORDS - velk_light_skip_for(triangles)) / 2u - vertices;
    uint lo = 1u;
    uint hi = VELK_LIGHT_MAX_GRID;
    while (lo < hi) {
        uint mid = (lo + hi + 1u) / 2u;
        uint total = 0u;
        for (uint t = 0u; t < triangles; ++t) total += velk_light_grid_points(min(s_grid[t], mid));
        if (total <= room) lo = mid;
        else hi = mid - 1u;
    }
    uint finest = lo;
    uint at = 0u;
    for (uint t = 0u; t < triangles; ++t) {
        uint n = min(s_grid[t], max(finest, 1u));
        s_grid[t] = (n << 24u) | at;
        at += velk_light_grid_points(n);
    }
    return at;
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
        // within the records placed in its store.
        uint store_lo;
        uint store_hi;
        bool in_store = velk_light_store_of(record, store_lo, store_hi);
        for (int dir = -1; in_store && dir <= 1; dir += 2) {
            for (uint k = 1u; k <= kScan; ++k) {
                if (dir < 0 && record < store_lo + k * VELK_RECORD_END) break;
                if (dir > 0 && record + k * VELK_RECORD_END >= store_hi) break;
                uint other = dir < 0 ? record - k * VELK_RECORD_END : record + k * VELK_RECORD_END;
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
                                            VELK_LIGHT_NONE, 0.0, laid)) {
                        light_words.data[other + VELK_RECORD_LIGHT] = VELK_LIGHT_NONE;
                        again = true;
                        continue;
                    }
                    uint other_first = velk_light_page(pool, laid);
                    for (uint p = 0u; p < other_vertices; ++p) {
                        uint at = velk_light_own_word(pool, other_first, p);
                        light_words.data[at] = VELK_LIGHT_NONE;
                        light_words.data[at + 1u] = VELK_LIGHT_NONE;
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
            // Coarse enough: its own points, with a grid in each triangle wider than the light
            // spacing, as fine as that triangle is wide, its corners found through its draw's
            // indices. All threads make them untraced, below.
            uint index_base = draw == 0xFFFFFFFFu ? VELK_LIGHT_NONE : velk_draw_data.data[draw].index_base;
            uint triangles = min(light_words.data[cluster + VELK_LOD_CLUSTER_TRIANGLES], kMaxTriangles);
            uint grid_points = index_base == VELK_LIGHT_NONE
                ? 0u
                : velk_light_grids(cluster, index_base + light_words.data[cluster + VELK_LOD_CLUSTER_INDEX],
                                   light_words.data[cluster + VELK_LOD_CLUSTER_VERTEX],
                                   inst.world_matrix, inst.offset, inst.size, g);
            uint table = grid_points != 0u ? triangles : 0u;
            uint kind = table != 0u ? VELK_LIGHT_GRIDDED | triangles : VELK_LIGHT_OWN;
            // How wide it is now, to lay it out again when it has grown to twice that.
            mat4 vp = g.view_projection;
            float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
            float scale = max(max(length(inst.world_matrix[0].xyz), length(inst.world_matrix[1].xyz)), length(inst.world_matrix[2].xyz));
            vec3 own = vec3(uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE]),
                            uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 1u]),
                            uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 2u]));
            float away = length(mat3(inst.world_matrix) * own + (inst.world_matrix[3].xyz - g.cam_pos.xyz));
            float across = 2.0 * uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale * pixels_per_unit / max(away, 1e-3);
            uint laid;
            if (velk_light_lay_out(vertices, cluster, kind, table, 2u * (vertices + grid_points), index_base, across, laid)) {
                s_page = laid;
                s_first = velk_light_page(pool, laid);
                s_table = table;
                s_points = vertices + grid_points;
                s_own = true;
            } else {
                light_words.data[record + VELK_RECORD_LIGHT] = was;
            }
        } else {
            uint laid;
            if (velk_light_lay_out(vertices, cluster, VELK_LIGHT_LINKED, 0u, vertices * 3u, VELK_LIGHT_NONE, 0.0, laid)) {
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
    // Its own points: their triangles' grids and the points, untraced, then the record names them
    // and they are traced this frame.
    if (s_own) {
        for (uint t = lane; t < s_table; t += 64u) {
            light_words.data[velk_light_word(pool, s_first, VELK_PAGE_GRIDS + t)] = s_grid[t];
        }
        for (uint p = lane; p < s_points; p += 64u) {
            uint at = velk_light_own_word(pool, s_first, p);
            light_words.data[at] = VELK_LIGHT_NONE;
            light_words.data[at + 1u] = VELK_LIGHT_NONE;
        }
        memoryBarrierBuffer();
        barrier();
        if (lane == 0u) {
            light_words.data[record + VELK_RECORD_LIGHT] = s_page;
            velk_light_trace_now(s_page, instance);
            // For the log, why: no light before, laid out again having grown, or one value before.
            uint why = was == VELK_LIGHT_NONE ? VELK_POOL_SUM_LAID_NEW
                     : was < VELK_LIGHT_ONE_VALUE ? VELK_POOL_SUM_LAID_GROWN : VELK_POOL_SUM_LAID_FROM_ONE;
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
 * Marks the pages the records of a kept cut's current store name, and those of the records for
 * light only their vertices link to, and counts its records with pages and with one value. A
 * thread per record, dispatched by the cut's drawing counts.
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

// A record's first page and its others, and its index pages when it has them.
void velk_light_mark_record(uint light)
{
    velk_light_mark(light);
    uint first = velk_light_page(pc.pool_word, light);
    uint count = velk_light_page_count(first);
    if (count <= VELK_LIGHT_DIRECT_PAGES) {
        for (uint k = 1u; k < count; ++k) velk_light_mark(light_words.data[first + VELK_PAGE_PAGES - 1u + k]);
        return;
    }
    uint index_pages = (count - 1u + VELK_LIGHT_PAGE_WORDS - 1u) / VELK_LIGHT_PAGE_WORDS;
    for (uint i = 0u; i < index_pages && i + 1u < VELK_LIGHT_DIRECT_PAGES; ++i) {
        uint index = light_words.data[first + VELK_PAGE_PAGES + i];
        velk_light_mark(index);
        if (index >= light_words.data[pc.pool_word + VELK_POOL_PAGES]) continue;
        uint at = velk_light_page(pc.pool_word, index);
        for (uint k = 0u; k < VELK_LIGHT_PAGE_WORDS && 1u + i * VELK_LIGHT_PAGE_WORDS + k < count; ++k) {
            velk_light_mark(light_words.data[at + k]);
        }
    }
}

void main()
{
    uint cut = pc.cut_word;
    uint index = gl_GlobalInvocationID.x;
    if (index >= light_words.data[cut + VELK_CUT_PLACED]) return;
    uint parity = light_words.data[cut + VELK_CUT_PARITY];
    uint light = light_words.data[light_words.data[cut + VELK_CUT_STORES + parity] + index * VELK_RECORD_END + VELK_RECORD_LIGHT];
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
        // For the log: how many points it has, whether a pixel read them since the last mark,
        // and how records' points are spread.
        uint pool = pc.pool_word;
        uint points = light_words.data[first + VELK_PAGE_POINT_WORDS] / 2u;
        atomicAdd(light_words.data[pool + VELK_POOL_MARK_POINTS], points);
        if (light_words.data[first + VELK_PAGE_READ] != 0u) atomicAdd(light_words.data[pool + VELK_POOL_MARK_READ_POINTS], points);
        light_words.data[first + VELK_PAGE_READ] = 0u;
        atomicMax(light_words.data[pool + VELK_POOL_MARK_MOST_POINTS], points);
        uint bucket = points < 256u ? 0u : points < 1024u ? 1u : points < 4096u ? 2u : points < 16384u ? 3u : 4u;
        atomicAdd(light_words.data[pool + VELK_POOL_MARK_BUCKETS + bucket], 1u);
        if (points >= 4096u) atomicAdd(light_words.data[pool + VELK_POOL_MARK_BIG_POINTS], points);
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
 * Puts every page no mark says is in use on the free stack, and clears the marks for the next
 * frame. A thread per 32 pages. Starts and ends the frame's marking: with `begin` set, one
 * thread empties the stack, the mark's counts and the search list instead.
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
            for (uint k = 0u; k < VELK_POOL_LAST_MARK - VELK_POOL_MARK_POINTS; ++k) {
                light_words.data[pool + VELK_POOL_LAST_MARK + k] = light_words.data[pool + VELK_POOL_MARK_POINTS + k];
                light_words.data[pool + VELK_POOL_MARK_POINTS + k] = 0u;
            }
            light_words.data[pool + VELK_POOL_FIRST_POINTS] = 0u;
            light_words.data[pool + VELK_POOL_LATER_POINTS] = 0u;
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
    constexpr uint64_t segment_bytes = uint64_t(kLightSegmentPages) * kLightPageWords * sizeof(uint32_t);
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
    const uint64_t list_word = uint64_t(kLightPoolHeaderWords) + max_pages + max_pages / 32u;
    const uint64_t trace_word = list_word + uint64_t(kLightSearchCapacity) * kLightSearchWords;
    const uint64_t points_word = trace_word + uint64_t(kLightTraceCapacity) * kLightTraceWords;
    const uint64_t point_list_words = uint64_t(kLightPointCapacity) * kLightPointWords;
    const uint64_t words = points_word + 3u * point_list_words;
    pool.header = arena.alloc(words * sizeof(uint32_t));
    if (!pool.header.valid()) {
        return false;
    }
    const uint32_t base = pool.word();
    vector<uint32_t> initial;
    initial.resize(kLightPoolHeaderWords + max_pages / 32u, 0u);
    initial[kLightPoolStackWord] = base + kLightPoolHeaderWords;
    initial[kLightPoolMarksWord] = base + kLightPoolHeaderWords + max_pages;
    initial[kLightPoolSearchWord] = base + static_cast<uint32_t>(list_word);
    initial[kLightPoolTraceWord] = base + static_cast<uint32_t>(trace_word);
    // The first frame's budget in rays, from an assumed walk, until a frame has measured one.
    initial[kLightPoolRayBudget] = kLightTraceStepsBudget / kLightAssumedSteps;
    initial[kLightPoolStepBudget] = kLightTraceStepsBudget;
    initial[kLightPoolFirstPointsWord] = base + static_cast<uint32_t>(points_word);
    initial[kLightPoolLaterPointsWord] = base + static_cast<uint32_t>(points_word + point_list_words);
    initial[kLightPoolCoarsePointsWord] = base + static_cast<uint32_t>(points_word + 2u * point_list_words);
    // The header, then the marks, all clear.
    arena.write_at(pool.header.offset(), initial.data(), kLightPoolHeaderWords * sizeof(uint32_t));
    arena.write_at(pool.header.offset() + (uint64_t(kLightPoolHeaderWords) + max_pages) * sizeof(uint32_t),
                   initial.data() + kLightPoolHeaderWords, uint64_t(max_pages / 32u) * sizeof(uint32_t));
    pool.segments.clear();
    return true;
}

/// Adds @p count segments to @p pool, as many as it may have at most, and says so in its header.
/// Returns how many were added.
inline uint32_t grow_light_pool(LightPool& pool, IGpuArena& arena, uint32_t count)
{
    uint32_t added = 0;
    while (added < count && pool.segments.size() < pool.max_segments) {
        ArenaRegion segment = arena.alloc(uint64_t(kLightSegmentPages) * kLightPageWords * sizeof(uint32_t));
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
