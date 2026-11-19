#ifndef VELK_RENDER_FRAME_CULL_LAYOUT_H
#define VELK_RENDER_FRAME_CULL_LAYOUT_H

#include <cstdint>

namespace velk {

/// The words of the culling's lists and kept cuts in the culling arena, each named once: in C++
/// as k<Prefix><Name>, and in the shaders as VELK_<PREFIX>_<NAME>, a #define velk.glsl carries
/// (VELK_CULL_LAYOUT_GLSL), so that both read the same layout. A list is
/// X(c++ prefix, shader prefix, c++ name, shader name, word), in order; a name may share its word
/// with another where the word means different things for different records.
#define VELK_LAYOUT_CPP_WORD(cp, gp, cpp, glsl, word) inline constexpr uint32_t k##cp##cpp = word;
#define VELK_LAYOUT_GLSL_WORD(cp, gp, cpp, glsl, word) "#define VELK_" #gp "_" #glsl " " #word "u\n"

/// A record of a kept cut (kCutRecord, VELK_RECORD). See kCutHeaderWords.
#define VELK_CUT_RECORD(X, cp, gp)                                                                 \
    X(cp, gp, Entry, ENTRY, 0)          /* its mesh's cluster entry, or kCutQuad, kCutRayQuad, kCullNoSlot */ \
    X(cp, gp, Cluster, CLUSTER, 1)      /* its cluster in the entry's hierarchy */                 \
    X(cp, gp, MeshTable, MESH_TABLE, 1) /* for a whole mesh (entry kCullNoSlot), its mesh's table */ \
    X(cp, gp, QuadNode, QUAD_NODE, 1)   /* for a quad, its node in its partition's quad tree */    \
    X(cp, gp, Slot, SLOT, 2)            /* its instance's slot */                                  \
    X(cp, gp, QuadTree, QUAD_TREE, 2)   /* for a quad, the word of its partition's quad tree */    \
    X(cp, gp, Light, LIGHT, 3)          /* its light: see light_pool.h */                          \
    X(cp, gp, End, END, 4)

/// A slot of a kept cut's takeover table (kTakeover, VELK_TAKEOVER): the old records of the
/// partitions choosing again, for each new record to find the one that was the same.
#define VELK_TAKEOVER(X, cp, gp)                                                                   \
    X(cp, gp, Frame, FRAME, 0)          /* the frame it was filled in; another frame's is empty */ \
    X(cp, gp, Record, RECORD, 1)        /* the old record's word */                                \
    X(cp, gp, End, END, 2)

/// A visual's chosen list (kChosenList, VELK_CHOSEN_LIST): what it draws this frame by cluster.
#define VELK_CHOSEN_LIST(X, cp, gp)                                                                \
    X(cp, gp, Count, COUNT, 0)          /* records added, past its room too */                     \
    X(cp, gp, Groups, GROUPS, 1)        /* the group counts of the dispatch over them, three words */ \
    X(cp, gp, Records, RECORDS, 4)      /* the first record */
/// A record of a chosen list (kChosen, VELK_CHOSEN).
#define VELK_CHOSEN(X, cp, gp)                                                                     \
    X(cp, gp, Entry, ENTRY, 0)          /* its mesh's cluster entry */                             \
    X(cp, gp, Cluster, CLUSTER, 1)      /* its cluster */                                          \
    X(cp, gp, Slot, SLOT, 2)            /* its instance's slot */                                  \
    X(cp, gp, CutRecord, CUT_RECORD, 3) /* the word of the kept cut record it is from, 0xFFFFFFFF for none */ \
    X(cp, gp, End, END, 4)

/// The list of clusters drawn in compute (kTinyList, VELK_TINY_LIST): instance_tiny_draw_src.
#define VELK_TINY_LIST(X, cp, gp)                                                                  \
    X(cp, gp, Count, COUNT, 0)          /* records added, past its room too */                     \
    X(cp, gp, Groups, GROUPS, 1)        /* the group counts of the dispatch that draws them, three words */ \
    X(cp, gp, Drawn, DRAWN, 4)          /* how many of them are drawn */                           \
    X(cp, gp, Records, RECORDS, 8)      /* the first record, after three spare words */
/// A record of the tiny list (kTiny, VELK_TINY).
#define VELK_TINY(X, cp, gp)                                                                       \
    X(cp, gp, Draw, DRAW, 0)            /* its draw data record */                                 \
    X(cp, gp, Cluster, CLUSTER, 1)      /* the word of its cluster in the level of detail table */ \
    X(cp, gp, Slot, SLOT, 2)            /* its instance's slot under that draw */                  \
    X(cp, gp, CutRecord, CUT_RECORD, 3) /* the word of the kept cut record it is from, 0xFFFFFFFF for none */ \
    X(cp, gp, End, END, 4)

/// The list of what failed the first occlusion test (kRetestList, VELK_RETEST_LIST).
#define VELK_RETEST_LIST(X, cp, gp)                                                                \
    X(cp, gp, Count, COUNT, 0)          /* records added, past its room too */                     \
    X(cp, gp, Records, RECORDS, 1)      /* the first record */
/// A record of the retest list (kRetest, VELK_RETEST). What does not fit is drawn.
#define VELK_RETEST(X, cp, gp)                                                                     \
    X(cp, gp, Instance, INSTANCE, 0)    /* its instance's index in the instance arena */           \
    X(cp, gp, Slot, SLOT, 1)            /* its slot */                                             \
    X(cp, gp, Table, TABLE, 2)          /* its table */                                            \
    X(cp, gp, Entry, ENTRY, 3)          /* an entry, kCullNoSlot for the whole instance */         \
    X(cp, gp, Cluster, CLUSTER, 4)      /* and a cluster */                                        \
    X(cp, gp, CutRecord, CUT_RECORD, 5) /* the kept cut record it is from */                       \
    X(cp, gp, ChosenWord, CHOSEN_WORD, 6) /* where the visual's chosen clusters go */              \
    X(cp, gp, ChosenCapacity, CHOSEN_CAPACITY, 7) /* and how many fit */                           \
    X(cp, gp, From, FROM, 8)            /* where its levels of detail are chosen from, three floats */ \
    X(cp, gp, End, END, 12)             /* after a spare word */

VELK_CUT_RECORD(VELK_LAYOUT_CPP_WORD, CutRecord, RECORD)
VELK_TAKEOVER(VELK_LAYOUT_CPP_WORD, Takeover, TAKEOVER)
VELK_CHOSEN_LIST(VELK_LAYOUT_CPP_WORD, ChosenList, CHOSEN_LIST)
VELK_CHOSEN(VELK_LAYOUT_CPP_WORD, Chosen, CHOSEN)
VELK_TINY_LIST(VELK_LAYOUT_CPP_WORD, TinyList, TINY_LIST)
VELK_TINY(VELK_LAYOUT_CPP_WORD, Tiny, TINY)
VELK_RETEST_LIST(VELK_LAYOUT_CPP_WORD, RetestList, RETEST_LIST)
VELK_RETEST(VELK_LAYOUT_CPP_WORD, Retest, RETEST)

/// Every layout's words, for velk.glsl.
#define VELK_CULL_LAYOUT_GLSL                                                                      \
    VELK_CUT_RECORD(VELK_LAYOUT_GLSL_WORD, CutRecord, RECORD)                                      \
    VELK_TAKEOVER(VELK_LAYOUT_GLSL_WORD, Takeover, TAKEOVER)                                       \
    VELK_CHOSEN_LIST(VELK_LAYOUT_GLSL_WORD, ChosenList, CHOSEN_LIST)                               \
    VELK_CHOSEN(VELK_LAYOUT_GLSL_WORD, Chosen, CHOSEN)                                             \
    VELK_TINY_LIST(VELK_LAYOUT_GLSL_WORD, TinyList, TINY_LIST)                                     \
    VELK_TINY(VELK_LAYOUT_GLSL_WORD, Tiny, TINY)                                                   \
    VELK_RETEST_LIST(VELK_LAYOUT_GLSL_WORD, RetestList, RETEST_LIST)                               \
    VELK_RETEST(VELK_LAYOUT_GLSL_WORD, Retest, RETEST)

} // namespace velk

#endif // VELK_RENDER_FRAME_CULL_LAYOUT_H
