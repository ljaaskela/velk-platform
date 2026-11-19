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

/// A kept cut's header (kCut, VELK_CUT), in the culling arena: see instance_cull.h's description
/// of kept cuts. Two stores of records flip each frame, and with them the node and ray list stores.
#define VELK_CUT_HEADER(X, cp, gp)                                                                 \
    X(cp, gp, CopyGroups, COPY_GROUPS, 0)       /* group counts of the copy dispatch: partitions keeping their records */ \
    X(cp, gp, ChooseGroups, CHOOSE_GROUPS, 4)   /* of the choosing dispatches: chunks choosing again */ \
    X(cp, gp, DrawGroups, DRAW_GROUPS, 8)       /* of the drawing dispatch: a group per 64 records */ \
    X(cp, gp, Parity, PARITY, 12)               /* which store is current, 0 or 1 */           \
    X(cp, gp, Asked, ASKED, 13)                 /* records the current store was asked for, past its room too */ \
    X(cp, gp, ChoseAgain, CHOSE_AGAIN, 14)      /* how many times partitions chose again, for the log */ \
    X(cp, gp, MostWanted, MOST_WANTED, 15)      /* the most records a frame wanted, when more than fit */ \
    X(cp, gp, Capacity, CAPACITY, 16)           /* records a store holds */                    \
    X(cp, gp, Stores, STORES, 17)               /* the two record stores' words */             \
    X(cp, gp, Partitions, PARTITIONS, 19)       /* partitions */                               \
    X(cp, gp, KeepWord, KEEP_WORD, 20)          /* the word of the keep list: a partition index each */ \
    X(cp, gp, ChooseWord, CHOOSE_WORD, 21)      /* the word of the choose list: kCullChunkListWords a chunk */ \
    X(cp, gp, StatesWord, STATES_WORD, 22)      /* the word of the partitions' states (VELK_PART) */ \
    X(cp, gp, ChunkCountsWord, CHUNK_COUNTS_WORD, 23) /* the word of the choose list's chunks' counts and first records */ \
    X(cp, gp, Nodes, NODES, 24)                 /* tree nodes in the current node store */     \
    X(cp, gp, NodeCapacity, NODE_CAPACITY, 25)  /* tree nodes a node store holds */            \
    X(cp, gp, NodeStores, NODE_STORES, 26)      /* the two node stores' words */               \
    X(cp, gp, Placed, PLACED, 28)               /* the end of the records placed in the current store */ \
    X(cp, gp, TakeoverFrame, TAKEOVER_FRAME, 29) /* the frame of the takeover table, counted by the begin pass */ \
    X(cp, gp, TakeoverWord, TAKEOVER_WORD, 30)  /* the takeover table's word (VELK_TAKEOVER a slot) */ \
    X(cp, gp, TakeoverSlots, TAKEOVER_SLOTS, 31) /* and its slots, a power of two */           \
    X(cp, gp, LightOnlyRecords, LIGHT_ONLY_RECORDS, 32) /* records for light only in the current store */ \
    X(cp, gp, LastLightOnlyRecords, LAST_LIGHT_ONLY_RECORDS, 33)                               \
    X(cp, gp, Rewritten, REWRITTEN, 34)         /* records written by partitions choosing again this frame */ \
    X(cp, gp, TookOver, TOOK_OVER, 35)          /* of those, how many took over an old record's light */ \
    X(cp, gp, LastRewritten, LAST_REWRITTEN, 36)                                               \
    X(cp, gp, LastTookOver, LAST_TOOK_OVER, 37)                                                \
    X(cp, gp, RayStores, RAY_STORES, 38)        /* the two ray list stores' words */           \
    X(cp, gp, LongestWalk, LONGEST_WALK, 40)    /* the most hierarchy nodes one thread walked choosing this frame */ \
    X(cp, gp, MostChosen, MOST_CHOSEN, 41)      /* the most records one walk chose */          \
    X(cp, gp, LastLongestWalk, LAST_LONGEST_WALK, 42)                                          \
    X(cp, gp, LastMostChosen, LAST_MOST_CHOSEN, 43)                                            \
    X(cp, gp, RayMarksWord, RAY_MARKS_WORD, 44) /* the word of the ray marks, a bit a record of the current store */ \
    X(cp, gp, End, END, 45)

/// A partition's state in a kept cut (kPart, VELK_PART), at the cut's STATES_WORD.
#define VELK_CUT_PART(X, cp, gp)                                                                   \
    X(cp, gp, From, FROM, 0)                    /* where in the world it chose from, three floats */ \
    X(cp, gp, ChunkFirst, CHUNK_FIRST, 3)       /* its table entry's first chunk word when it chose */ \
    X(cp, gp, Chunks, CHUNKS, 4)                /* and its chunk count */                      \
    X(cp, gp, First, FIRST, 5)                  /* its first record in the current store */    \
    X(cp, gp, Count, COUNT, 6)                  /* its record count there */                   \
    X(cp, gp, LastFirst, LAST_FIRST, 7)         /* its first record in the last store */       \
    X(cp, gp, Choosing, CHOOSING, 8)            /* 1 while choosing this frame */              \
    X(cp, gp, ChunkStart, CHUNK_START, 9)       /* while choosing, where its chunks start in the choose list */ \
    X(cp, gp, NodeFirst, NODE_FIRST, 10)        /* its first tree node in the current node store */ \
    X(cp, gp, LastNodeFirst, LAST_NODE_FIRST, 11) /* in the last */                            \
    X(cp, gp, QuadTree, QUAD_TREE, 12)          /* its table entry's quad tree word when it chose */ \
    X(cp, gp, OldCount, OLD_COUNT, 15)          /* while choosing again, how many records it had, 0 for none */ \
    X(cp, gp, RayCount, RAY_COUNT, 16)          /* how many records rays trace (its ray list) */ \
    X(cp, gp, Looked, LOOKED, 17)               /* which way the camera looked when it chose, three floats */ \
    X(cp, gp, OutOfView, OUT_OF_VIEW, 20)       /* 1 when it chose clusters at the light's level for being out of view */ \
    X(cp, gp, End, END, 24)

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

VELK_CUT_HEADER(VELK_LAYOUT_CPP_WORD, Cut, CUT)
VELK_CUT_PART(VELK_LAYOUT_CPP_WORD, Part, PART)
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
    VELK_CUT_HEADER(VELK_LAYOUT_GLSL_WORD, Cut, CUT)                                               \
    VELK_CUT_PART(VELK_LAYOUT_GLSL_WORD, Part, PART)                                               \
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
