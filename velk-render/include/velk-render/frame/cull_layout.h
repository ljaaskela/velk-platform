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

/// A batch's culling table, a row per mesh (kTable, VELK_TABLE). See instance_cull_compute_src.
#define VELK_BATCH_TABLE(X, cp, gp)                                                                \
    X(cp, gp, Center, CENTER, 0)                /* its bounding sphere's centre, three floats */ \
    X(cp, gp, Radius, RADIUS, 3)                /* and radius */                               \
    X(cp, gp, Count, COUNT, 4)                  /* visible slots the first test listed */      \
    X(cp, gp, List, LIST, 5)                    /* the word of the list of visible slots */    \
    X(cp, gp, Capacity, CAPACITY, 6)            /* the list's capacity */                      \
    X(cp, gp, FirstEntry, FIRST_ENTRY, 8)       /* the word of its primitives' cluster entries (VELK_CLUSTER_ENTRY) */ \
    X(cp, gp, EntryCount, ENTRY_COUNT, 9)       /* and how many */                             \
    X(cp, gp, SecondCount, SECOND_COUNT, 11)    /* visible slots the second test listed after them */ \
    X(cp, gp, End, END, 12)

/// A cluster in a level of detail hierarchy's table (kLodCluster, VELK_LOD_CLUSTER). Mirrors
/// MeshLodCluster. A sphere is three floats of centre, then the radius.
#define VELK_LOD_CLUSTER(X, cp, gp)                                                                \
    X(cp, gp, Index, INDEX, 0)                  /* the word of its first index */              \
    X(cp, gp, Triangles, TRIANGLES, 1)                                                         \
    X(cp, gp, Level, LEVEL, 2)                  /* 0 is the full mesh */                       \
    X(cp, gp, Sphere, SPHERE, 3)                /* its group's sphere */                       \
    X(cp, gp, Error, ERROR, 7)                                                                 \
    X(cp, gp, ParentSphere, PARENT_SPHERE, 8)                                                  \
    X(cp, gp, ParentError, PARENT_ERROR, 12)                                                   \
    X(cp, gp, OwnSphere, OWN_SPHERE, 13)        /* around its own triangles */                 \
    X(cp, gp, Largest, LARGEST, 17)             /* the diagonal of its largest triangle's box */ \
    X(cp, gp, Vertex, VERTEX, 18)               /* the word of its first vertex */             \
    X(cp, gp, Vertices, VERTICES, 19)                                                          \
    X(cp, gp, Uv1, UV1, 20)                                                                    \
    X(cp, gp, FullError, FULL_ERROR, 21)        /* its error as built */                       \
    X(cp, gp, Mark, MARK, 22)                   /* the word of the mark of the group it was made from */ \
    X(cp, gp, OwnMark, OWN_MARK, 23)            /* the word of its own group's mark */         \
    X(cp, gp, Color, COLOR, 24)                                                                \
    X(cp, gp, Words, WORDS, 28)

/// A node of a partition's quad tree (kQuadNode, VELK_QUAD_NODE), in world space; also what an
/// instance of a mesh is in it, in the mesh's space. See kQuadLeavesOnly in instance_cull.h.
#define VELK_QUAD_NODE(X, cp, gp)                                                                  \
    X(cp, gp, Center, CENTER, 0)                /* three floats */                             \
    X(cp, gp, U, U, 3)                          /* its two half axes, three floats each; its normal is their cross product */ \
    X(cp, gp, V, V, 6)                                                                         \
    X(cp, gp, Color, COLOR, 9)                  /* RGBA8, red in the low byte */               \
    X(cp, gp, Radius, RADIUS, 10)               /* of a sphere at its centre around it and every quad under it, negative for an empty node */ \
    X(cp, gp, Light, LIGHT, 11)                 /* the light of the members under it, one value or kLightNone */ \
    X(cp, gp, Words, WORDS, 12)

/// A view's quad list (kQuadList, VELK_QUAD_LIST), in the culling arena: the quad draw's
/// indirect arguments and count, then the word of each quad drawn, and after the list's capacity
/// the word of each one's kept cut record.
#define VELK_QUAD_LIST(X, cp, gp)                                                                  \
    X(cp, gp, Vertices, VERTICES, 0)            /* 4 */                                        \
    X(cp, gp, Count, COUNT, 1)                  /* the instance count: quads drawn */          \
    X(cp, gp, FirstVertex, FIRST_VERTEX, 2)                                                    \
    X(cp, gp, FirstInstance, FIRST_INSTANCE, 3)                                                \
    X(cp, gp, DrawCount, DRAW_COUNT, 4)         /* 1 */                                        \
    X(cp, gp, Draw, DRAW, 5)                    /* the quad draw's record in the draw data, which rays name */ \
    X(cp, gp, InCuts, IN_CUTS, 7)               /* quads in the view's cuts, in or out of view, for the log */ \
    X(cp, gp, Quads, QUADS, 8)

/// An item (kPlainItem, VELK_PLAIN_ITEM), what a kept cut record names. See kPlainItemWords' use
/// in instance_cull.h (InstanceCullPass::together).
#define VELK_PLAIN_ITEM(X, cp, gp)                                                                 \
    X(cp, gp, Instance, INSTANCE, 0)            /* its index in the instance arena, kCullNoSlot for none in its raster region */ \
    X(cp, gp, Slot, SLOT, 1)                    /* in its batch's instances */                 \
    X(cp, gp, Table, TABLE, 2)                  /* its batch's culling table */                \
    X(cp, gp, Draw, DRAW, 3)                    /* its draw's index in the draw data */        \
    X(cp, gp, Kind, KIND, 4)                    /* what rays test it as: kRayShapeMesh, else an RtShape kind */ \
    X(cp, gp, Mesh, MESH, 5)                    /* its mesh's ray data, kInvalidMeshStaticBase for none */ \
    X(cp, gp, Motion, MOTION, 6)                /* how far a motion function can move it, a float */ \
    X(cp, gp, Words, WORDS, 7)

/// An instanced visual's item descriptor (kItemDescriptor, VELK_ITEM_DESCRIPTOR), then a
/// VELK_ITEM_MESH for each of its meshes. See InstanceCullPass::item_descriptors.
#define VELK_ITEM_DESCRIPTOR(X, cp, gp)                                                            \
    X(cp, gp, First, FIRST, 0)                  /* its first item */                           \
    X(cp, gp, Slots, SLOTS, 1)                                                                 \
    X(cp, gp, Instances, INSTANCES, 2)          /* its instances' base */                      \
    X(cp, gp, SlotTable, SLOT_TABLE, 3)         /* its slot table's word, 0 for none */        \
    X(cp, gp, Meshes, MESHES, 4)                                                               \
    X(cp, gp, Table, TABLE, 5)                  /* its culling table */                        \
    X(cp, gp, Motion, MOTION, 6)                /* how far it can move, a float */             \
    X(cp, gp, ObjectBase, OBJECT_BASE, 7)       /* its first object id; slot i's is that plus i */ \
    X(cp, gp, HeaderWords, HEADER_WORDS, 8)

/// A mesh of an item descriptor (kItemMesh, VELK_ITEM_MESH): an item's words from Draw on.
#define VELK_ITEM_MESH(X, cp, gp)                                                                  \
    X(cp, gp, Draw, DRAW, 0)                                                                   \
    X(cp, gp, Kind, KIND, 1)                                                                   \
    X(cp, gp, Mesh, MESH, 2)                                                                   \
    X(cp, gp, Words, WORDS, 3)

/// A node of a partition's tree for rays, or of a view's top tree (kCutNode, VELK_CUT_NODE). See
/// kCutLeaf in instance_cull.h for its references.
#define VELK_CUT_NODE(X, cp, gp)                                                                   \
    X(cp, gp, LeftMin, LEFT_MIN, 0)             /* its children's boxes in world space, three floats each */ \
    X(cp, gp, LeftMax, LEFT_MAX, 3)                                                            \
    X(cp, gp, RightMin, RIGHT_MIN, 6)                                                          \
    X(cp, gp, RightMax, RIGHT_MAX, 9)                                                          \
    X(cp, gp, LeftRef, LEFT_REF, 12)            /* a reference to each child */                \
    X(cp, gp, RightRef, RIGHT_REF, 13)                                                         \
    X(cp, gp, Level, LEVEL, 14)                 /* 1 over leaves */                            \
    X(cp, gp, Root, ROOT, 15)                   /* 1 for the root */                           \
    X(cp, gp, Words, WORDS, 16)

/// An entry of a partition's ray list (kCutRay, VELK_CUT_RAY): a record rays trace and a sphere
/// around it in world space, which a ray is tested against before the record's instance is read.
#define VELK_CUT_RAY(X, cp, gp)                                                                    \
    X(cp, gp, Index, INDEX, 0)                  /* the record's place in its cut's pool */     \
    X(cp, gp, Center, CENTER, 1)                /* its sphere's centre, three floats */        \
    X(cp, gp, Radius, RADIUS, 4)                                                               \
    X(cp, gp, Words, WORDS, 5)

/// A view's top tree for rays (kRayTop, VELK_RAY_TOP), then a VELK_RAY_TOP_CUT a cut, a
/// VELK_RAY_TOP_LEAF a leaf, and its nodes, VELK_CUT_NODE each.
#define VELK_RAY_TOP(X, cp, gp)                                                                    \
    X(cp, gp, Cuts, CUTS, 0)                                                                   \
    X(cp, gp, Leaves, LEAVES, 1)                                                               \
    X(cp, gp, Nodes, NODES, 2)                                                                 \
    X(cp, gp, Root, ROOT, 3)                    /* the root node's index */                    \
    X(cp, gp, Built, BUILT, 4)                  /* 0 until its nodes are built over its leaves */ \
    X(cp, gp, DiagBuilds, DIAG_BUILDS, 5)       /* TEMPORARY: builds since the log, and of them */ \
    X(cp, gp, DiagFallbacks, DIAG_FALLBACKS, 6) /* those that fell back to a balanced tree */  \
    X(cp, gp, HeaderWords, HEADER_WORDS, 7)

/// A cut of a view's top tree (kRayTopCut, VELK_RAY_TOP_CUT).
#define VELK_RAY_TOP_CUT(X, cp, gp)                                                                \
    X(cp, gp, Cut, CUT, 0)                      /* the kept cut's word */                      \
    X(cp, gp, InstancesBase, INSTANCES_BASE, 1)                                                \
    X(cp, gp, Items, ITEMS, 2)                  /* its plain items' word, or kCullNoSlot */    \
    X(cp, gp, Partitions, PARTITIONS, 3)                                                       \
    X(cp, gp, FirstLeaf, FIRST_LEAF, 4)                                                        \
    X(cp, gp, Moves, MOVES, 5)                  /* 1 if a motion function moves its instances */ \
    X(cp, gp, Quads, QUADS, 6)                  /* the view's quad list, or kCullNoSlot */     \
    X(cp, gp, Words, WORDS, 8)

/// A leaf of a view's top tree (kRayTopLeaf, VELK_RAY_TOP_LEAF): a partition of a cut.
#define VELK_RAY_TOP_LEAF(X, cp, gp)                                                               \
    X(cp, gp, Cut, CUT, 0)                      /* the cut's index */                          \
    X(cp, gp, Partition, PARTITION, 1)                                                         \
    X(cp, gp, Words, WORDS, 2)

/// A screen probe (kProbe, VELK_PROBE).
#define VELK_PROBE(X, cp, gp)                                                                      \
    X(cp, gp, Light, LIGHT, 0)                  /* the light arriving, three floats */         \
    X(cp, gp, Position, POSITION, 3)            /* where it is, three floats */                \
    X(cp, gp, Normal, NORMAL, 6)                /* octahedral */                               \
    X(cp, gp, Frames, FRAMES, 7)                /* how many frames it holds, a float */        \
    X(cp, gp, Words, WORDS, 8)

/// An entry of the light pool's trace list (kTraceEntry, VELK_TRACE_ENTRY).
#define VELK_TRACE_ENTRY(X, cp, gp)                                                                \
    X(cp, gp, How, HOW, 0)                      /* what it is (kLightTrace*) */                \
    X(cp, gp, At, AT, 1)                        /* the page, record or node it is in */        \
    X(cp, gp, Instance, INSTANCE, 2)                                                           \
    X(cp, gp, Table, TABLE, 3)                  /* its cluster's or mesh's table word */       \
    X(cp, gp, Words, WORDS, 4)

/// An entry of the light pool's search list (kSearchEntry, VELK_SEARCH_ENTRY).
#define VELK_SEARCH_ENTRY(X, cp, gp)                                                               \
    X(cp, gp, Record, RECORD, 0)                                                               \
    X(cp, gp, Was, WAS, 1)                      /* the record's light before the search */     \
    X(cp, gp, Instance, INSTANCE, 2)                                                           \
    X(cp, gp, Words, WORDS, 3)

/// An entry of the light pool's split list (kSplitEntry, VELK_SPLIT_ENTRY): an ask for a
/// triangle's root piece, or for a piece's children.
#define VELK_SPLIT_ENTRY(X, cp, gp)                                                                \
    X(cp, gp, Page, PAGE, 0)                    /* the piece's page, or for a root its record's \
                                                   first page */                               \
    X(cp, gp, How, HOW, 1)                      /* the triangle for a root (bits 0 to 7), the  \
                                                   depth to make pieces down to (8 to 15), and \
                                                   bit 31 for a root */                        \
    X(cp, gp, Words, WORDS, 2)

/// An entry of one of the light pool's point lists (kPointEntry, VELK_POINT_ENTRY).
#define VELK_POINT_ENTRY(X, cp, gp)                                                                \
    X(cp, gp, First, FIRST, 0)                  /* the word of its first page, or for a piece's \
                                                   point (VELK_LIGHT_POINT_PIECE) its page */  \
    X(cp, gp, Point, POINT, 1)                                                                 \
    X(cp, gp, Instance, INSTANCE, 2)                                                           \
    X(cp, gp, Words, WORDS, 3)

/// What changed this frame for the light (kChangeList, VELK_CHANGE_LIST), then a VELK_CHANGE
/// for each change.
#define VELK_CHANGE_LIST(X, cp, gp)                                                                \
    X(cp, gp, Count, COUNT, 0)                                                                 \
    X(cp, gp, All, ALL, 1)                      /* 1 when everything counts as changed */      \
    X(cp, gp, Changes, CHANGES, 4)

/// A change (kChange, VELK_CHANGE): a box in the world, and the light that changed, or
/// kLightNone for something in the box that moved, came or went.
#define VELK_CHANGE(X, cp, gp)                                                                     \
    X(cp, gp, Min, MIN, 0)                      /* three floats */                             \
    X(cp, gp, Light, LIGHT, 3)                                                                 \
    X(cp, gp, Max, MAX, 4)                      /* three floats */                             \
    X(cp, gp, Words, WORDS, 8)

/// A primitive's entry for drawing by cluster (kClusterEntry, VELK_CLUSTER_ENTRY). See
/// instance_cluster_runs_src.
#define VELK_CLUSTER_ENTRY(X, cp, gp)                                                              \
    X(cp, gp, ListWord, LIST, 0)                /* the word of its list of positions, then each one's kept cut record and cluster record */ \
    X(cp, gp, CapacityWord, CAPACITY, 1)        /* the list's capacity */                      \
    X(cp, gp, TableWord, TABLE, 2)              /* the word of its cluster table (kLodClusterWords a cluster) */ \
    X(cp, gp, ClustersWord, CLUSTERS, 3)        /* its clusters */                             \
    X(cp, gp, CountsWord, COUNTS, 4)            /* the word of its counts, one a cluster */    \
    X(cp, gp, CursorsWord, CURSORS, 5)          /* the word of its cursors, likewise */        \
    X(cp, gp, CommandsWord, COMMANDS, 6)        /* the word of its commands, likewise */       \
    X(cp, gp, DrawsWord, DRAWS, 7)              /* how many commands are in use */             \
    X(cp, gp, NodesWord, NODES, 8)              /* the word of the tree over its clusters */   \
    X(cp, gp, FlagsWord, FLAGS, 9)              /* kLodDeforming, kLodMoved */                 \
    X(cp, gp, FirstPositionsWord, FIRST_POSITIONS, 10) /* how many positions the first phase used */ \
    X(cp, gp, SecondCommandsWord, SECOND_COMMANDS, 11) /* the second phase's commands */       \
    X(cp, gp, SecondDrawsWord, SECOND_DRAWS, 12) /* and how many are in use */                 \
    X(cp, gp, DrawWord, DRAW, 13)               /* the draw data record its pixels name, 0xFFFFFFFF until emitted */ \
    X(cp, gp, RayWord, RAY, 14)                 /* the word of its clusters' ray headers (MeshLodInfo::ray_word) */ \
    X(cp, gp, Words, END, 16)

/// A partition's entry in a partition table (kPartitionEntry, VELK_PARTITION_ENTRY). See
/// instance_partition_cull_src.
#define VELK_PARTITION_ENTRY(X, cp, gp)                                                            \
    X(cp, gp, Min, MIN, 0)                      /* its world bounds' minimum, three floats */  \
    X(cp, gp, Max, MAX, 3)                      /* and maximum */                              \
    X(cp, gp, FirstChunk, FIRST_CHUNK, 6)       /* the word of its first chunk */              \
    X(cp, gp, Chunks, CHUNKS, 7)                /* its chunk count */                          \
    X(cp, gp, QuadTree, QUAD_TREE, 8)           /* the word of its quad tree, kCullNoSlot for none */ \
    X(cp, gp, TreeLeaves, TREE_LEAVES, 9)       /* the tree's leaves, with kQuadLeavesOnly */  \
    X(cp, gp, End, END, 10)

/// A chunk of a kept cut's choose list (kChunk, VELK_CHUNK).
#define VELK_CHUNK(X, cp, gp)                                                                      \
    X(cp, gp, FirstSlot, FIRST_SLOT, 0)         /* the word of its first slot */               \
    X(cp, gp, Partition, PARTITION, 1)          /* the partition it is from */                 \
    X(cp, gp, End, END, 2)

/// A kept cut's header (kCut, VELK_CUT), in the culling arena: see instance_cull.h's description
/// of kept cuts. Its records, ray list entries and tree nodes are in pages of one pool, each
/// partition's named by its page table.
#define VELK_CUT_HEADER(X, cp, gp)                                                                 \
    X(cp, gp, Pages, PAGES, 0)                  /* pages in the pool */                        \
    X(cp, gp, FreePages, FREE_PAGES, 1)         /* pages on the free stack, as the last sweep left it */ \
    X(cp, gp, PagesTaken, PAGES_TAKEN, 2)       /* pages taken off the stack this frame, from its top down */ \
    X(cp, gp, PagesWanted, PAGES_WANTED, 3)     /* pages the pool should hold, set when it is too small */ \
    X(cp, gp, ChooseGroups, CHOOSE_GROUPS, 4)   /* group counts of the choosing dispatches: chunks choosing again */ \
    X(cp, gp, TableUsed, TABLE_USED, 7)         /* entries of the current page table given out */ \
    X(cp, gp, DrawGroups, DRAW_GROUPS, 8)       /* of the passes over the pool's records: a group a page, up to the last in use */ \
    X(cp, gp, TableParity, TABLE_PARITY, 11)    /* which page table is current, 0 or 1 */      \
    X(cp, gp, RecordPool, RECORD_POOL, 12)      /* the word of the pool's records (VELK_RECORD) */ \
    X(cp, gp, RayPool, RAY_POOL, 13)            /* of its ray list entries (VELK_CUT_RAY), one a record */ \
    X(cp, gp, ChoseAgain, CHOSE_AGAIN, 14)      /* how many times partitions chose again, for the log */ \
    X(cp, gp, NodePool, NODE_POOL, 15)          /* of its tree nodes (VELK_CUT_NODE), kCutPageNodes a page */ \
    X(cp, gp, Capacity, CAPACITY, 16)           /* records the pool holds, kCutPageRecords a page */ \
    X(cp, gp, PageHeaders, PAGE_HEADERS, 17)    /* the word of the pages' headers (VELK_CUT_PAGE) */ \
    X(cp, gp, StackWord, STACK_WORD, 18)        /* the word of the free stack, a page each */  \
    X(cp, gp, Partitions, PARTITIONS, 19)       /* partitions */                               \
    X(cp, gp, PageMarks, PAGE_MARKS, 20)        /* the word of the page marks, a bit a page */ \
    X(cp, gp, ChooseWord, CHOOSE_WORD, 21)      /* the word of the choose list: kCullChunkListWords a chunk */ \
    X(cp, gp, StatesWord, STATES_WORD, 22)      /* the word of the partitions' states (VELK_PART) */ \
    X(cp, gp, ChunkCountsWord, CHUNK_COUNTS_WORD, 23) /* the word of the choose list's chunks' counts and first records */ \
    X(cp, gp, Tables, TABLES, 24)               /* the two page tables' words, a page an entry */ \
    X(cp, gp, KeptPages, KEPT_PAGES, 26)        /* pages of partitions keeping their cut this frame */ \
    X(cp, gp, PagesDemand, PAGES_DEMAND, 27)    /* pages partitions choosing again asked for this frame, placed or not */ \
    X(cp, gp, TakeoverFrame, TAKEOVER_FRAME, 29) /* the frame of the takeover table, counted by the begin pass */ \
    X(cp, gp, TakeoverWord, TAKEOVER_WORD, 30)  /* the takeover table's word (VELK_TAKEOVER a slot) */ \
    X(cp, gp, TakeoverSlots, TAKEOVER_SLOTS, 31) /* and its slots, a power of two */           \
    X(cp, gp, LightOnlyRecords, LIGHT_ONLY_RECORDS, 32) /* records for light only in the current store */ \
    X(cp, gp, LastLightOnlyRecords, LAST_LIGHT_ONLY_RECORDS, 33)                               \
    X(cp, gp, Rewritten, REWRITTEN, 34)         /* records written by partitions choosing again this frame */ \
    X(cp, gp, TookOver, TOOK_OVER, 35)          /* of those, how many took over an old record's light */ \
    X(cp, gp, LastRewritten, LAST_REWRITTEN, 36)                                               \
    X(cp, gp, LastTookOver, LAST_TOOK_OVER, 37)                                                \
    X(cp, gp, LongestWalk, LONGEST_WALK, 40)    /* the most hierarchy nodes one thread walked choosing this frame */ \
    X(cp, gp, MostChosen, MOST_CHOSEN, 41)      /* the most records one walk chose */          \
    X(cp, gp, LastLongestWalk, LAST_LONGEST_WALK, 42)                                          \
    X(cp, gp, LastMostChosen, LAST_MOST_CHOSEN, 43)                                            \
    X(cp, gp, RayMarksWord, RAY_MARKS_WORD, 44) /* the word of the ray marks, a bit a record of the pool */ \
    X(cp, gp, VisibleGroups, VISIBLE_GROUPS, 45) /* of the drawing dispatch: a group a page of partitions in view */ \
    X(cp, gp, VisibleWord, VISIBLE_WORD, 48)    /* the word of their runs: first record and end, two words a group */ \
    X(cp, gp, DiagTreeBuilds, DIAG_TREE_BUILDS, 49) /* TEMPORARY: partition trees built since the log */ \
    X(cp, gp, DiagTreeFallbacks, DIAG_TREE_FALLBACKS, 50) /* and of them those that fell back to balanced */ \
    X(cp, gp, End, END, 51)

/// A partition's state in a kept cut (kPart, VELK_PART), at the cut's STATES_WORD.
#define VELK_CUT_PART(X, cp, gp)                                                                   \
    X(cp, gp, From, FROM, 0)                    /* where in the world it chose from, three floats */ \
    X(cp, gp, ChunkFirst, CHUNK_FIRST, 3)       /* its table entry's first chunk word when it chose */ \
    X(cp, gp, Chunks, CHUNKS, 4)                /* and its chunk count */                      \
    X(cp, gp, TableFirst, TABLE_FIRST, 5)       /* its page table's first entry in the current page table */ \
    X(cp, gp, Count, COUNT, 6)                  /* its record count, kCutPageRecords a page */ \
    X(cp, gp, LastTableFirst, LAST_TABLE_FIRST, 7) /* while choosing again, its old page table's first entry in the last */ \
    X(cp, gp, Choosing, CHOOSING, 8)            /* 1 while choosing this frame */              \
    X(cp, gp, ChunkStart, CHUNK_START, 9)       /* while choosing, where its chunks start in the choose list */ \
    X(cp, gp, Root, ROOT, 10)                   /* its tree's root, a node of the pool, while RAY_COUNT is not 0 */ \
    X(cp, gp, OldRayCount, OLD_RAY_COUNT, 11)   /* while choosing again, RAY_COUNT as it was */ \
    X(cp, gp, QuadTree, QUAD_TREE, 12)          /* its table entry's quad tree word when it chose */ \
    X(cp, gp, Retry, RETRY, 13)                 /* 1 when it kept its old records for want of pages, to choose again */ \
    X(cp, gp, Same, SAME, 14)                   /* while choosing again, 1 when its chunks and quad tree are as before */ \
    X(cp, gp, OldCount, OLD_COUNT, 15)          /* while choosing again, how many records it had, 0 for none */ \
    X(cp, gp, RayCount, RAY_COUNT, 16)          /* how many records rays trace (its ray list) */ \
    X(cp, gp, Looked, LOOKED, 17)               /* which way the camera looked when it chose, three floats */ \
    X(cp, gp, OutOfView, OUT_OF_VIEW, 20)       /* 1 when it chose clusters at the light's level for being out of view */ \
    X(cp, gp, Always, ALWAYS, 21)               /* 1 when its records are visited in view or not: some deform, or are items drawn by their own pass */ \
    X(cp, gp, LightOnly, LIGHT_ONLY, 22)        /* its records for light only, for the log */ \
    X(cp, gp, Quads, QUADS, 23)                 /* its quads, for the log */                   \
    X(cp, gp, Box, BOX, 24)                     /* the world box around what it draws, min then max, six floats */ \
    X(cp, gp, End, END, 30)

/// A page header of a kept cut's pool (kCutPage, VELK_CUT_PAGE): which partition holds the page,
/// and where in its page table.
#define VELK_CUT_PAGE(X, cp, gp)                                                                   \
    X(cp, gp, Owner, OWNER, 0)          /* the partition's index, kCullNoSlot for a free page */  \
    X(cp, gp, Place, PLACE, 1)          /* its place in the partition's page table */             \
    X(cp, gp, Words, WORDS, 2)

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
    X(cp, gp, End, END, 8)

VELK_BATCH_TABLE(VELK_LAYOUT_CPP_WORD, Table, TABLE)
VELK_LOD_CLUSTER(VELK_LAYOUT_CPP_WORD, LodCluster, LOD_CLUSTER)
VELK_QUAD_NODE(VELK_LAYOUT_CPP_WORD, QuadNode, QUAD_NODE)
VELK_QUAD_LIST(VELK_LAYOUT_CPP_WORD, QuadList, QUAD_LIST)
VELK_PLAIN_ITEM(VELK_LAYOUT_CPP_WORD, PlainItem, PLAIN_ITEM)
VELK_ITEM_DESCRIPTOR(VELK_LAYOUT_CPP_WORD, ItemDescriptor, ITEM_DESCRIPTOR)
VELK_ITEM_MESH(VELK_LAYOUT_CPP_WORD, ItemMesh, ITEM_MESH)
VELK_CUT_NODE(VELK_LAYOUT_CPP_WORD, CutNode, CUT_NODE)
VELK_CUT_RAY(VELK_LAYOUT_CPP_WORD, CutRay, CUT_RAY)
VELK_RAY_TOP(VELK_LAYOUT_CPP_WORD, RayTop, RAY_TOP)
VELK_RAY_TOP_CUT(VELK_LAYOUT_CPP_WORD, RayTopCut, RAY_TOP_CUT)
VELK_RAY_TOP_LEAF(VELK_LAYOUT_CPP_WORD, RayTopLeaf, RAY_TOP_LEAF)
VELK_PROBE(VELK_LAYOUT_CPP_WORD, Probe, PROBE)
VELK_TRACE_ENTRY(VELK_LAYOUT_CPP_WORD, TraceEntry, TRACE_ENTRY)
VELK_POINT_ENTRY(VELK_LAYOUT_CPP_WORD, PointEntry, POINT_ENTRY)
VELK_SEARCH_ENTRY(VELK_LAYOUT_CPP_WORD, SearchEntry, SEARCH_ENTRY)
VELK_SPLIT_ENTRY(VELK_LAYOUT_CPP_WORD, SplitEntry, SPLIT_ENTRY)
VELK_CHANGE_LIST(VELK_LAYOUT_CPP_WORD, ChangeList, CHANGE_LIST)
VELK_CHANGE(VELK_LAYOUT_CPP_WORD, Change, CHANGE)
VELK_CLUSTER_ENTRY(VELK_LAYOUT_CPP_WORD, ClusterEntry, CLUSTER_ENTRY)
VELK_PARTITION_ENTRY(VELK_LAYOUT_CPP_WORD, PartitionEntry, PARTITION_ENTRY)
VELK_CHUNK(VELK_LAYOUT_CPP_WORD, Chunk, CHUNK)
VELK_CUT_HEADER(VELK_LAYOUT_CPP_WORD, Cut, CUT)
VELK_CUT_PART(VELK_LAYOUT_CPP_WORD, Part, PART)
VELK_CUT_PAGE(VELK_LAYOUT_CPP_WORD, CutPage, CUT_PAGE)
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
    VELK_BATCH_TABLE(VELK_LAYOUT_GLSL_WORD, Table, TABLE)                                          \
    VELK_LOD_CLUSTER(VELK_LAYOUT_GLSL_WORD, LodCluster, LOD_CLUSTER)                               \
    VELK_QUAD_NODE(VELK_LAYOUT_GLSL_WORD, QuadNode, QUAD_NODE)                                     \
    VELK_QUAD_LIST(VELK_LAYOUT_GLSL_WORD, QuadList, QUAD_LIST)                                     \
    VELK_PLAIN_ITEM(VELK_LAYOUT_GLSL_WORD, PlainItem, PLAIN_ITEM)                                  \
    VELK_ITEM_DESCRIPTOR(VELK_LAYOUT_GLSL_WORD, ItemDescriptor, ITEM_DESCRIPTOR)                   \
    VELK_ITEM_MESH(VELK_LAYOUT_GLSL_WORD, ItemMesh, ITEM_MESH)                                     \
    VELK_CUT_NODE(VELK_LAYOUT_GLSL_WORD, CutNode, CUT_NODE)                                        \
    VELK_CUT_RAY(VELK_LAYOUT_GLSL_WORD, CutRay, CUT_RAY)                                           \
    VELK_RAY_TOP(VELK_LAYOUT_GLSL_WORD, RayTop, RAY_TOP)                                           \
    VELK_RAY_TOP_CUT(VELK_LAYOUT_GLSL_WORD, RayTopCut, RAY_TOP_CUT)                                \
    VELK_RAY_TOP_LEAF(VELK_LAYOUT_GLSL_WORD, RayTopLeaf, RAY_TOP_LEAF)                             \
    VELK_PROBE(VELK_LAYOUT_GLSL_WORD, Probe, PROBE)                                                \
    VELK_TRACE_ENTRY(VELK_LAYOUT_GLSL_WORD, TraceEntry, TRACE_ENTRY)                               \
    VELK_POINT_ENTRY(VELK_LAYOUT_GLSL_WORD, PointEntry, POINT_ENTRY)                               \
    VELK_SEARCH_ENTRY(VELK_LAYOUT_GLSL_WORD, SearchEntry, SEARCH_ENTRY)                            \
    VELK_SPLIT_ENTRY(VELK_LAYOUT_GLSL_WORD, SplitEntry, SPLIT_ENTRY)                               \
    VELK_CHANGE_LIST(VELK_LAYOUT_GLSL_WORD, ChangeList, CHANGE_LIST)                               \
    VELK_CHANGE(VELK_LAYOUT_GLSL_WORD, Change, CHANGE)                                             \
    VELK_CLUSTER_ENTRY(VELK_LAYOUT_GLSL_WORD, ClusterEntry, CLUSTER_ENTRY)                         \
    VELK_PARTITION_ENTRY(VELK_LAYOUT_GLSL_WORD, PartitionEntry, PARTITION_ENTRY)                   \
    VELK_CHUNK(VELK_LAYOUT_GLSL_WORD, Chunk, CHUNK)                                                \
    VELK_CUT_HEADER(VELK_LAYOUT_GLSL_WORD, Cut, CUT)                                               \
    VELK_CUT_PART(VELK_LAYOUT_GLSL_WORD, Part, PART)                                               \
    VELK_CUT_PAGE(VELK_LAYOUT_GLSL_WORD, CutPage, CUT_PAGE)                                        \
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
