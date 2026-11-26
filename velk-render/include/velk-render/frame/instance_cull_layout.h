#ifndef VELK_RENDER_FRAME_INSTANCE_CULL_LAYOUT_H
#define VELK_RENDER_FRAME_INSTANCE_CULL_LAYOUT_H

#include <velk/api/perf.h>
#include <velk/api/velk.h>
#include <velk/string_view.h>
#include <velk/vector.h>

#include <velk-render/api/cached_view_pass.h>
#include <velk-render/frame/cull_layout.h>
#include <velk-render/frame/draw_call_emit.h>
#include <velk-render/frame/light_pool.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_batch.h>
#include <velk-render/interface/intf_frame_snippet_registry.h>
#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/render_path/frame_context.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace velk {

/// Slots in a chunk of a partition's list. A partition's last chunk is filled out with
/// kCullNoSlot.
inline constexpr uint32_t kCullChunkSlots = 64;
inline constexpr uint32_t kCullNoSlot = 0xFFFFFFFFu;
/// Words per partition in a partition table: VELK_PARTITION_ENTRY (cull_layout.h). See
/// instance_partition_cull_src.
inline constexpr uint32_t kCullEntryWords = kPartitionEntryEnd;
/// Set in a partition table entry's leaves word when its tree's leaves are the finest it has (a
/// partition without its instances, whose list names chunk quads): where no node is small
/// enough, the leaf itself is taken. Mirrors VELK_QUAD_LEAVES_ONLY.
inline constexpr uint32_t kQuadLeavesOnly = 0x80000000u;
/// Words per chunk in a kept cut's choose list: VELK_CHUNK (cull_layout.h).
inline constexpr uint32_t kCullChunkListWords = kChunkEnd;
/// The slot an object id names, in tables by object (IScene::get_object_ids).
inline constexpr uint32_t kObjectSlotMask = 0x3FFFFFFu;
/// Items (InstanceCullPass::together), kPlainItemWords each (VELK_PLAIN_ITEM in cull_layout.h),
/// are what kept cut records name. An item's kind is an RtShape kind: kRayShapeMesh for its mesh,
/// else its visual's analytic shape (IAnalyticShape); its bounds grow by its motion. A plain
/// object's instances are an item each, first; then each instanced visual's items, an item per
/// slot, which instance_items_src writes whole each frame from the visuals' item descriptors
/// (VELK_ITEM_DESCRIPTOR, then a VELK_ITEM_MESH per mesh), with their objects and next words, so
/// that the CPU never goes through them.
/// A plain item's shape kind for a mesh. Mirrors RtShape's 255.
inline constexpr uint32_t kRayShapeMesh = 255;
/// A plain item's shape kind for a rect over its instance's extent. Mirrors RtShape's 0.
inline constexpr uint32_t kRayShapeRect = 0;
/// How far the camera may get from where a partition's levels of detail were chosen before
/// they are chosen again, as a share of its distance from the partition.
inline constexpr float kCutRefresh = 1.f / 16.f;

/// A view's kept cut of a partitioned visual, in the culling arena: the clusters chosen for each
/// partition, all round the camera, kept while the camera stays near where they were chosen
/// from. Its header is VELK_CUT_HEADER and each partition's state VELK_CUT_PART (cull_layout.h).
///
/// Records live in a pool of pages, kCutPageRecords records a page, and at the same page
/// kCutPageRecords ray list entries and kCutPageNodes tree nodes. A partition's pages are named,
/// in order, by its page table: COUNT records from TABLE_FIRST of the current page table, the
/// record at its place i on page table[i / kCutPageRecords]. A page header (VELK_CUT_PAGE) names
/// the partition holding the page and its place in that table. A partition that keeps its records
/// keeps its pages, so its records, ray list and tree stay where they are; only its page table is
/// copied into the current one, which flips each frame. A partition that chooses again takes new
/// pages off the free stack; its old pages stay readable until the end of the culling (its
/// OLD_COUNT records from LAST_TABLE_FIRST of the last page table), whose light its new records
/// take over. Pages no current page table names are found free every frame
/// (instance_cut_pages_src). One that finds no free pages keeps its old records and chooses again
/// on the next frame, unless its members changed, when it has none for the frame. A new quad
/// tree (QUAD_TREE) or a turn of the camera after choosing for being out of view (OUT_OF_VIEW)
/// makes it choose again. A partition's ray list holds, in the order of its pages, an entry
/// (kCutRayWords) for each record rays trace (those at the light's level: records for rays only,
/// clusters as coarse as the light, quads, whole objects): its record's place in the pool
/// and its box. Its tree is over them; the ray marks have a bit for each record of the
/// pool, set by the choosing where the record's member is in a patch for rays (kCutRayQuad), so
/// that the tree pass leaves it out, read only for the partitions that chose this frame.
/// A partition's records are in the order of its chunks, each chunk's in the order of its
/// members, and each instance's in the order its hierarchy's tree is expanded and walked, so
/// that records near in its page table are near in the world.
/// A record is VELK_CUT_RECORD (cull_layout.h): entry, cluster and slot, as the chosen lists have
/// them, and its light (kLightNone until it is read, see light_pool.h); for a mesh without
/// clusters the whole instance: entry kCullNoSlot, cluster its mesh's table; for an item rays see
/// that its own pass draws, entry and cluster kCullNoSlot. What is drawn from a record, and what a
/// ray hits of it, carries the word of the record (velk_ids_record, RayHit::record), which holds
/// for that frame.
///
/// Each partition has a tree over its records for rays, kCutNodeWords a node: the boxes of
/// its two children (left min, left max, right min, right max, in world space), then a
/// reference to each, its height (1 over leaves only) and 1 for the root. Its node i is node
/// i % kCutPageNodes of page table[i / kCutPageNodes]. A reference with kCutLeaf is up to four
/// of its ray list's entries, which never cross a page: the first one's place in the pool times
/// four plus how many less one; without, a node's place in the pool; kCullNoSlot for none. The
/// tree is a radix tree over its leaves in Morton order (VELK_LBVH_GLSL), the root (ROOT) its
/// last node. See instance_cut_tree_src.
inline constexpr uint32_t kCutHeaderWords = kCutEnd;
inline constexpr uint32_t kCutPartitionWords = kPartEnd;
/// Records, and ray list entries, a page of a kept cut's pool holds. Mirrors VELK_CUT_PAGE_RECORDS.
inline constexpr uint32_t kCutPageRecords = 64;
/// Tree nodes a page holds: enough for the tree over its records' leaves of four. Mirrors
/// VELK_CUT_PAGE_NODES.
inline constexpr uint32_t kCutPageNodes = 16;
/// Entries a block of the sort of ray lists past kRaySortMax takes (kCutLargeHeaderWords), a work
/// group each. Mirrors VELK_CUT_LARGE_BLOCK_ENTRIES.
inline constexpr uint32_t kCutLargeBlockEntries = 256;
/// Blocks a kept cut's sort of long ray lists has room for each frame: a list that does not fit
/// is sorted by its partition's own work group, as one up to kRaySortMax is.
inline constexpr uint32_t kCutLargeRoom = 512;
inline constexpr uint32_t kCutLargeHeaderWords = kCutLargeEnd;
inline constexpr uint32_t kCutLargeListWords = kCutLargeListEnd;
/// The words of a kept cut's sort of long ray lists (kCutLargeHeaderWords): its header, a queue
/// place for each partition, a word a block, and 256 digit counts a block.
inline uint32_t cut_large_words(uint32_t partitions, uint32_t room)
{
    return kCutLargeHeaderWords + partitions * kCutLargeListWords + room + room * 256u;
}
/// Words per slot of a kept cut's takeover table: the frame it was filled in and an old record's word.
inline constexpr uint32_t kCutTakeoverWords = kTakeoverEnd;
inline constexpr uint32_t kCutRecordWords = kCutRecordEnd;
inline constexpr uint32_t kCutLeaf = 0x80000000u;

/// A partition's quad tree: its members as quads, and every node above them a quad standing for
/// the members under it, so that where they are small on screen a few quads take their place.
/// A complete binary tree over the partition's members in their order, by index with no links:
/// node 0 is the root, node i's children are 2i + 1 and 2i + 2, and the leaves are the last
/// `leaves` nodes, leaf k the member at position k of the partition's list (a power of two of
/// them, the rest empty). A node is kQuadNodeWords, VELK_QUAD_NODE in cull_layout.h; its light
/// is that of the members under it where it is as wide on screen as the light spacing
/// (kLightPixels), see light_pool.h.

/// A kept cut's record of a quad (kQuadNodeWords) has this for its entry, the node's word for its
/// cluster, and its tree's word for its slot, which its light is found by. Mirrors VELK_CUT_QUAD.
inline constexpr uint32_t kCutQuad = 0xFFFFFFFEu;
/// A kept cut's record of a patch of members smaller on screen than the light spacing, for rays
/// only: the topmost node of its partition's quad tree over them that is no wider than the
/// spacing, its word for the record's cluster and its tree's for its slot. A ray through its
/// sphere keeps part of its light, by how much of the sphere its members' quads cover (the
/// node's quad keeps their area); probe rays pass it. The members' own records are not traced.
/// Mirrors VELK_CUT_RAY_QUAD.
inline constexpr uint32_t kCutRayQuad = 0xFFFFFFFDu;
/// A kept cut's record whose batch words (entry, table or plain item) did not carry over when its
/// batches were made again. The rename marks it, and its partition chooses again before anything
/// reads it (instance_cut_stale_src). Mirrors VELK_CUT_STALE.
inline constexpr uint32_t kCutStale = 0xFFFFFFFCu;
/// A kept cut's record of a cluster for rays only has this on its entry: a cluster at the light's
/// spacing (kLightPixels) over drawn ones finer than that, which rays walk in their place. It is
/// not drawn and has no light of its own but the one value a ray's hit asks for. Mirrors
/// VELK_CUT_FOR_RAYS.
inline constexpr uint32_t kCutForRays = 0x80000000u;
/// How wide on screen, in pixels, a partition's quad may be to take the place of the members
/// under it, seen from where its partition chose. The topmost such quad over a member is chosen.
inline constexpr float kQuadPixels = kLodElementPixels;

/// The leaves of the quad tree over @p members members: see kQuadNodeWords.
inline uint32_t quad_tree_leaves(uint32_t members)
{
    uint32_t leaves = 1;
    while (leaves < members) {
        leaves *= 2u;
    }
    return leaves;
}

/// Nodes in the tree over a partition of @p records records: one less than its leaves of four,
/// at least one. Mirrors velk_cut_tree_nodes.
inline uint32_t cut_tree_nodes(uint32_t records)
{
    if (records == 0) {
        return 0;
    }
    const uint32_t leaves = (records + 3u) / 4u;
    return leaves > 1u ? leaves - 1u : 1u;
}

/// The culling's trace (VELK_TRACE_PIXEL): what it does with everything the ray through one
/// pixel passes the bounds of, so that what is missing there can be followed. In the culling
/// arena, named by the view's FrameGlobals::trace (kTraceNone for none): the pixel's x and y,
/// how many notes were made since the CPU last read them, how many fit, a cluster entry whose
/// walks are noted wherever they go (VELK_TRACE_ENTRY, kTraceNone for none) and a spare word,
/// then the notes, kTraceNoteWords each: the stage and in bits 8 and up what came of it, then
/// three words the stage says (VELK_TRACE_GLSL).
inline constexpr uint32_t kTraceNone = 0xFFFFFFFFu;
inline constexpr uint32_t kTraceHeaderWords = 6;
inline constexpr uint32_t kTraceNoteWords = 4;
inline constexpr uint32_t kTraceCapacity = 4096;
/// A partition's choice: a record written (0), or left out past the count it was given (1);
/// then its entry, cluster and slot.
inline constexpr uint32_t kTraceChoose = 1;
/// A partition with records whose box the ray passes: listed (0), outside the view (1), its runs
/// past the room (2); then its index, its count of records and 0.
inline constexpr uint32_t kTracePartition = 2;
/// A cluster record drawn: chosen (0), outside the view (1), held back by occlusion (2), compute
/// raster (3), the chosen list full (4); then its entry, cluster and record.
inline constexpr uint32_t kTraceDraw = 3;
/// A fragment at the pixel, in the visibility pass (VELK_TRACE_FRAGMENT_GLSL): reached its shader
/// (0), cut out by its material's alpha (1), written (2); then its draw, its ids' second word and
/// its depth's bits.
inline constexpr uint32_t kTraceRaster = 4;
/// The resolve at the pixel (visibility_resolve_main_src): took the hardware raster's ids (0) or
/// compute raster's (1); then the ids' two words and 0.
inline constexpr uint32_t kTraceResolve = 5;
/// A blended fragment at the pixel, in the transparent pass (transparent_lit_main_src): drawn (0);
/// then its draw, its ids' second word and the bits of the opacity it was drawn with.
inline constexpr uint32_t kTraceBlended = 6;
/// A partition's walk of a mesh's hierarchy (velk_lod_child) reached a cluster on the ray: chosen
/// (2), for rays only (3), skipped with all under its node too coarse (4) or with a parent that
/// will do (5), too coarse itself (6), its parent will do (7); then its entry, cluster and the
/// partition's state word.
inline constexpr uint32_t kTraceWalk = 7;
/// An instance culled every frame (instance_cull_compute_src), not by a kept cut: walked (0),
/// too far (1), outside the view (2), held back by occlusion (3); then its first cluster entry,
/// its slot and its count of entries.
inline constexpr uint32_t kTraceInstance = 8;
/// A fragment at the pixel drawn by the forward path (forward_fragment_driver_template): drawn
/// (0); then its draw, its ids' second word and its depth's bits.
inline constexpr uint32_t kTraceForward = 9;
/// The numbers one decision of the walk of the entry traced went by (VELK_TRACE_ENTRY): 0; then
/// what (a node's reference or a cluster, with which number in bits 24 to 27), the number as
/// stored and on screen in pixels, each a float's bits.
inline constexpr uint32_t kTraceNumbers = 10;
/// The shape of the tree of the entry traced: 0; then a child's reference, its node's index and
/// which of its two children it is.
inline constexpr uint32_t kTraceShape = 11;

/// The most entries the ray trees' builds sort by where they are (VELK_SORT_GLSL); a partition
/// with more records for rays, or a top tree with more partitions, keeps its order. Mirrors
/// kSortMax there.
inline constexpr uint32_t kRaySortMax = 2048;

/// Words before the levels of a depth pyramid in the culling arena: the level count, the
/// first level's width and height, then each level's first word, as many as
/// kDepthPyramidMaxLevels. See depth_pyramid_compute_src.
inline constexpr uint32_t kDepthPyramidMaxLevels = 15;
inline constexpr uint32_t kDepthPyramidHeaderWords = 3 + kDepthPyramidMaxLevels;

/// The list of what failed the first occlusion test: VELK_RETEST_LIST, VELK_RETEST
/// (cull_layout.h).
inline constexpr uint32_t kCullRetestHeaderWords = kRetestListRecords;
inline constexpr uint32_t kCullRetestRecordWords = kRetestEnd;
inline constexpr uint32_t kCullRetestMaxRecords = 1u << 20;

/// The list of clusters drawn in compute (instance_tiny_draw_src): VELK_TINY_LIST, VELK_TINY
/// (cull_layout.h).
inline constexpr uint32_t kTinyHeaderWords = kTinyListRecords;
inline constexpr uint32_t kTinyRecordWords = kTinyEnd;

/// A view's top tree for rays, over the partitions of all its kept cuts, in the culling arena:
/// VELK_RAY_TOP, VELK_RAY_TOP_CUT and VELK_RAY_TOP_LEAF (cull_layout.h), then the nodes,
/// kCutNodeWords each as a partition's are, but a reference with kCutLeaf names a leaf, and
/// without a node of this tree by its index.

/// Nodes in the top tree over @p leaves leaves: one less than them, at least one. The top build
/// works it out the same way.
inline uint32_t top_tree_nodes(uint32_t leaves)
{
    if (leaves == 0) {
        return 0;
    }
    return leaves > 1u ? leaves - 1u : 1u;
}

/// Runs of a kept cut's records that instance_cut_draw_src walks (VELK_CUT_VISIBLE_WORD): at
/// most a run per page of the pool and one more per partition.
inline uint32_t cut_visible_runs(uint32_t capacity, uint32_t partitions)
{
    return capacity / kCutPageRecords + partitions + 1u;
}

/// Clusters instance_tiny_draw_one_pass_src can tell apart: its 64-bit words' low half holds a
/// cluster's place in the list above the triangle's 8 bits.
inline constexpr uint32_t kTinyOnePassRecords = 1u << 24u;

} // namespace velk

#endif // VELK_RENDER_FRAME_INSTANCE_CULL_LAYOUT_H
