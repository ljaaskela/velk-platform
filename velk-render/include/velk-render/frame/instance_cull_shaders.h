#ifndef VELK_RENDER_FRAME_INSTANCE_CULL_SHADERS_H
#define VELK_RENDER_FRAME_INSTANCE_CULL_SHADERS_H

#include <velk/string_view.h>

#include <velk-render/frame/instance_cull_layout.h>

// The instance culling's shaders. Their text is in velk-render/shaders, each file built in as the
// macro VELK_<NAME>_GLSL that its generated header <velk-render/glsl/<name>.inc> defines.

namespace velk {

// The trace's shader side (kTraceNone and the rest above). The shader declares cull_words.
#include <velk-render/glsl/trace.inc>

// Where a cut's parts are. The shader declares cull_words.
#include <velk-render/glsl/cut.inc>

// Sorting a ray tree's entries by where they are, in a work group's shared memory, for the tree
// builds: a key for each of up to kRaySortMax entries, its Morton code above its index in the
// low kSortIndexBits bits, sorted ascending by a bitonic sort.
#include <velk-render/glsl/sort.inc>

// A ray tree as a radix tree over its leaves in their sorted order (Karras 2012), for the tree
// builds, after VELK_CUT_GLSL. The shader defines velk_lbvh_key, the sorted code of leaf g
// without its place, so that leaves with the same code split evenly by their places;
// velk_lbvh_leaf_ref, its reference in a node, 0xFFFFFFFF for none; and
// velk_lbvh_leaf_box, the box of the leaf a reference names; velk_lbvh_node_word, the word of
// the tree's node i; velk_lbvh_node_ref, what its parent names it by (never with the leaf bit);
// velk_lbvh_ref_word, the word of the node a reference names; and velk_store_box and
// velk_load_vec3. With g_lbvh_balanced each leaf's key is its place, which makes the tree
// balanced.
#include <velk-render/glsl/lbvh.inc>

/**
 * Fills one level of a depth pyramid in the culling arena with the farthest depth of what
 * each of its texels covers, depth being larger for what is nearer. The first level halves
 * the frame's depth texture, and later ones the level before. With compute_word set, the first
 * level takes each pixel's nearer of that texture and what compute raster drew there
 * (instance_tiny_draw_src), so that those pixels hide what is behind them too.
 */
#include <velk-render/glsl/depth_pyramid_compute.inc>
inline constexpr string_view depth_pyramid_compute_src = VELK_DEPTH_PYRAMID_COMPUTE_GLSL;

// Whether a sphere around a point relative to the camera, as seen through vp, lies behind all
// that a depth pyramid holds there. A larger depth is nearer. Declares nothing the shader must
// provide but cull_words.
#include <velk-render/glsl/occlusion.inc>

// Adds a cluster to the list of those drawn in compute, false when it is full. The shader has
// pc.tiny_word and pc.tiny_capacity.
#include <velk-render/glsl/tiny_add.inc>

// velk_lod_visit calls velk_lod_chosen for each cluster of a primitive's hierarchy that this
// instance uses: those whose own error on screen is small enough and whose parent's is not. A
// parent's sphere holds its children's and its error is not less than theirs, so going up from
// any triangle the answer changes once, which is what picks one complete surface. The shader
// declares cull_words, lod_nodes and
//   void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam,
//                        vec3 lod_cam, float pixels_per_unit, bool deforming);
// first. `entry` is as at instance_cluster_runs_src. Clusters are found through the tree over
// them, which skips those under a node when none could qualify. With VELK_LOD_LIGHT defined, it
// also calls
//   void velk_lod_light_chosen(uint entry, uint c, uint slot);
// for each cluster that the same test passes at pc.light_pixels in place of `max_pixels`, the
// light's level over drawn clusters finer than it, which the shader declares too; and what is
// out of the view of the camera at g_view_cam (world space), widened by a margin, is chosen at
// the light's level only, which sets g_out_of_view.
#include <velk-render/glsl/lod_select.inc>

// Draws chosen cluster `c` of `entry`: in compute when its mean triangle on screen, the
// width of the sphere around its own triangles over the square root of their count, is
// less than pc.tiny_pixels, its largest triangle fits the most compute raster covers (4
// times that, see instance_tiny_draw_src), and the list has room. Else by the hardware: also
// while its draw is not known yet, and when its material moves vertices. `cam` is the camera,
// which compute raster decides by; `lod_cam` where levels of detail were chosen from.
// `cut_record` is the word of the kept cut record it is from, 0xFFFFFFFF for none. Comes after
// VELK_LOD_SELECT_GLSL; the shader declares pc.tiny_pixels, pc.lod_max_pixels, velk_lod_use and
// velk_tiny_add.
#include <velk-render/glsl/lod_take.inc>

/**
 * Decides for each partition of a visual, in view or not, whether its kept cut
 * (kCutHeaderWords) holds. A table entry (kCullEntryWords) holds the partition's world bounds
 * (min xyz and max xyz, as floats), the word of its first chunk and its chunk count, 0 for an
 * unused entry. A kept cut holds while the camera is no further from where the partition chose
 * than `refresh` times the camera's distance from the partition, and the partition's table
 * entry is as it was. A partition that keeps its records keeps its pages, and its page table goes
 * into the current one; any other chooses again from the camera, with its chunks in the choose
 * list, kCullChunkListWords each: its first slot's word and its partition's index. So a
 * partition's choice is never off by more than that share.
 */
#include <velk-render/glsl/instance_partition_cull.inc>
inline constexpr string_view instance_partition_cull_src = VELK_INSTANCE_PARTITION_CULL_GLSL;

/**
 * Starts a frame of a kept cut (kCutHeaderWords): the other page table becomes current, and the
 * counts and lists of the last frame are emptied. One thread.
 */
#include <velk-render/glsl/instance_cut_begin.inc>
inline constexpr string_view instance_cut_begin_src = VELK_INSTANCE_CUT_BEGIN_GLSL;

/**
 * Finds the free pages of a kept cut's pool once the culling is done: with `phase` 0, a thread
 * per entry of the current page table marks its page (and the first thread judges whether the
 * pool is too small, VELK_CUT_PAGES_WANTED, then empties the free stack and the drawing counts);
 * with 1, a thread per 32 pages puts every page no table names on
 * the free stack, its header saying so, and the group counts of the passes over the pool's
 * records (VELK_CUT_DRAW_GROUPS) go up to the last page in use, then clears the marks. The pages
 * of partitions that chose again are free from here, for the next frame's choosing.
 */
#include <velk-render/glsl/instance_cut_pages.inc>
inline constexpr string_view instance_cut_pages_src = VELK_INSTANCE_CUT_PAGES_GLSL;

/**
 * Chooses the clusters of each instance in the chunks of the choose list, all round the camera,
 * from its partition's viewpoint. With `write` 0, counts them into the chunk's count and the
 * partition's, and lists the chunk's share of the partition's old records in the takeover table;
 * with 1, writes them from the chunk's first place in its partition's page table, which
 * instance_cut_place_src made of its count, and each takes over the light of the old record
 * that was the same (VELK_PART_OLD_COUNT). A mesh without clusters gives one record for the
 * whole instance. A work group per chunk,
 * whose threads share the work of all its members' trees: their tops are expanded together,
 * then each thread walks a few of the smaller trees below, so that no thread walks a large
 * mesh's whole tree alone. The records stay in member order, each member's together.
 *
 * With items_word set, the chunks hold object ids, as the scene's plain partitions have them,
 * and each object is drawn by its plain items (InstanceCullPass::together): an item is its
 * instance's index, its slot, its table and its draw, no table for an item rays see but its own pass
 * draws, which gives one record for the whole object; objects_word holds each item's object id,
 * next_word each item's next item of the same object, and head_word by object slot its first.
 * A record then names the item in place of a slot.
 */
#include <velk-render/glsl/instance_cut_choose.inc>
inline constexpr string_view instance_cut_choose_src = VELK_INSTANCE_CUT_CHOOSE_GLSL;

/**
 * Gives each partition that chose again this frame pages for its count, off the free stack, its
 * page table in the current one, and each of its chunks its first place in that table, in
 * order. One that finds too few free pages keeps its old records, pages and ray list for this
 * frame and chooses again on the next; one whose members changed (VELK_PART_SAME 0) has no
 * records for the frame instead, since its old ones may name what is gone. A thread per
 * partition.
 */
#include <velk-render/glsl/instance_cut_place.inc>
inline constexpr string_view instance_cut_place_src = VELK_INSTANCE_CUT_PLACE_GLSL;

// Where a cut record is in the world, for the shader that declares cull_words and
// cull_instances: a box around it, empty for a slot with no instance. `instances_base` and
// `items_word` are as at instance_cut_choose_src; `pad` grows it.
#include <velk-render/glsl/cut_box.inc>

// A kept cut's ray list entries (kCutRayWords), for the tree builds after VELK_CUT_GLSL,
// VELK_CUT_BOX_GLSL and VELK_SORT_GLSL: writing one from its record, its box, and its code in the
// sort of a list past kRaySortMax entries. The shader's push constants have cut_word,
// instances_base, items_word and pad, as at instance_cut_tree_src.
#include <velk-render/glsl/ray_entry.inc>

// A partition's ray tree's leaves and nodes on its pages, for the tree builds after
// VELK_RAY_ENTRY_GLSL: the partition's page table (g_table) and the entries its leaves hold
// (g_leaf_records), and what VELK_LBVH_GLSL asks of the shader but velk_lbvh_key.
#include <velk-render/glsl/cut_tree_nodes.inc>

// Which of a partition's records rays trace, and each record's share of the partition's box and
// counts, for the scans of instance_cut_tree_src and instance_cut_scan_src after VELK_CUT_GLSL and
// VELK_CUT_BOX_GLSL. The shader's push constants have cut_word, instances_base, items_word, pad,
// globals_base and light_pixels, as at instance_cut_tree_src.
#include <velk-render/glsl/cut_ray_record.inc>

// The words of a kept cut's queue of long lists (kCutLargeHeaderWords): its header's, a queue
// place's, and which list a work group's block is of.
#include <velk-render/glsl/cut_large_queue.inc>

/**
 * Builds the tree over each partition's records that chose again this frame (kCutNodeWords):
 * its ray list sorted by where its records are (Morton order, the widest after them, those
 * without a box last and left out), then leaves of four records in that order, and the radix
 * tree over them (VELK_LBVH_GLSL). `pad` grows every box, for instances that a motion function
 * moves, by twice how far it can move one, so that the tree holds wherever they are while it is
 * kept. Also the partition's box around all it draws, whether it is always visited, and its counts
 * for the log (VELK_PART_BOX, ALWAYS, FOR_RAYS, QUADS), which instance_cut_visible_src goes by.
 * A work group per partition (`mode` 0); a partition past kRaySortMax records that the cut's
 * queue of long lists has room for (kCutLargeHeaderWords) is queued instead, scanned by
 * instance_cut_scan_src and sorted and built by instance_cut_large_src. One whose scan leaves
 * kRaySortMax entries or fewer is sorted and built here, as its own group would (`mode` 1, a work
 * group a queued list).
 */
#include <velk-render/glsl/instance_cut_tree.inc>
inline constexpr string_view instance_cut_tree_src = VELK_INSTANCE_CUT_TREE_GLSL;

/**
 * Scans the partitions instance_cut_tree_src queued, a work group of 256 threads a block of
 * kCutLargeBlockEntries records or a list: by `mode`, 0 whether rays trace each record and each
 * block's share of what the partition's own group adds up (VELK_CUT_LARGE_BLOCK); 1 for each list
 * where its blocks' entries start, its count, its partition's box and counts and the box around
 * its entries' centres; 2 the entries written, in the records' order. The list, the partition's
 * words and the box are those a partition's own work group makes.
 */
#include <velk-render/glsl/instance_cut_scan.inc>
inline constexpr string_view instance_cut_scan_src = VELK_INSTANCE_CUT_SCAN_GLSL;

/**
 * Sorts a kept cut's ray lists past kRaySortMax entries that instance_cut_scan_src made, all at
 * once, a work group of 256 threads a block of kCutLargeBlockEntries entries or a list: by
 * `mode`, 0 the group counts of the passes after it, from the queue (one group); 1 each entry's
 * code (as velk_sort_ray_list_large's) and the box count; then for each of four 8-bit digits, 2
 * each block's digit counts, 3 for each list where each block's entries of each digit go, and 4
 * the entries to there, keeping their order (`shift` the digit's lowest bit, `source` the
 * scratch buffer read); then 5 each place's record in the new order and 6 the entries written in
 * it. The order is velk_sort_ray_list_large's, entry for entry. Then each list's tree, the one
 * velk_lbvh_build_bounded builds, node for node: 7 each leaf's key; then for the sorted tree and,
 * with `source` 1, again for those built balanced instead, 8 each node's children, 9 the boxes and
 * heights from the leaves up, and 10 the root, or the retry for a tree too deep (a group a list).
 */
#include <velk-render/glsl/instance_cut_large.inc>
inline constexpr string_view instance_cut_large_src = VELK_INSTANCE_CUT_LARGE_GLSL;

/**
 * Builds a view's top tree for rays (kRayTopHeaderWords) over the trees of its kept cuts'
 * partitions, as they are this frame: each leaf's box is its partition's root's. Up to
 * kRaySortMax leaves, a radix tree (VELK_LBVH_GLSL) over those with records for rays in Morton
 * order of their boxes' centres; past that, a balanced tree over all of them in the order of the
 * cuts and of each cut's partition table, those without records not referenced. One work group.
 */
#include <velk-render/glsl/instance_cut_top.inc>
inline constexpr string_view instance_cut_top_src = VELK_INSTANCE_CUT_TOP_GLSL;

/**
 * Lists the records instance_cut_draw_src walks: those of each partition whose box around what
 * it draws (VELK_PART_BOX) is in view, or that is always visited, a run a page (its first record's
 * pool place and the end of those in use), the group counts of the drawing dispatch in
 * VELK_CUT_VISIBLE_GROUPS. With `whole` set, every partition's. Adds up the partitions' counts
 * for the log. A thread per partition.
 */
#include <velk-render/glsl/instance_cut_visible.inc>
inline constexpr string_view instance_cut_visible_src = VELK_INSTANCE_CUT_VISIBLE_GLSL;

/**
 * Draws from a kept cut: each record a partition holds whose instance is near enough, and
 * whose cluster is in view and not hidden behind what was drawn last frame, goes where
 * instance_cull_compute_src puts what it chooses: compute raster, the visual's chosen list, or
 * the second occlusion phase. A whole instance record adds its slot to its mesh's visible list,
 * and a quad in view goes to the view's quad list. Only the records of partitions in view, which
 * instance_cut_visible_src lists in runs: a work group a run, a thread a record. With `whole`
 * set, every record is drawn wherever it is, for a view that shows another camera's choice from
 * outside.
 */
#include <velk-render/glsl/instance_cut_draw.inc>
inline constexpr string_view instance_cut_draw_src = VELK_INSTANCE_CUT_DRAW_GLSL;

/**
 * Tests each instance's bounding sphere against the view and appends the visible
 * slots to its mesh's list in the culling arena, which the vertex shader reads through
 * velk_instance. The table at table_word holds 12 words per mesh: its bounding sphere
 * (center xyz and radius, as floats), the count of visible slots, the word of the
 * list, the list's capacity, a spare word, and where the mesh's primitives drawn by cluster
 * are: the word of their entries, and how many, as described at instance_cluster_runs_src. Each cluster a visible instance uses is counted there.
 * Instances with a zero matrix have been removed.
 *
 * For visuals and batches without a kept cut (kCutHeaderWords): partitioned visuals with one
 * choose and draw through instance_cut_choose_src and instance_cut_draw_src instead.
 *
 * With items_word set, one dispatch goes through the instances of many batches. An item
 * is kPlainItemWords: the instance's index in the instance arena, its slot, its batch's table,
 * and its batch's draw.
 * Such a batch's table has no list of its own.
 *
 * The clusters each visible instance uses are chosen here once, and written at chosen_word
 * for instance_cluster_fill_src. A cluster that does not fit is not counted either, so the
 * counts always match the records.
 */
#include <velk-render/glsl/instance_cull_compute.inc>
inline constexpr string_view instance_cull_compute_src = VELK_INSTANCE_CULL_COMPUTE_GLSL;

/**
 * Tests what failed instance_cull_compute_src's occlusion test again, against the pyramid of
 * what this frame has drawn so far, and adds what is visible after what that test passed.
 */
#include <velk-render/glsl/instance_retest_compute.inc>
inline constexpr string_view instance_retest_compute_src = VELK_INSTANCE_RETEST_COMPUTE_GLSL;

/**
 * Lays out one primitive's draws by cluster, once its instances are counted. Each cluster in
 * use gets an indexed command, instanced over its own run of the list, so that instances
 * using the same cluster share a draw. An entry is kClusterEntryWords: the word of the list, which
 * is followed by each position's kept cut record (0xFFFFFFFF for none) and its cluster's record, its capacity, the word of the cluster table
 * (kLodClusterWords words per cluster, starting with the word of the cluster's first index in
 * the mesh-word arena and its triangle count), the cluster count, the words of the counts, the cursors and the commands, one of
 * each per cluster, how many commands are in use, the word of the tree over its clusters and its flags, how
 * many positions the first phase used, the second phase's commands and how many are in use, and
 * the draw data record its pixels name (kClusterEntryDrawWord), and the word of its clusters'
 * ray headers (kClusterEntryRayWord).
 *
 * With entries_word set, one dispatch goes through the entries listed there. The first
 * invocation writes the group counts of the dispatch that sorts the records at chosen_word.
 * With second_phase, the runs follow the positions the first phase used.
 */
#include <velk-render/glsl/instance_cluster_runs.inc>
inline constexpr string_view instance_cluster_runs_src = VELK_INSTANCE_CLUSTER_RUNS_GLSL;

/**
 * Sorts the records instance_cull_compute_src wrote into the runs of their clusters.
 */
#include <velk-render/glsl/instance_cluster_fill.inc>
inline constexpr string_view instance_cluster_fill_src = VELK_INSTANCE_CLUSTER_FILL_GLSL;

/**
 * Writes the group counts of the dispatch that draws the clusters culling listed for compute,
 * one group each, in rows of at most 65535 (what every device takes along one axis), and how
 * many of them are drawn, as kTinyHeaderWords describes.
 */
#include <velk-render/glsl/instance_tiny_args.inc>
inline constexpr string_view instance_tiny_args_src = VELK_INSTANCE_TINY_ARGS_GLSL;

/**
 * Rasterizes the clusters culling listed for compute, a work group per cluster and a thread
 * per triangle. Corners are placed as the vertex shader would from the vertices as stored:
 * clusters whose material has a vertex function stay with the hardware. A pixel is covered
 * when its centre is inside the triangle, at the depth interpolated there, where the hardware
 * drew nothing nearer. A triangle covers at most `most` pixels in each direction; culling
 * leaves a cluster with a larger one to the hardware, so this only guards against error.
 *
 * Two dispatches. With `mode` 0, keeps the nearest depth of each pixel in the plane at
 * planes_word, with a fixed number per triangle in its lowest bits, so that exact ties go to
 * one triangle. With 1, the triangle whose depth that is writes its ids to the first two words
 * of the pixel's record after that plane (velk_pixel_ids). The resolve takes both from there.
 *
 * instance_tiny_draw_one_pass_src does it in one dispatch where the device has 64-bit buffer
 * atomics: each pixel keeps a 64-bit word at wide_word (an even word), the depth in its high
 * half and in its low half the cluster's place in the list (kTinyOnePassRecords of them at
 * most) and the triangle, its lowest 8 bits. instance_tiny_unpack_src then writes the plane
 * and the ids from it.
 */
#include <velk-render/glsl/tiny_draw.inc>

inline constexpr string_view instance_tiny_draw_src = "#version 450\n#include \"velk.glsl\"\n" VELK_TINY_DRAW_GLSL;

inline constexpr string_view instance_tiny_draw_one_pass_src =
    "#version 450\n"
    "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n"
    "#extension GL_EXT_shader_atomic_int64 : require\n"
    "#define VELK_TINY_ONE_PASS 1\n"
    "#include \"velk.glsl\"\n" VELK_TINY_DRAW_GLSL;

/**
 * After instance_tiny_draw_one_pass_src, a thread a pixel: where it drew, writes the depth to
 * the plane at planes_word and the ids to the pixel's record after it, as the two dispatches of
 * instance_tiny_draw_src leave them, and clears the pixel's 64-bit word for the next frame.
 */
#include <velk-render/glsl/instance_tiny_unpack.inc>
inline constexpr string_view instance_tiny_unpack_src = VELK_INSTANCE_TINY_UNPACK_GLSL;

/**
 * Writes the instanced visuals' items (kPlainItemWords), a thread per item from `first_item` on:
 * its instance's place in the raster region from its visual's slot table (none without a place,
 * or before the table is there), its instance's index there, and by its instance's mesh
 * (params.y) that mesh's draw, ray shape kind and ray data. The visual is found in the descriptors
 * (kItemDescriptorHeaderWords then kItemMeshWords a mesh each), by the items it covers.
 */
#include <velk-render/glsl/instance_items.inc>
inline constexpr string_view instance_items_src = VELK_INSTANCE_ITEMS_GLSL;

/**
 * Names a kept cut's records again after their batches were made again: each record a page of
 * its pool holds for a partition gets its cluster entry, or a whole mesh's table, from `pairs`
 * (old word, new word, sorted by the old), its plain item from `items` (by old item), or its
 * instanced visual's item from `blocks`. A record whose words do not carry over becomes
 * kCutStale. A thread per record the pool holds.
 */
#include <velk-render/glsl/instance_cut_rename.inc>
inline constexpr string_view instance_cut_rename_src = VELK_INSTANCE_CUT_RENAME_GLSL;

/**
 * After a rename, makes each partition holding a record that did not carry over (kCutStale)
 * choose again this frame: its table entry's chunk word no longer matches, so the decide pass
 * chooses it from nothing, without taking over its old records' light, and has none should no
 * free pages be there for it. A work group per partition, over its records by its page table of
 * the last frame, which the decide pass then reads.
 */
#include <velk-render/glsl/instance_cut_stale.inc>
inline constexpr string_view instance_cut_stale_src = VELK_INSTANCE_CUT_STALE_GLSL;

} // namespace velk

#endif // VELK_RENDER_FRAME_INSTANCE_CULL_SHADERS_H
