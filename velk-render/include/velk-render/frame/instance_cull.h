#ifndef VELK_RENDER_FRAME_INSTANCE_CULL_H
#define VELK_RENDER_FRAME_INSTANCE_CULL_H

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
/// (kCutRayWords) for each record rays trace (those at the light's level: records for light
/// only, clusters as coarse as the light, quads, whole objects): its record's place in the pool
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
/// A kept cut's record of a cluster for light only has this on its entry: a cluster coarse
/// enough for light (kLightPixels) over drawn ones finer than that. It holds light and is neither
/// drawn nor seen by rays. Mirrors VELK_CUT_LIGHT_ONLY.
inline constexpr uint32_t kCutLightOnly = 0x80000000u;
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
/// (2), for light only (3), skipped with all under its node too coarse (4) or with a parent that
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

// The trace's shader side (kTraceNone and the rest above). The shader declares cull_words.
#define VELK_TRACE_GLSL R"(
#define VELK_TRACE_NONE 0xFFFFFFFFu
#define VELK_TRACE_CHOOSE 1u
#define VELK_TRACE_PARTITION 2u
#define VELK_TRACE_DRAW 3u

// The ray through the traced pixel, from the camera; false when nothing is traced.
bool velk_trace_ray(GlobalData g, out vec3 origin, out vec3 dir)
{
    origin = vec3(0.0);
    dir = vec3(0.0, 0.0, 1.0);
    uint t = g.trace;
    if (t == VELK_TRACE_NONE) return false;
    vec2 ndc = (vec2(float(cull_words.data[t]), float(cull_words.data[t + 1u])) + 0.5) * g.viewport.zw * 2.0 - 1.0;
    vec4 near_h = g.inverse_view_projection * vec4(ndc, 1.0, 1.0);
    vec4 mid_h = g.inverse_view_projection * vec4(ndc, 0.5, 1.0);
    origin = near_h.xyz / near_h.w;
    dir = normalize(mid_h.xyz / mid_h.w - origin);
    return true;
}

// Whether that ray passes within `radius` of `center`, from the camera.
bool velk_trace_sphere(GlobalData g, vec3 center, float radius)
{
    vec3 o;
    vec3 d;
    if (!velk_trace_ray(g, o, d)) return false;
    vec3 oc = center - o;
    return length(oc - d * max(dot(oc, d), 0.0)) <= radius;
}

// Whether that ray passes the box from `lo` to `hi`, from the camera.
bool velk_trace_box(GlobalData g, vec3 lo, vec3 hi)
{
    vec3 o;
    vec3 d;
    if (!velk_trace_ray(g, o, d)) return false;
    vec3 inv = 1.0 / d;
    vec3 t0 = (lo - o) * inv;
    vec3 t1 = (hi - o) * inv;
    vec3 tn = min(t0, t1);
    vec3 tf = max(t0, t1);
    return min(min(tf.x, tf.y), tf.z) >= max(max(max(tn.x, tn.y), tn.z), 0.0);
}

void velk_trace_note(GlobalData g, uint stage, uint outcome, uint a, uint b, uint c)
{
    uint t = g.trace;
    if (t == VELK_TRACE_NONE) return;
    uint at = atomicAdd(cull_words.data[t + 2u], 1u);
    if (at >= cull_words.data[t + 3u]) return;
    uint w = t + 6u + at * 4u;
    cull_words.data[w] = stage | (outcome << 8u);
    cull_words.data[w + 1u] = a;
    cull_words.data[w + 2u] = b;
    cull_words.data[w + 3u] = c;
}
)"

// Where a cut's parts are. The shader declares cull_words.
#define VELK_CUT_GLSL R"(
uint velk_cut_state(uint cut, uint index)
{
    return cull_words.data[cut + VELK_CUT_STATES_WORD] + index * VELK_CUT_PARTITION_WORDS;
}

// The word of the current page table, or the last frame's.
uint velk_cut_table(uint cut, bool current)
{
    uint parity = cull_words.data[cut + VELK_CUT_TABLE_PARITY];
    return cull_words.data[cut + VELK_CUT_TABLES + (current ? parity : 1u - parity)];
}

// The pool place of place `i` of a partition whose page table starts at word `table`: of its
// records, or of its ray list's entries.
uint velk_cut_place(uint table, uint i)
{
    return cull_words.data[table + i / VELK_CUT_PAGE_RECORDS] * VELK_CUT_PAGE_RECORDS + i % VELK_CUT_PAGE_RECORDS;
}

// The word of the record at pool place `place`.
uint velk_cut_record_word(uint cut, uint place)
{
    return cull_words.data[cut + VELK_CUT_RECORD_POOL] + place * VELK_RECORD_END;
}

// The word of the ray list entry at pool place `place`.
uint velk_cut_ray_word(uint cut, uint place)
{
    return cull_words.data[cut + VELK_CUT_RAY_POOL] + place * VELK_CUT_RAY_WORDS;
}

// The pool place of node `i` of the tree of a partition whose page table starts at word `table`.
uint velk_cut_node_place(uint table, uint i)
{
    return cull_words.data[table + i / VELK_CUT_PAGE_NODES] * VELK_CUT_PAGE_NODES + i % VELK_CUT_PAGE_NODES;
}

// The word of the tree node at pool place `place`.
uint velk_cut_node_word(uint cut, uint place)
{
    return cull_words.data[cut + VELK_CUT_NODE_POOL] + place * VELK_CUT_NODE_WORDS;
}

// The pages `records` records take.
uint velk_cut_pages(uint records)
{
    return (records + VELK_CUT_PAGE_RECORDS - 1u) / VELK_CUT_PAGE_RECORDS;
}

// Nodes in the tree over a partition's `records` records: one less than its leaves of four, at
// least one. Mirrors cut_tree_nodes.
uint velk_cut_tree_nodes(uint records)
{
    if (records == 0u) return 0u;
    uint leaves = (records + 3u) / 4u;
    return leaves > 1u ? leaves - 1u : 1u;
}
)"

/// The most entries the ray trees' builds sort by where they are (VELK_SORT_GLSL); a partition
/// with more records for rays, or a top tree with more partitions, keeps its order. Mirrors
/// kSortMax there.
inline constexpr uint32_t kRaySortMax = 2048;

// Sorting a ray tree's entries by where they are, in a work group's shared memory, for the tree
// builds: a key for each of up to kRaySortMax entries, its Morton code above its index in the
// low kSortIndexBits bits, sorted ascending by a bitonic sort.
#define VELK_SORT_GLSL R"(
const uint kSortMax = 2048u;
const uint kSortIndexBits = 11u;
const uint kSortIndexMask = 2047u;
shared uint s_order[kSortMax];

// The low 10 bits of `v` spread to every third bit.
uint velk_morton_spread(uint v)
{
    v &= 0x3FFu;
    v = (v | (v << 16u)) & 0x030000FFu;
    v = (v | (v << 8u)) & 0x0300F00Fu;
    v = (v | (v << 4u)) & 0x030C30C3u;
    v = (v | (v << 2u)) & 0x09249249u;
    return v;
}

// The 30-bit Morton code of `p` within the box `lo` to `hi`. One grid scale on all axes, the
// box's widest, so that a flat spread still gets cells that are square.
uint velk_morton(vec3 p, vec3 lo, vec3 hi)
{
    float extent = max(max(hi.x - lo.x, hi.y - lo.y), max(hi.z - lo.z, 1e-20));
    vec3 q = clamp((p - lo) / extent, 0.0, 1.0) * 1023.0;
    return (velk_morton_spread(uint(q.x)) << 2u) | (velk_morton_spread(uint(q.y)) << 1u) | velk_morton_spread(uint(q.z));
}

// Sorts s_order's first `n` keys ascending, `n` a power of two, the `lanes` threads of the work
// group together; all of them call it.
void velk_sort_keys(uint n, uint lane, uint lanes)
{
    for (uint k = 2u; k <= n; k <<= 1u) {
        for (uint j = k >> 1u; j > 0u; j >>= 1u) {
            for (uint i = lane; i < n; i += lanes) {
                uint l = i ^ j;
                if (l > i) {
                    uint a = s_order[i];
                    uint b = s_order[l];
                    if ((a > b) == ((i & k) == 0u)) {
                        s_order[i] = b;
                        s_order[l] = a;
                    }
                }
            }
            memoryBarrierShared();
            barrier();
        }
    }
}
)"

// A ray tree as a radix tree over its leaves in their sorted order (Karras 2012), for the tree
// builds, after VELK_CUT_GLSL. The shader defines velk_lbvh_key, the sorted code of leaf g
// without its place, so that leaves with the same code split evenly by their places;
// velk_lbvh_leaf_ref, its reference in a node, 0xFFFFFFFF for none; and
// velk_lbvh_leaf_box, the box of the leaf a reference names; velk_lbvh_node_word, the word of
// the tree's node i; velk_lbvh_node_ref, what its parent names it by (never with the leaf bit);
// velk_lbvh_ref_word, the word of the node a reference names; and velk_store_box and
// velk_load_vec3. With g_lbvh_balanced each leaf's key is its place, which makes the tree
// balanced.
#define VELK_LBVH_GLSL R"(
bool g_lbvh_balanced = false;
uint velk_lbvh_key(uint g);
uint velk_lbvh_leaf_ref(uint g);
void velk_lbvh_leaf_box(uint ref, out vec3 lo, out vec3 hi);
uint velk_lbvh_node_word(uint i);
uint velk_lbvh_node_ref(uint i);
uint velk_lbvh_ref_word(uint ref);
void velk_store_box(uint at, vec3 lo, vec3 hi);
vec3 velk_load_vec3(uint at);

// Whether a reference names a node, not a leaf or nothing.
bool velk_lbvh_is_node(uint ref)
{
    return ref != 0xFFFFFFFFu && (ref & 0x80000000u) == 0u;
}

// How many leading bits the keys of leaves i and j of n share; -1 past either end. Keys alike
// go on by their places.
int velk_lbvh_delta(int i, int j, int n)
{
    if (j < 0 || j >= n) return -1;
    uint a = g_lbvh_balanced ? uint(i) : velk_lbvh_key(uint(i));
    uint b = g_lbvh_balanced ? uint(j) : velk_lbvh_key(uint(j));
    if (a != b) return 31 - findMSB(a ^ b);
    return 63 - findMSB(uint(i ^ j));
}

// The children of internal node i (0 the root) of the radix tree over n leaves, n at least 2:
// a leaf as 0x80000000 and its place, else an internal node.
void velk_lbvh_children(int i, int n, out uint left, out uint right)
{
    int d = velk_lbvh_delta(i, i + 1, n) - velk_lbvh_delta(i, i - 1, n) >= 0 ? 1 : -1;
    // The other end of its range: past it the shared prefix is no longer than toward i - d.
    int dmin = velk_lbvh_delta(i, i - d, n);
    int lmax = 2;
    for (uint k = 0u; k < 30u && velk_lbvh_delta(i, i + lmax * d, n) > dmin; ++k) lmax *= 2;
    int l = 0;
    for (int t = lmax / 2; t >= 1; t /= 2) {
        if (velk_lbvh_delta(i, i + (l + t) * d, n) > dmin) l += t;
    }
    int j = i + l * d;
    // Where the range splits: the last leaf that shares more than the range does with i.
    int dnode = velk_lbvh_delta(i, j, n);
    int s = 0;
    int div = 2;
    for (uint k = 0u; k < 32u; ++k) {
        int t = (l + div - 1) / div;
        if (velk_lbvh_delta(i, i + (s + t) * d, n) > dnode) s += t;
        if (t <= 1) break;
        div *= 2;
    }
    int split = i + s * d + min(d, 0);
    left = min(i, j) == split ? 0x80000000u | uint(split) : uint(split);
    right = max(i, j) == split + 1 ? 0x80000000u | uint(split + 1) : uint(split + 1);
}

// Builds the radix tree over the `n` leaves into the tree's `node_count` nodes
// (velk_lbvh_node_word, kCutNodeWords each): internal node i at node_count - 1 - i, the root
// last, the nodes before the tree's cleared; then each node's boxes from its children's, from
// the bottom up, a round a level, and its height (VELK_CUT_NODE_LEVEL, 1 over leaves only).
// Returns the root's height, 0 when it did not finish. All `lanes` threads of the work group
// call it.
uint velk_lbvh_build(uint node_count, uint n, uint lane, uint lanes)
{
    uint internal = n > 1u ? n - 1u : 1u;
    for (uint s = lane; s + internal < node_count; s += lanes) {
        uint node = velk_lbvh_node_word(s);
        velk_store_box(node + VELK_CUT_NODE_LEFT_MIN, vec3(1e30), vec3(-1e30));
        velk_store_box(node + VELK_CUT_NODE_RIGHT_MIN, vec3(1e30), vec3(-1e30));
        cull_words.data[node + VELK_CUT_NODE_LEFT_REF] = 0xFFFFFFFFu;
        cull_words.data[node + VELK_CUT_NODE_RIGHT_REF] = 0xFFFFFFFFu;
        cull_words.data[node + VELK_CUT_NODE_LEVEL] = 0u;
        cull_words.data[node + VELK_CUT_NODE_ROOT] = 0u;
    }
    // Each internal node's children. Until the boxes are done, ROOT holds the round a node was
    // done in, plus one.
    for (uint i = lane; i < internal; i += lanes) {
        uint node = velk_lbvh_node_word(node_count - 1u - i);
        uint left = 0xFFFFFFFFu;
        uint right = 0xFFFFFFFFu;
        if (n >= 2u) {
            uint l;
            uint r;
            velk_lbvh_children(int(i), int(n), l, r);
            left = (l & 0x80000000u) != 0u ? velk_lbvh_leaf_ref(l & 0x7FFFFFFFu) : velk_lbvh_node_ref(node_count - 1u - l);
            right = (r & 0x80000000u) != 0u ? velk_lbvh_leaf_ref(r & 0x7FFFFFFFu) : velk_lbvh_node_ref(node_count - 1u - r);
        } else if (n == 1u) {
            left = velk_lbvh_leaf_ref(0u);
        }
        cull_words.data[node + VELK_CUT_NODE_LEFT_REF] = left;
        cull_words.data[node + VELK_CUT_NODE_RIGHT_REF] = right;
        cull_words.data[node + VELK_CUT_NODE_LEVEL] = 0u;
        cull_words.data[node + VELK_CUT_NODE_ROOT] = 0u;
    }
    memoryBarrierBuffer();
    barrier();
    // A node is done once its child nodes were done in an earlier round, whose boxes the
    // barrier since has made seen. The keys all differ, so the tree is at most 64 deep.
    uint root = velk_lbvh_node_word(node_count - 1u);
    for (uint round = 1u; round <= 72u; ++round) {
        for (uint i = lane; i < internal; i += lanes) {
            uint node = velk_lbvh_node_word(node_count - 1u - i);
            if (cull_words.data[node + VELK_CUT_NODE_ROOT] != 0u) continue;
            uint height = 0u;
            bool ready = true;
            for (uint k = 0u; k < 2u; ++k) {
                uint ref = cull_words.data[node + VELK_CUT_NODE_LEFT_REF + k];
                if (!velk_lbvh_is_node(ref)) continue;
                uint below = velk_lbvh_ref_word(ref);
                uint done = cull_words.data[below + VELK_CUT_NODE_ROOT];
                if (done == 0u || done >= round) ready = false;
                height = max(height, cull_words.data[below + VELK_CUT_NODE_LEVEL]);
            }
            if (!ready) continue;
            for (uint k = 0u; k < 2u; ++k) {
                uint ref = cull_words.data[node + VELK_CUT_NODE_LEFT_REF + k];
                vec3 lo = vec3(1e30);
                vec3 hi = vec3(-1e30);
                if (ref == 0xFFFFFFFFu) {
                } else if ((ref & 0x80000000u) != 0u) {
                    velk_lbvh_leaf_box(ref, lo, hi);
                } else {
                    uint below = velk_lbvh_ref_word(ref);
                    lo = min(velk_load_vec3(below + VELK_CUT_NODE_LEFT_MIN), velk_load_vec3(below + VELK_CUT_NODE_RIGHT_MIN));
                    hi = max(velk_load_vec3(below + VELK_CUT_NODE_LEFT_MAX), velk_load_vec3(below + VELK_CUT_NODE_RIGHT_MAX));
                }
                velk_store_box(node + VELK_CUT_NODE_LEFT_MIN + k * (VELK_CUT_NODE_RIGHT_MIN - VELK_CUT_NODE_LEFT_MIN), lo, hi);
            }
            cull_words.data[node + VELK_CUT_NODE_LEVEL] = height + 1u;
            cull_words.data[node + VELK_CUT_NODE_ROOT] = round;
        }
        memoryBarrierBuffer();
        barrier();
        if (cull_words.data[root + VELK_CUT_NODE_ROOT] != 0u) break;
    }
    uint height = cull_words.data[root + VELK_CUT_NODE_ROOT] != 0u ? cull_words.data[root + VELK_CUT_NODE_LEVEL] : 0u;
    memoryBarrierBuffer();
    barrier();
    for (uint i = lane; i < internal; i += lanes) {
        cull_words.data[velk_lbvh_node_word(node_count - 1u - i) + VELK_CUT_NODE_ROOT] = i == 0u ? 1u : 0u;
    }
    memoryBarrierBuffer();
    barrier();
    return height;
}

// Builds the radix tree over `n` leaves (velk_lbvh_build), and a balanced one instead where it
// is deeper than a balanced one by more than 16 levels, so that the walk's stack holds
// (VELK_TRACE_STACK_SIZE).
uint g_lbvh_tried_height = 0u;  // the radix tree's height the last build tried, for the log
void velk_lbvh_build_bounded(uint node_count, uint n, uint lane, uint lanes, bool balanced)
{
    g_lbvh_balanced = balanced;
    uint height = velk_lbvh_build(node_count, n, lane, lanes);
    g_lbvh_tried_height = height;
    uint most = (n > 1u ? uint(findMSB(n - 1u)) + 1u : 1u) + 16u;
    if (!balanced && (height == 0u || height > most)) {
        g_lbvh_balanced = true;
        velk_lbvh_build(node_count, n, lane, lanes);
    }
}
)"

/// Words before the levels of a depth pyramid in the culling arena: the level count, the
/// first level's width and height, then each level's first word, as many as
/// kDepthPyramidMaxLevels. See depth_pyramid_compute_src.
inline constexpr uint32_t kDepthPyramidMaxLevels = 15;
inline constexpr uint32_t kDepthPyramidHeaderWords = 3 + kDepthPyramidMaxLevels;

/**
 * Fills one level of a depth pyramid in the culling arena with the farthest depth of what
 * each of its texels covers, depth being larger for what is nearer. The first level halves
 * the frame's depth texture, and later ones the level before. With compute_word set, the first
 * level takes each pixel's nearer of that texture and what compute raster drew there
 * (instance_tiny_draw_src), so that those pixels hide what is behind them too.
 */
inline constexpr string_view depth_pyramid_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 1, binding = 16, std430) buffer PyramidWords { uint data[]; } pyramid_words;

layout(push_constant) uniform PC {
    uint depth_id;       // the depth texture, when source_word is 0xFFFFFFFF
    uint source_word;
    uint source_width;
    uint source_height;
    uint target_word;
    uint width;
    uint height;
    uint compute_word;   // compute raster's depth plane, a word a pixel, or 0xFFFFFFFF for none
} pc;

float read_source(ivec2 at)
{
    if (pc.source_word == 0xFFFFFFFFu) {
        float depth = texelFetch(velk_textures[nonuniformEXT(pc.depth_id)], at, 0).r;
        if (pc.compute_word != 0xFFFFFFFFu) {
            // Its lowest bits break ties, not part of the depth.
            uint drawn = pyramid_words.data[pc.compute_word + uint(at.y) * pc.source_width + uint(at.x)];
            depth = max(depth, uintBitsToFloat(drawn & ~0xFFu));
        }
        return depth;
    }
    return uintBitsToFloat(pyramid_words.data[pc.source_word + uint(at.y) * pc.source_width + uint(at.x)]);
}

void main()
{
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= pc.width || id.y >= pc.height) return;

    // An odd source's last column or row belongs to the last texel, so that none is left out.
    ivec2 last = ivec2(1 + ((pc.source_width & 1u) != 0u && id.x == pc.width - 1u ? 1 : 0),
                       1 + ((pc.source_height & 1u) != 0u && id.y == pc.height - 1u ? 1 : 0));
    ivec2 limit = ivec2(pc.source_width, pc.source_height) - 1;
    float farthest = 1e30;
    for (int y = 0; y <= last.y; ++y) {
        for (int x = 0; x <= last.x; ++x) {
            farthest = min(farthest, read_source(min(ivec2(id) * 2 + ivec2(x, y), limit)));
        }
    }
    pyramid_words.data[pc.target_word + id.y * pc.width + id.x] = floatBitsToUint(farthest);
}
)";

/// The list of what failed the first occlusion test: VELK_RETEST_LIST, VELK_RETEST
/// (cull_layout.h).
inline constexpr uint32_t kCullRetestHeaderWords = kRetestListRecords;
inline constexpr uint32_t kCullRetestRecordWords = kRetestEnd;
inline constexpr uint32_t kCullRetestMaxRecords = 1u << 20;

// Whether a sphere around a point relative to the camera, as seen through vp, lies behind all
// that a depth pyramid holds there. A larger depth is nearer. Declares nothing the shader must
// provide but cull_words.
#define VELK_OCCLUSION_GLSL R"(
bool velk_behind_pyramid(uint pyramid, mat4 vp, vec3 center, float radius)
{
    uint levels = cull_words.data[pyramid];
    if (levels == 0u) return false;

    // The sphere's rectangle on screen and its nearest depth, from the sphere itself (Mara and
    // McGuire 2013): vp's rows are the screen's axes, scaled by the projection.
    vec3 r0 = vec3(vp[0][0], vp[1][0], vp[2][0]);
    vec3 r1 = vec3(vp[0][1], vp[1][1], vp[2][1]);
    vec3 r2 = vec3(vp[0][2], vp[1][2], vp[2][2]);
    vec3 r3 = vec3(vp[0][3], vp[1][3], vp[2][3]);
    vec2 lo;
    vec2 hi;
    float nearest;
    float lw = length(r3);
    if (lw < 1e-12) {
        // Orthographic: the centre moved by the radius along each axis.
        float w = vp[3][3];
        if (w <= 1e-5) return false;
        vec2 at = vec2(dot(r0, center) + vp[3][0], dot(r1, center) + vp[3][1]);
        vec2 reach = radius * vec2(length(r0), length(r1));
        lo = (at - reach) / w;
        hi = (at + reach) / w;
        nearest = (dot(r2, center) + vp[3][2] + radius * length(r2)) / w;
    } else {
        // In the camera's axes, z its distance ahead; the tangent lines to the sphere bound it.
        float px = length(r0);
        float py = length(r1);
        vec3 c = vec3((dot(r0, center) + vp[3][0]) / px, (dot(r1, center) + vp[3][1]) / py, (dot(r3, center) + vp[3][3]) / lw);
        if (c.z - radius <= 1e-5) return false;
        vec3 cr = c * radius;
        float czr2 = c.z * c.z - radius * radius;
        float vx = sqrt(c.x * c.x + czr2);
        float vy = sqrt(c.y * c.y + czr2);
        vec2 scale = vec2(px, py) / lw;
        lo = vec2((vx * c.x - cr.z) / (vx * c.z + cr.x), (vy * c.y - cr.z) / (vy * c.z + cr.y)) * scale;
        hi = vec2((vx * c.x + cr.z) / (vx * c.z - cr.x), (vy * c.y + cr.z) / (vy * c.z - cr.y)) * scale;
        // Its point nearest the camera.
        vec4 clip = vp * vec4(center - r3 / lw * radius, 1.0);
        nearest = clip.z / clip.w;
    }

    uvec2 first_size = uvec2(cull_words.data[pyramid + 1u], cull_words.data[pyramid + 2u]);
    vec2 a = clamp(lo * 0.5 + 0.5, vec2(0.0), vec2(1.0)) * vec2(first_size);
    vec2 b = clamp(hi * 0.5 + 0.5, vec2(0.0), vec2(1.0)) * vec2(first_size);

    // The level at which the rectangle spans at most two texels each way.
    float extent = max(max(b.x - a.x, b.y - a.y), 1.0);
    uint level = min(uint(ceil(log2(extent))), levels - 1u);
    uvec2 size = max(first_size >> level, uvec2(1u));
    uvec2 from = min(uvec2(a) >> level, size - 1u);
    uvec2 to = min(uvec2(b) >> level, size - 1u);
    uint words = cull_words.data[pyramid + 3u + level];
    float farthest = 1e30;
    for (uint y = from.y; y <= to.y; ++y) {
        for (uint x = from.x; x <= to.x; ++x) {
            farthest = min(farthest, uintBitsToFloat(cull_words.data[words + y * size.x + x]));
        }
    }
    return nearest < farthest;
}
)"

/// The list of clusters drawn in compute (instance_tiny_draw_src): VELK_TINY_LIST, VELK_TINY
/// (cull_layout.h).
inline constexpr uint32_t kTinyHeaderWords = kTinyListRecords;
inline constexpr uint32_t kTinyRecordWords = kTinyEnd;

// Adds a cluster to the list of those drawn in compute, false when it is full. The shader has
// pc.tiny_word and pc.tiny_capacity.
#define VELK_TINY_ADD_GLSL R"(
bool velk_tiny_add(uint draw, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[pc.tiny_word + VELK_TINY_LIST_COUNT], 1u);
    if (at >= pc.tiny_capacity) return false;
    uint record = pc.tiny_word + VELK_TINY_LIST_RECORDS + at * VELK_TINY_END;
    cull_words.data[record + VELK_TINY_DRAW] = draw;
    cull_words.data[record + VELK_TINY_CLUSTER] = cluster;
    cull_words.data[record + VELK_TINY_SLOT] = slot;
    cull_words.data[record + VELK_TINY_CUT_RECORD] = cut_record;
    return true;
}
)"

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
#define VELK_LOD_SELECT_GLSL R"(
#ifdef VELK_LOD_LIGHT
vec3 g_view_cam;
bool g_out_of_view;
uint g_visited;  // hierarchy nodes this thread walked

// Whether a sphere at `center` (world space) of `radius` is out of the camera's view, widened
// by a quarter on each side, so that turning a little keeps what was chosen.
bool velk_lod_out_of_view(vec3 center, float radius, mat4 vp)
{
    vec4 clip = vp * vec4(center - g_view_cam, 1.0);
    vec3 r0 = vec3(vp[0][0], vp[1][0], vp[2][0]);
    vec3 r1 = vec3(vp[0][1], vp[1][1], vp[2][1]);
    vec3 r3 = vec3(vp[0][3], vp[1][3], vp[2][3]);
    float w = clip.w * 1.25 + radius * length(r3);
    return clip.w + radius * length(r3) < 0.0 || abs(clip.x) > w + radius * length(r0) || abs(clip.y) > w + radius * length(r1);
}
#endif
vec3 velk_lod_center(uint word, mat4 world, vec3 cam)
{
    return mat3(world) * vec3(uintBitsToFloat(cull_words.data[word]), uintBitsToFloat(cull_words.data[word + 1u]),
                              uintBitsToFloat(cull_words.data[word + 2u]))
        + (world[3].xyz - cam);
}

// `word` holds a sphere's center and radius, then an error.
float velk_lod_pixels(uint word, mat4 world, float scale, vec3 cam, float pixels_per_unit)
{
    float away = length(velk_lod_center(word, world, cam)) - uintBitsToFloat(cull_words.data[word + 3u]) * scale;
    return uintBitsToFloat(cull_words.data[word + 4u]) * scale * pixels_per_unit / max(away, 1e-3);
}

// What child `child` of a hierarchy node is to the choice (VELK_LOD_SKIP, VELK_LOD_DESCEND,
// VELK_LOD_CHOSEN or VELK_LOD_LIGHT_ONLY), and in `c` the node it is or its cluster.
// `clusters` is the entry's cluster words.
#define VELK_LOD_SKIP 0u
#define VELK_LOD_DESCEND 1u
#define VELK_LOD_CHOSEN 2u
#define VELK_LOD_LIGHT_ONLY 3u
uint velk_lod_child(uint child, uint clusters, mat4 world, float scale, vec3 lod_cam, mat4 vp, float pixels_per_unit,
                    float max_pixels, out uint c)
{
    c = 0u;
    float per_error = scale * pixels_per_unit;
#ifdef VELK_LOD_LIGHT
    float coarsest = max(max_pixels, pc.light_pixels);
#else
    float coarsest = max_pixels;
#endif
    // A node's child is 8 words: a sphere holding the spheres the clusters under it are tested
    // with, the smallest of their errors, the largest of their parents', and what it is.
    uint ref = lod_nodes.data[child + 6u];
    if (ref == VELK_LOD_NO_CHILD) return VELK_LOD_SKIP;
    // Skipped when all under it are too coarse, or all have a parent that will do.
    vec3 center = vec3(uintBitsToFloat(lod_nodes.data[child]), uintBitsToFloat(lod_nodes.data[child + 1u]),
                       uintBitsToFloat(lod_nodes.data[child + 2u]));
    float dist = length(mat3(world) * center + (world[3].xyz - lod_cam));
    float reach = uintBitsToFloat(lod_nodes.data[child + 3u]) * scale;
    float finest = max_pixels;
#ifdef VELK_LOD_LIGHT
    // Out of view: no finer than the light's level.
    if (velk_lod_out_of_view(mat3(world) * center + world[3].xyz, reach, vp)) finest = pc.light_pixels;
#endif
#ifdef VELK_TRACE_WALK
    // The tree's shape (kTraceShape): which node this child is under, and which of its two.
    velk_trace_shape(ref, child);
    // The node's two bounds, as stored and on screen, for every child the walk of the entry
    // traced tests.
    velk_trace_numbers(ref | 0x08000000u, uintBitsToFloat(lod_nodes.data[child + 4u]),
                       uintBitsToFloat(lod_nodes.data[child + 4u]) * per_error / (dist + reach));
    velk_trace_numbers(ref | 0x04000000u, uintBitsToFloat(lod_nodes.data[child + 5u]),
                       uintBitsToFloat(lod_nodes.data[child + 5u]) * per_error / max(dist - reach, 1e-3));
#endif
    if (uintBitsToFloat(lod_nodes.data[child + 4u]) * per_error / (dist + reach) > coarsest) {
#ifdef VELK_TRACE_WALK
        if ((ref & VELK_LOD_LEAF) != 0u) velk_trace_walk(clusters, ref & ~VELK_LOD_LEAF, world, scale, 4u);
        else velk_trace_node(mat3(world) * center + world[3].xyz, reach, ref, 8u);
#endif
        return VELK_LOD_SKIP;
    }
    if (uintBitsToFloat(lod_nodes.data[child + 5u]) * per_error / max(dist - reach, 1e-3) <= finest) {
#ifdef VELK_TRACE_WALK
        if ((ref & VELK_LOD_LEAF) != 0u) velk_trace_walk(clusters, ref & ~VELK_LOD_LEAF, world, scale, 5u);
        else velk_trace_node(mat3(world) * center + world[3].xyz, reach, ref, finest > max_pixels ? 10u : 9u);
#endif
        return VELK_LOD_SKIP;
    }
    if ((ref & VELK_LOD_LEAF) == 0u) {
        c = ref;
        return VELK_LOD_DESCEND;
    }

    c = ref & ~VELK_LOD_LEAF;
    uint cluster = clusters + c * VELK_LOD_CLUSTER_WORDS;
    float own = velk_lod_pixels(cluster + VELK_LOD_CLUSTER_SPHERE, world, scale, lod_cam, pixels_per_unit);
    float parent = velk_lod_pixels(cluster + VELK_LOD_CLUSTER_PARENT_SPHERE, world, scale, lod_cam, pixels_per_unit);
    float limit = max_pixels;
#ifdef VELK_LOD_LIGHT
    // Out of view, the light's level is all there is of it: rays and light use it, and it
    // is drawn if it comes into view before its partition chooses again.
    vec3 at = mat3(world) * vec3(uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_SPHERE]),
                                 uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_SPHERE + 1u]),
                                 uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_SPHERE + 2u])) + world[3].xyz;
    bool out_of_view = velk_lod_out_of_view(at, uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_SPHERE + 3u]) * scale, vp);
    if (out_of_view) {
        limit = pc.light_pixels;
        g_out_of_view = true;
    }
#endif
#ifdef VELK_TRACE_WALK
    // The cluster's own error and its parent's, as stored and on screen.
    velk_trace_numbers(c | 0x02000000u, uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_SPHERE + 4u]), own);
    velk_trace_numbers(c | 0x01000000u, uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_PARENT_SPHERE + 4u]), parent);
    // Traced: chosen (2), for light only (3), too coarse (6), or its parent will do (7).
    velk_trace_walk(clusters, c, world, scale,
                    own <= limit && parent > limit ? 2u
#ifdef VELK_LOD_LIGHT
                    : !out_of_view && own <= pc.light_pixels && parent > pc.light_pixels ? 3u
#endif
                    : own > limit ? 6u : 7u);
#endif
    if (own <= limit && parent > limit) return VELK_LOD_CHOSEN;
#ifdef VELK_LOD_LIGHT
    if (!out_of_view && own <= pc.light_pixels && parent > pc.light_pixels) return VELK_LOD_LIGHT_ONLY;
#endif
    return VELK_LOD_SKIP;
}

// The walk's stack: 32 for each thread of the work group, which has 64, in shared memory, since as
// an array of its own it would be in scratch memory.
shared uint s_lod_stack[64u * 32u];

// Walks the tree of `entry` from node `start` down (0 is its root), depth first. `cam` is the
// camera, which occlusion and compute raster go by; `lod_cam` where levels of detail are chosen
// from: the camera, or its partition's viewpoint.
void velk_lod_walk(uint entry, uint start, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, mat4 vp, float height,
                   float max_pixels)
{
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * height * 0.5;
    uint clusters = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE];
    uint nodes = cull_words.data[entry + VELK_CLUSTER_ENTRY_NODES];
    // A deforming mesh's spheres are where its clusters are at rest, which says nothing
    // about whether they are hidden.
    bool deforming = (cull_words.data[entry + VELK_CLUSTER_ENTRY_FLAGS] & VELK_LOD_DEFORMING) != 0u;
    // The tree's nodes are among the mesh's nodes that rays walk, in lod_nodes. The tree is no
    // deeper than kLodMaxDepth.
    uint stack = gl_LocalInvocationIndex * 32u;
    int sp = 0;
    s_lod_stack[stack + uint(sp++)] = start;
    while (sp > 0) {
        uint node = nodes + s_lod_stack[stack + uint(--sp)] * VELK_LOD_NODE_STRIDE;
#ifdef VELK_LOD_LIGHT
        ++g_visited;
#endif
        // The second child first on the stack, so that the first child's subtree is walked
        // first: the order velk_lod_child's callers expand nodes in.
        uint later = VELK_LOD_NO_CHILD;
        for (uint k = 0u; k < 2u; ++k) {
            uint c;
            uint what = velk_lod_child(node + k * 8u, clusters, world, scale, lod_cam, vp, pixels_per_unit, max_pixels, c);
            if (what == VELK_LOD_DESCEND) {
                if (k == 0u) {
                    later = c;
                } else if (sp < 32) {
                    s_lod_stack[stack + uint(sp++)] = c;
                }
            } else if (what == VELK_LOD_CHOSEN) {
                velk_lod_chosen(entry, c, slot, world, scale, cam, lod_cam, pixels_per_unit, deforming);
            }
#ifdef VELK_LOD_LIGHT
            else if (what == VELK_LOD_LIGHT_ONLY) {
                velk_lod_light_chosen(entry, c, slot);
            }
#endif
        }
        if (later != VELK_LOD_NO_CHILD && sp < 32) s_lod_stack[stack + uint(sp++)] = later;
    }
}

void velk_lod_visit(uint entry, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, mat4 vp, float height,
                    float max_pixels)
{
    velk_lod_walk(entry, 0u, slot, world, scale, cam, lod_cam, vp, height, max_pixels);
}
)"

// Draws chosen cluster `c` of `entry`: in compute when its mean triangle on screen, the
// width of the sphere around its own triangles over the square root of their count, is
// less than pc.tiny_pixels, its largest triangle fits the most compute raster covers (4
// times that, see instance_tiny_draw_src), and the list has room. Else by the hardware: also
// while its draw is not known yet, and when its material moves vertices. `cam` is the camera,
// which compute raster decides by; `lod_cam` where levels of detail were chosen from.
// `cut_record` is the word of the kept cut record it is from, 0xFFFFFFFF for none. Comes after
// VELK_LOD_SELECT_GLSL; the shader declares pc.tiny_pixels, pc.lod_max_pixels, velk_lod_use and
// velk_tiny_add.
#define VELK_LOD_TAKE_GLSL R"(
void velk_lod_take(uint entry, uint c, uint slot, uint cut_record, mat4 world, float scale, vec3 cam, vec3 lod_cam,
                   float pixels_per_unit)
{
    // Drawn in place of finer clusters that are not loaded: its record names a mark, set here
    // where its error as built is too large, which asks for them.
    // Using it also says its own group is wanted, which keeps the group loaded.
    uint taken = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + c * VELK_LOD_CLUSTER_WORDS;
    uint own_mark = cull_words.data[taken + VELK_LOD_CLUSTER_OWN_MARK];
    if (own_mark != 0xFFFFFFFFu) {
        cull_words.data[own_mark] = 1u;
    }
    uint mark = cull_words.data[taken + VELK_LOD_CLUSTER_MARK];
    if (mark != 0xFFFFFFFFu) {
        float from = length(velk_lod_center(taken + VELK_LOD_CLUSTER_SPHERE, world, lod_cam))
                   - uintBitsToFloat(cull_words.data[taken + VELK_LOD_CLUSTER_SPHERE + 3u]) * scale;
        if (uintBitsToFloat(cull_words.data[taken + VELK_LOD_CLUSTER_FULL_ERROR]) * scale * pixels_per_unit / max(from, 1e-3) > pc.lod_max_pixels) {
            cull_words.data[mark] = 1u;
        }
    }
    uint draw = cull_words.data[entry + VELK_CLUSTER_ENTRY_DRAW];
    // Compute raster draws opaque into the visibility buffer: not what a blended batch's own pass draws.
    if (pc.tiny_pixels > 0.0 && draw != 0xFFFFFFFFu
        && (cull_words.data[entry + VELK_CLUSTER_ENTRY_FLAGS] & (VELK_LOD_MOVED | VELK_LOD_RAYS_ONLY)) == 0u) {
        uint cluster = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + c * VELK_LOD_CLUSTER_WORDS;
        float radius = uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale;
        float away = length(velk_lod_center(cluster + VELK_LOD_CLUSTER_OWN_SPHERE, world, cam));
        float across = 2.0 * radius * pixels_per_unit / max(away, 1e-3);
        float triangles = float(max(cull_words.data[cluster + VELK_LOD_CLUSTER_TRIANGLES], 1u));
        // The largest triangle as wide as it can be on screen, at the cluster's nearest.
        float largest = uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_LARGEST]) * scale * pixels_per_unit / max(away - radius, 1e-3);
        if (across < pc.tiny_pixels * sqrt(triangles) && largest < 4.0 * pc.tiny_pixels
            && velk_tiny_add(draw, cluster, slot, cut_record)) {
            return;
        }
    }
    velk_lod_use(entry, c, slot, cut_record);
}
)"

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
inline constexpr string_view instance_partition_cull_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint table_word;
    uint entry_count;
    uint globals_base;
    uint cut_word;
    float refresh;
} pc;
)" VELK_CUT_GLSL R"(
// Keeps partition `index`'s records, or lists it to choose again. `away` is from the camera
// to its bounds; `look` which way the camera looks. A partition that chose some clusters at
// the light's level only because they were out of view chooses again once the camera has
// turned by about 20 degrees, so that what comes into view gets its detail.
void velk_cut_decide(uint index, uint entry, uint chunks, vec3 cam, vec3 away, vec3 look)
{
    uint cut = pc.cut_word;
    uint state = velk_cut_state(cut, index);
    cull_words.data[state + VELK_PART_CHOOSING] = 0u;
    if (chunks == 0u) {
        cull_words.data[state + VELK_PART_COUNT] = 0u;
        cull_words.data[state + VELK_PART_RAY_COUNT] = 0u;
        return;
    }
    vec3 viewpoint = vec3(uintBitsToFloat(cull_words.data[state + VELK_PART_FROM]), uintBitsToFloat(cull_words.data[state + VELK_PART_FROM + 1u]),
                          uintBitsToFloat(cull_words.data[state + VELK_PART_FROM + 2u]));
    uint count = cull_words.data[state + VELK_PART_COUNT];
    bool same = cull_words.data[state + VELK_PART_CHUNK_FIRST] == cull_words.data[entry + VELK_PARTITION_ENTRY_FIRST_CHUNK]
             && cull_words.data[state + VELK_PART_CHUNKS] == chunks
             && cull_words.data[state + VELK_PART_QUAD_TREE] == cull_words.data[entry + VELK_PARTITION_ENTRY_QUAD_TREE];
    vec3 looked = vec3(uintBitsToFloat(cull_words.data[state + VELK_PART_LOOKED]), uintBitsToFloat(cull_words.data[state + VELK_PART_LOOKED + 1u]),
                       uintBitsToFloat(cull_words.data[state + VELK_PART_LOOKED + 2u]));
    bool turned = cull_words.data[state + VELK_PART_OUT_OF_VIEW] != 0u && dot(looked, look) < 0.94;
    bool retry = cull_words.data[state + VELK_PART_RETRY] != 0u;
    cull_words.data[state + VELK_PART_RETRY] = 0u;
    if (same && count != 0u && !turned && !retry && length(cam - viewpoint) <= pc.refresh * length(away)) {
        // Kept: its pages stay, its page table goes into the current one.
        uint pages = velk_cut_pages(count);
        uint first = atomicAdd(cull_words.data[cut + VELK_CUT_TABLE_USED], pages);
        uint was = cull_words.data[state + VELK_PART_TABLE_FIRST];
        if (first + pages <= cull_words.data[cut + VELK_CUT_PAGES] && was + pages <= cull_words.data[cut + VELK_CUT_PAGES]) {
            uint from = velk_cut_table(cut, false) + was;
            uint to = velk_cut_table(cut, true) + first;
            for (uint p = 0u; p < pages; ++p) cull_words.data[to + p] = cull_words.data[from + p];
            cull_words.data[state + VELK_PART_TABLE_FIRST] = first;
            cull_words.data[state + VELK_PART_OLD_COUNT] = 0u;
            atomicAdd(cull_words.data[cut + VELK_CUT_KEPT_PAGES], pages);
            return;
        }
        // Never so, since a page is in one table at most: nothing for it this frame.
        cull_words.data[state + VELK_PART_COUNT] = 0u;
        cull_words.data[state + VELK_PART_RAY_COUNT] = 0u;
        return;
    }
    // Choosing again, its new records take over the light of its old ones that are the same
    // (entry, cluster and slot), which stay on its old pages: also when its members changed, as
    // when one of them moved, so that what stayed keeps its light. Its pages and ray list as
    // they were, kept for the frame should no free pages be there for its new ones.
    cull_words.data[state + VELK_PART_LAST_TABLE_FIRST] = cull_words.data[state + VELK_PART_TABLE_FIRST];
    // When it chose: the clusters whose errors change after it have it choose again.
    cull_words.data[state + VELK_PART_CHOSE] = velk_globals.data[pc.globals_base].present_counter + 1u;
    cull_words.data[state + VELK_PART_OLD_COUNT] = count;
    cull_words.data[state + VELK_PART_OLD_RAY_COUNT] = count != 0u ? cull_words.data[state + VELK_PART_RAY_COUNT] : 0u;
    cull_words.data[state + VELK_PART_SAME] = same ? 1u : 0u;
    cull_words.data[state + VELK_PART_FROM] = floatBitsToUint(cam.x);
    cull_words.data[state + VELK_PART_FROM + 1u] = floatBitsToUint(cam.y);
    cull_words.data[state + VELK_PART_FROM + 2u] = floatBitsToUint(cam.z);
    cull_words.data[state + VELK_PART_LOOKED] = floatBitsToUint(look.x);
    cull_words.data[state + VELK_PART_LOOKED + 1u] = floatBitsToUint(look.y);
    cull_words.data[state + VELK_PART_LOOKED + 2u] = floatBitsToUint(look.z);
    cull_words.data[state + VELK_PART_OUT_OF_VIEW] = 0u;
    cull_words.data[state + VELK_PART_CHUNK_FIRST] = cull_words.data[entry + VELK_PARTITION_ENTRY_FIRST_CHUNK];
    cull_words.data[state + VELK_PART_CHUNKS] = chunks;
    cull_words.data[state + VELK_PART_QUAD_TREE] = cull_words.data[entry + VELK_PARTITION_ENTRY_QUAD_TREE];
    cull_words.data[state + VELK_PART_COUNT] = 0u;
    // Nothing for rays until its tree pass lists its new records, or the place pass gives its
    // old ones back when no free pages are there for the new.
    cull_words.data[state + VELK_PART_RAY_COUNT] = 0u;
    cull_words.data[state + VELK_PART_CHOOSING] = 1u;
    atomicAdd(cull_words.data[cut + VELK_CUT_CHOSE_AGAIN], 1u);
    uint at = atomicAdd(cull_words.data[cut + VELK_CUT_CHOOSE_GROUPS], chunks);
    cull_words.data[state + VELK_PART_CHUNK_START] = at;
    uint list = cull_words.data[cut + VELK_CUT_CHOOSE_WORD];
    uint first_chunk = cull_words.data[entry + VELK_PARTITION_ENTRY_FIRST_CHUNK];
    for (uint c = 0u; c < chunks; ++c) {
        cull_words.data[list + (at + c) * VELK_CHUNK_END + VELK_CHUNK_FIRST_SLOT] = first_chunk + c * 64u;
        cull_words.data[list + (at + c) * VELK_CHUNK_END + VELK_CHUNK_PARTITION] = index;
    }
}

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= pc.entry_count) return;

    uint entry = pc.table_word + index * VELK_CULL_ENTRY_WORDS;
    uint chunks = cull_words.data[entry + VELK_PARTITION_ENTRY_CHUNKS];

    vec3 lo = vec3(uintBitsToFloat(cull_words.data[entry + VELK_PARTITION_ENTRY_MIN]),
                   uintBitsToFloat(cull_words.data[entry + VELK_PARTITION_ENTRY_MIN + 1u]),
                   uintBitsToFloat(cull_words.data[entry + VELK_PARTITION_ENTRY_MIN + 2u]));
    vec3 hi = vec3(uintBitsToFloat(cull_words.data[entry + VELK_PARTITION_ENTRY_MAX]),
                   uintBitsToFloat(cull_words.data[entry + VELK_PARTITION_ENTRY_MAX + 1u]),
                   uintBitsToFloat(cull_words.data[entry + VELK_PARTITION_ENTRY_MAX + 2u]));

    GlobalData globals = velk_globals.data[pc.globals_base];
    lo -= globals.cam_pos.xyz;
    hi -= globals.cam_pos.xyz;
    vec3 away = max(max(lo, -hi), vec3(0.0));
    mat4 vp = globals.view_projection;
    vec3 look = normalize(vec3(vp[0][3], vp[1][3], vp[2][3]));

    velk_cut_decide(index, entry, chunks, globals.cam_pos.xyz, away, look);
}
)";

/**
 * Starts a frame of a kept cut (kCutHeaderWords): the other page table becomes current, and the
 * counts and lists of the last frame are emptied. One thread.
 */
inline constexpr string_view instance_cut_begin_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 1) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint cut_word;
} pc;

void main()
{
    uint cut = pc.cut_word;
    cull_words.data[cut + VELK_CUT_TABLE_PARITY] = 1u - cull_words.data[cut + VELK_CUT_TABLE_PARITY];
    cull_words.data[cut + VELK_CUT_TABLE_USED] = 0u;
    cull_words.data[cut + VELK_CUT_PAGES_TAKEN] = 0u;
    cull_words.data[cut + VELK_CUT_KEPT_PAGES] = 0u;
    cull_words.data[cut + VELK_CUT_PAGES_DEMAND] = 0u;
    // A new frame of the takeover table: what it held is gone.
    cull_words.data[cut + VELK_CUT_TAKEOVER_FRAME] += 1u;
    cull_words.data[cut + VELK_CUT_LAST_LIGHT_ONLY_RECORDS] = cull_words.data[cut + VELK_CUT_LIGHT_ONLY_RECORDS];
    cull_words.data[cut + VELK_CUT_LIGHT_ONLY_RECORDS] = 0u;
    cull_words.data[cut + VELK_CUT_LAST_REWRITTEN] = cull_words.data[cut + VELK_CUT_REWRITTEN];
    cull_words.data[cut + VELK_CUT_LAST_TOOK_OVER] = cull_words.data[cut + VELK_CUT_TOOK_OVER];
    cull_words.data[cut + VELK_CUT_REWRITTEN] = 0u;
    cull_words.data[cut + VELK_CUT_TOOK_OVER] = 0u;
    cull_words.data[cut + VELK_CUT_LAST_LONGEST_WALK] = cull_words.data[cut + VELK_CUT_LONGEST_WALK];
    cull_words.data[cut + VELK_CUT_LAST_MOST_CHOSEN] = cull_words.data[cut + VELK_CUT_MOST_CHOSEN];
    cull_words.data[cut + VELK_CUT_LONGEST_WALK] = 0u;
    cull_words.data[cut + VELK_CUT_MOST_CHOSEN] = 0u;
    cull_words.data[cut + VELK_CUT_CHOOSE_GROUPS] = 0u;
    cull_words.data[cut + VELK_CUT_VISIBLE_GROUPS] = 0u;
}
)";

/**
 * Finds the free pages of a kept cut's pool once the culling is done: with `phase` 0, a thread
 * per entry of the current page table marks its page (and the first thread judges whether the
 * pool is too small, VELK_CUT_PAGES_WANTED, then empties the free stack and the drawing counts);
 * with 1, a thread per 32 pages puts every page no table names on
 * the free stack, its header saying so, and the group counts of the passes over the pool's
 * records (VELK_CUT_DRAW_GROUPS) go up to the last page in use, then clears the marks. The pages
 * of partitions that chose again are free from here, for the next frame's choosing.
 */
inline constexpr string_view instance_cut_pages_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint cut_word;
    uint phase;
} pc;
)" VELK_CUT_GLSL R"(
void main()
{
    uint cut = pc.cut_word;
    uint index = gl_GlobalInvocationID.x;
    uint pages = cull_words.data[cut + VELK_CUT_PAGES];
    uint marks = cull_words.data[cut + VELK_CUT_PAGE_MARKS];
    if (pc.phase == 0u) {
        if (index == 0u) {
            // Whether the pool is too small, once every partition choosing again has taken its
            // pages or failed to: when the pages kept and asked for do not fit it, or when none
            // could be placed while some asked, with free pages fewer than even the smallest ask.
            // A shortage that partitions keeping their old records for a frame drain is neither.
            uint kept = cull_words.data[cut + VELK_CUT_KEPT_PAGES];
            uint demand = cull_words.data[cut + VELK_CUT_PAGES_DEMAND];
            uint free_now = min(cull_words.data[cut + VELK_CUT_FREE_PAGES], pages);
            // The pages asked for and not got, which the CPU watches every frame while there are
            // any, so that a pool too small grows soon.
            uint taken = cull_words.data[cut + VELK_CUT_PAGES_TAKEN];
            cull_words.data[cut + VELK_CUT_SHORT_PAGES] = demand > taken ? demand - taken : 0u;
            if (kept + demand > pages) {
                atomicMax(cull_words.data[cut + VELK_CUT_PAGES_WANTED], kept + demand);
            } else if (demand != 0u && cull_words.data[cut + VELK_CUT_PAGES_TAKEN] == 0u) {
                // Every ask was more than the free pages, so this is more than the pool.
                atomicMax(cull_words.data[cut + VELK_CUT_PAGES_WANTED], pages - free_now + demand);
            }
            cull_words.data[cut + VELK_CUT_FREE_PAGES] = 0u;
            cull_words.data[cut + VELK_CUT_DRAW_GROUPS] = 0u;
        }
        if (index >= min(cull_words.data[cut + VELK_CUT_TABLE_USED], pages)) return;
        uint page = cull_words.data[velk_cut_table(cut, true) + index];
        if (page < pages) atomicOr(cull_words.data[marks + page / 32u], 1u << (page % 32u));
        return;
    }
    if (index * 32u >= pages) return;
    uint used = cull_words.data[marks + index];
    uint free_pages = 0u;
    uint last = 0u;
    for (uint b = 0u; b < 32u && index * 32u + b < pages; ++b) {
        if ((used & (1u << b)) == 0u) ++free_pages;
        else last = index * 32u + b + 1u;
    }
    if (last != 0u) atomicMax(cull_words.data[cut + VELK_CUT_DRAW_GROUPS], last);
    uint at = cull_words.data[cut + VELK_CUT_STACK_WORD] + atomicAdd(cull_words.data[cut + VELK_CUT_FREE_PAGES], free_pages);
    uint headers = cull_words.data[cut + VELK_CUT_PAGE_HEADERS];
    for (uint b = 0u; b < 32u && index * 32u + b < pages; ++b) {
        if ((used & (1u << b)) != 0u) continue;
        uint page = index * 32u + b;
        cull_words.data[at++] = page;
        cull_words.data[headers + page * VELK_CUT_PAGE_WORDS + VELK_CUT_PAGE_OWNER] = 0xFFFFFFFFu;
    }
    cull_words.data[marks + index] = 0u;
}
)";

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
inline constexpr string_view instance_cut_choose_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct CullInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer CullInstances { CullInstance data[]; } cull_instances;
layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;
layout(set = 1, binding = 12, std430) readonly buffer LodNodes { uint data[]; } lod_nodes;

layout(push_constant) uniform PC {
    uint cut_word;
    uint instances_base;
    uint slot_count;
    uint table_word;
    uint globals_base;
    float lod_max_pixels;
    uint write;
    uint items_word;
    uint objects_word;
    uint next_word;
    uint head_word;
    uint object_slots;
    uint partition_table;  // the cut's partition table, whose entries name their quad trees
    float quad_pixels;     // kQuadPixels
    float light_pixels;    // the light's level: lod_max_pixels times kLightPixels over kLodElementPixels
    uint descriptors_word; // the instanced visuals' item descriptors (kItemDescriptorHeaderWords)
    uint descriptor_count;
} pc;

#define VELK_LOD_LIGHT
)" VELK_CUT_GLSL VELK_TRACE_GLSL R"(
uint g_count;
uint g_state;
uint g_table;     // the word of the partition's page table in the current one, while writing
bool g_writing;
uint g_at;        // its place in the partition's page table where this thread's next record goes, while writing
bool g_takeover;  // writing records that take over the light of the old ones that were the same
uint g_took;      // how many did
bool g_no_rays;   // the records being made are of a member in a patch for rays (VELK_CUT_RAY_QUAD)
// The partition's quad tree (kQuadNodeWords), or none, its leaves, and this member's leaf.
uint g_tree;
uint g_leaves;
uint g_position;
bool g_leaves_only;  // the leaves are the finest the partition has: its members are not instances
// What a member's roots go to (velk_cut_root): counted, put in the item list from g_root_at, or
// chosen at once by the thread.
#define VELK_ROOTS_COUNT 0u
#define VELK_ROOTS_PUT 1u
#define VELK_ROOTS_DIRECT 2u
uint g_root_mode;
uint g_roots;
uint g_root_at;

// The chunk's work, shared by its threads: items of 4 words, what a record names in its slot
// (its member's slot or plain item, or a quad's tree), an entry word, a node of the entry's tree
// or a record's cluster, and what it is (VELK_CHOOSE_KIND), with VELK_CHOOSE_NO_RAYS for a
// member in a patch for rays. Two lists, which expanding goes between.
#define VELK_CHOOSE_ITEMS 256u
#define VELK_CHOOSE_NODE 0u
#define VELK_CHOOSE_RECORD 1u
#define VELK_CHOOSE_KIND 1u
#define VELK_CHOOSE_NO_RAYS 2u
shared uint s_items[2u * VELK_CHOOSE_ITEMS * 4u];
shared uint s_item_counts[VELK_CHOOSE_ITEMS];
shared uint s_counts[64];
shared uint s_total;
shared uint s_nodes;

uint velk_takeover_hash(uint entry, uint cluster, uint slot)
{
    uint h = (entry * 0x9e3779b1u) ^ (cluster * 0x85ebca77u) ^ (slot * 0xc2b2ae3du);
    h ^= h >> 16u; h *= 0x7feb352du;
    h ^= h >> 15u; h *= 0x846ca68bu;
    h ^= h >> 16u;
    return h;
}

// The most slots a lookup in the takeover table tries.
#define VELK_TAKEOVER_PROBES 64u

// Lists the old record at word `was` in the takeover table (kCutHeaderWords 30), for the
// records chosen again to find by what they are.
void velk_takeover_put(uint cut, uint was)
{
    uint table = cull_words.data[cut + VELK_CUT_TAKEOVER_WORD];
    uint mask = cull_words.data[cut + VELK_CUT_TAKEOVER_SLOTS] - 1u;
    uint stamp = cull_words.data[cut + VELK_CUT_TAKEOVER_FRAME];
    uint h = velk_takeover_hash(cull_words.data[was + VELK_RECORD_ENTRY], cull_words.data[was + VELK_RECORD_CLUSTER],
                                cull_words.data[was + VELK_RECORD_SLOT]);
    for (uint p = 0u; p < VELK_TAKEOVER_PROBES; ++p) {
        uint s = table + ((h + p) & mask) * VELK_TAKEOVER_END;
        uint seen = cull_words.data[s + VELK_TAKEOVER_FRAME];
        if (seen == stamp) continue;
        if (atomicCompSwap(cull_words.data[s + VELK_TAKEOVER_FRAME], seen, stamp) == seen) {
            cull_words.data[s + VELK_TAKEOVER_RECORD] = was;
            return;
        }
    }
}

// The word of the old record that is (entry, cluster, slot), or 0xFFFFFFFF.
uint velk_takeover_find(uint cut, uint entry, uint cluster, uint slot)
{
    uint table = cull_words.data[cut + VELK_CUT_TAKEOVER_WORD];
    uint mask = cull_words.data[cut + VELK_CUT_TAKEOVER_SLOTS] - 1u;
    uint stamp = cull_words.data[cut + VELK_CUT_TAKEOVER_FRAME];
    uint h = velk_takeover_hash(entry, cluster, slot);
    for (uint p = 0u; p < VELK_TAKEOVER_PROBES; ++p) {
        uint s = table + ((h + p) & mask) * VELK_TAKEOVER_END;
        if (cull_words.data[s + VELK_TAKEOVER_FRAME] != stamp) return 0xFFFFFFFFu;
        uint was = cull_words.data[s + VELK_TAKEOVER_RECORD];
        if (cull_words.data[was + VELK_RECORD_ENTRY] == entry && cull_words.data[was + VELK_RECORD_CLUSTER] == cluster
            && cull_words.data[was + VELK_RECORD_SLOT] == slot) {
            return was;
        }
    }
    return 0xFFFFFFFFu;
}

// Counts a record, or writes it at this thread's next place, with the light of the old record
// that was the same.
void velk_cut_record(uint entry, uint cluster, uint slot)
{
    if (g_writing) {
        uint at = g_at++;
        // Never past the pages the count gave the partition.
        if (at >= cull_words.data[g_state + VELK_PART_COUNT]) return;
        uint place = velk_cut_place(g_table, at);
        uint record = velk_cut_record_word(pc.cut_word, place);
        cull_words.data[record + VELK_RECORD_ENTRY] = entry;
        cull_words.data[record + VELK_RECORD_CLUSTER] = cluster;
        cull_words.data[record + VELK_RECORD_SLOT] = slot;
        // Whether rays leave it out, for the tree pass.
        uint mark = cull_words.data[pc.cut_word + VELK_CUT_RAY_MARKS_WORD] + (place >> 5u);
        uint bit = 1u << (place & 31u);
        if (g_no_rays) atomicOr(cull_words.data[mark], bit);
        else atomicAnd(cull_words.data[mark], ~bit);
        uint light = 0xFFFFFFFFu;
        uint radiance = 0xFFFFFFFFu;
        uint radiance_dir = 0u;
        if (g_takeover) {
            uint was = velk_takeover_find(pc.cut_word, entry, cluster, slot);
            if (was != 0xFFFFFFFFu) {
                light = cull_words.data[was + VELK_RECORD_LIGHT];
                radiance = cull_words.data[was + VELK_RECORD_RADIANCE];
                radiance_dir = cull_words.data[was + VELK_RECORD_RADIANCE_DIR];
                ++g_took;
            }
        }
        cull_words.data[record + VELK_RECORD_LIGHT] = light;
        cull_words.data[record + VELK_RECORD_RADIANCE] = radiance;
        cull_words.data[record + VELK_RECORD_RADIANCE_DIR] = radiance_dir;
    }
    ++g_count;
}

void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     bool deforming)
{
    // Traced: written, or left out past the count.
    GlobalData g = velk_globals.data[pc.globals_base];
    if (g_writing && g.trace != VELK_TRACE_NONE) {
        uint cluster = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + c * VELK_LOD_CLUSTER_WORDS;
        uint own = cluster + VELK_LOD_CLUSTER_OWN_SPHERE;
        vec3 center = mat3(world) * vec3(uintBitsToFloat(cull_words.data[own]), uintBitsToFloat(cull_words.data[own + 1u]),
                                         uintBitsToFloat(cull_words.data[own + 2u]))
                    + (world[3].xyz - g.cam_pos.xyz);
        float radius = uintBitsToFloat(cull_words.data[own + 3u]) * scale;
        if (velk_trace_sphere(g, center, radius)) {
            velk_trace_note(g, VELK_TRACE_CHOOSE, g_at < cull_words.data[g_state + VELK_PART_COUNT] ? 0u : 1u, entry, c, slot);
        }
    }
    velk_cut_record(entry, c, slot);
}

// A cluster at the light's level over drawn ones finer than it: a record for light only.
void velk_lod_light_chosen(uint entry, uint c, uint slot)
{
    velk_cut_record(entry | VELK_CUT_LIGHT_ONLY, c, slot);
}

// The trace of the walk (kTraceWalk): what the walk of entry g_walk_entry, for the partition
// at g_state, made of cluster `c` (its words at `clusters`), when the traced pixel's ray passes
// its own sphere.
#define VELK_TRACE_WALK
uint g_walk_entry;
void velk_trace_walk(uint clusters, uint c, mat4 world, float scale, uint what)
{
    GlobalData g = velk_globals.data[pc.globals_base];
    if (g.trace == VELK_TRACE_NONE) return;
    // The entry traced in every pass, its items' expansion too; else only while writing.
    bool named = cull_words.data[g.trace + 4u] == g_walk_entry;
    if (!g_writing && !named) return;
    uint own = clusters + c * VELK_LOD_CLUSTER_WORDS + VELK_LOD_CLUSTER_OWN_SPHERE;
    vec3 center = mat3(world) * vec3(uintBitsToFloat(cull_words.data[own]), uintBitsToFloat(cull_words.data[own + 1u]),
                                     uintBitsToFloat(cull_words.data[own + 2u]))
                + (world[3].xyz - g.cam_pos.xyz);
    // On the ray, or of the entry traced wherever it is.
    if (cull_words.data[g.trace + 4u] != g_walk_entry
        && !velk_trace_sphere(g, center, uintBitsToFloat(cull_words.data[own + 3u]) * scale)) return;
    velk_trace_note(g, 7u, what, g_walk_entry, c, g_state);
}

// The numbers a decision of the walk of the entry traced went by (kTraceNumbers): `what` (a
// node's reference or a cluster, and which number), the number as stored and what it is on
// screen from the partition's viewpoint, in pixels.
void velk_trace_numbers(uint what, float stored, float pixels)
{
    GlobalData g = velk_globals.data[pc.globals_base];
    if (g.trace == VELK_TRACE_NONE || cull_words.data[g.trace + 4u] != g_walk_entry) return;
    velk_trace_note(g, 10u, 0u, what, floatBitsToUint(stored), floatBitsToUint(pixels));
}

// The child `ref` of the node whose child words start at `child`, in the tree of the entry
// traced: its node's index and which child it is.
void velk_trace_shape(uint ref, uint child)
{
    GlobalData g = velk_globals.data[pc.globals_base];
    if (g.trace == VELK_TRACE_NONE || cull_words.data[g.trace + 4u] != g_walk_entry) return;
    uint offset = child - cull_words.data[g_walk_entry + VELK_CLUSTER_ENTRY_NODES];
    velk_trace_note(g, 11u, 0u, ref, offset / VELK_LOD_NODE_STRIDE, (offset % VELK_LOD_NODE_STRIDE) / 8u);
}

// Likewise a node of the tree, its sphere at `at` (world space) of `reach`, skipped with all
// under it (`what`), for which `node` is its reference.
void velk_trace_node(vec3 at, float reach, uint node, uint what)
{
    GlobalData g = velk_globals.data[pc.globals_base];
    if (g.trace == VELK_TRACE_NONE) return;
    bool named = cull_words.data[g.trace + 4u] == g_walk_entry;
    if (!g_writing && !named) return;
    if (!named && !velk_trace_sphere(g, at - g.cam_pos.xyz, reach)) return;
    velk_trace_note(g, 7u, what, g_walk_entry, node | 0x40000000u, g_state | (g_writing ? 0u : 0x80000000u));
}
)" VELK_LOD_SELECT_GLSL R"(
// The instance a record's `named` is of: its item's.
uint velk_cut_named_instance(uint named)
{
    return cull_words.data[pc.items_word + named * VELK_PLAIN_ITEM_WORDS];
}

float velk_cut_scale(mat4 world)
{
    return sqrt(max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)), dot(world[2].xyz, world[2].xyz)));
}

// Walks the tree of `entry` from `node` down for `named`, choosing what it holds.
void velk_cut_walk(uint named, uint entry, uint node, vec3 viewpoint)
{
    CullInstance ci = cull_instances.data[velk_cut_named_instance(named)];
    mat4 world = velk_sized_world(ci.world_matrix, ci.offset.xyz, ci.size.xyz);
    GlobalData globals = velk_globals.data[pc.globals_base];
    g_walk_entry = entry;
    velk_lod_walk(entry, node, named, world, velk_cut_scale(world), viewpoint, viewpoint, globals.view_projection,
                  globals.viewport.y, pc.lod_max_pixels);
}

// A root of a member's choice: a record, or the root of one of its trees. Counted, put in the
// item list, or chosen at once, as g_root_mode says.
void velk_cut_root(uint kind, uint named, uint entry, uint ref, vec3 viewpoint)
{
    if (g_root_mode == VELK_ROOTS_COUNT) {
        ++g_roots;
    } else if (g_root_mode == VELK_ROOTS_PUT) {
        uint at = g_root_at++;
        if (at >= VELK_CHOOSE_ITEMS) return;
        s_items[at * 4u] = named;
        s_items[at * 4u + 1u] = entry;
        s_items[at * 4u + 2u] = ref;
        s_items[at * 4u + 3u] = kind | (g_no_rays ? VELK_CHOOSE_NO_RAYS : 0u);
    } else if (kind == VELK_CHOOSE_RECORD) {
        // Traced: a root chosen as it is, of the entry traced.
        GlobalData g = velk_globals.data[pc.globals_base];
        if (g_writing && g.trace != VELK_TRACE_NONE && cull_words.data[g.trace + 4u] == entry) {
            velk_trace_note(g, 7u, 11u, entry, ref, g_state);
        }
        velk_cut_record(entry, ref, named);
    } else {
        velk_cut_walk(named, entry, ref, viewpoint);
    }
}

// The items a node item at word `w` of s_items becomes: each of its children that is not
// skipped, in order, a node to walk or a record. Counted, or with `put` written from item `to`.
uint velk_cut_expand(uint w, vec3 viewpoint, bool put, uint to)
{
    uint named = s_items[w];
    uint entry = s_items[w + 1u];
    uint node = s_items[w + 2u];
    g_walk_entry = entry & ~VELK_CUT_LIGHT_ONLY;
    CullInstance ci = cull_instances.data[velk_cut_named_instance(named)];
    mat4 world = velk_sized_world(ci.world_matrix, ci.offset.xyz, ci.size.xyz);
    float scale = velk_cut_scale(world);
    GlobalData globals = velk_globals.data[pc.globals_base];
    mat4 vp = globals.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * globals.viewport.y * 0.5;
    uint clusters = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE];
    uint at = cull_words.data[entry + VELK_CLUSTER_ENTRY_NODES] + node * VELK_LOD_NODE_STRIDE;
    uint no_rays = s_items[w + 3u] & VELK_CHOOSE_NO_RAYS;
    if (!put) ++g_visited;
    uint n = 0u;
    for (uint k = 0u; k < 2u; ++k) {
        uint c;
        uint what = velk_lod_child(at + k * 8u, clusters, world, scale, viewpoint, vp, pixels_per_unit, pc.lod_max_pixels, c);
        if (what == VELK_LOD_SKIP) continue;
        if (put) {
            uint o = (to + n) * 4u;
            s_items[o] = named;
            s_items[o + 1u] = what == VELK_LOD_LIGHT_ONLY ? entry | VELK_CUT_LIGHT_ONLY : entry;
            s_items[o + 2u] = c;
            s_items[o + 3u] = (what == VELK_LOD_DESCEND ? VELK_CHOOSE_NODE : VELK_CHOOSE_RECORD) | no_rays;
        }
        ++n;
    }
    return n;
}

// The roots of an instance's choice; `named` is what its records name: its item.
void velk_cut_choose(uint named, vec3 viewpoint, uint table)
{
    uint first_entry = cull_words.data[table + VELK_TABLE_FIRST_ENTRY];
    uint entries = cull_words.data[table + VELK_TABLE_ENTRY_COUNT];
    if (entries == 0u) {
        velk_cut_root(VELK_CHOOSE_RECORD, named, 0xFFFFFFFFu, table, viewpoint);
        return;
    }
    for (uint e = 0u; e < entries; ++e) {
        velk_cut_root(VELK_CHOOSE_NODE, named, first_entry + e * VELK_CLUSTER_ENTRY_WORDS, 0u, viewpoint);
    }
}

// Chooses for one instance; `named` is what its records name: its item. Nothing for an instance
// without a place in its raster region.
void velk_cut_instance(uint named, uint instance, uint table_word, vec3 viewpoint)
{
    if (instance == 0xFFFFFFFFu) return;
    mat4 world = cull_instances.data[instance].world_matrix;
    if (world[3][3] == 0.0) return;
    // An item for rays only (no table): the whole object.
    if (table_word == 0xFFFFFFFFu) {
        velk_cut_root(VELK_CHOOSE_RECORD, named, 0xFFFFFFFFu, 0xFFFFFFFFu, viewpoint);
        return;
    }
    velk_cut_choose(named, viewpoint, table_word + cull_instances.data[instance].params.y * VELK_TABLE_END);
}

// The topmost node of the quad tree over this member that is small enough on screen from
// `viewpoint`, or none when its own quad is not. A node's size is its quad's longer side, seen
// at the nearest point of its sphere. A parent's quad keeps its children's area with the larger
// child's aspect and its sphere holds theirs, so sizes only grow going up, and the first too
// large ends it.
uint velk_quad_over(vec3 viewpoint, float pixels_per_unit)
{
    uint node = g_leaves - 1u + g_position;
    uint chosen = 0xFFFFFFFFu;
    for (uint step = 0u; step < 32u; ++step) {
        uint w = g_tree + node * VELK_QUAD_NODE_WORDS;
        float radius = uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_RADIUS]);
        if (radius < 0.0) break;
        vec3 center = vec3(uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER]),
                           uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER + 1u]),
                           uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER + 2u]));
        vec3 u = vec3(uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_U]), uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_U + 1u]),
                      uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_U + 2u]));
        vec3 v = vec3(uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_V]), uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_V + 1u]),
                      uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_V + 2u]));
        float side = 2.0 * max(length(u), length(v));
        float near = length(center - viewpoint) - radius;
        // Out of view, no finer than the light's level.
        bool out_of_view = velk_lod_out_of_view(center, radius, velk_globals.data[pc.globals_base].view_projection);
        g_out_of_view = g_out_of_view || out_of_view;
        float limit = out_of_view ? max(pc.quad_pixels, pc.light_pixels) : pc.quad_pixels;
        if (near <= 0.0 || side * pixels_per_unit > limit * near) break;
        chosen = node;
        if (node == 0u) break;
        node = (node - 1u) / 2u;
    }
    return chosen;
}

// The patch for rays over this member: the topmost node of the quad tree over it no wider on
// screen than the light spacing from `viewpoint`, measured as velk_quad_over measures, or none
// when its own quad is wider.
uint velk_quad_patch(vec3 viewpoint, float pixels_per_unit)
{
    float spacing = pc.light_pixels * pc.quad_pixels / pc.lod_max_pixels;
    uint node = g_leaves - 1u + g_position;
    uint chosen = 0xFFFFFFFFu;
    for (uint step = 0u; step < 32u; ++step) {
        uint w = g_tree + node * VELK_QUAD_NODE_WORDS;
        float radius = uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_RADIUS]);
        if (radius < 0.0) break;
        vec3 center = vec3(uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER]),
                           uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER + 1u]),
                           uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER + 2u]));
        vec3 u = vec3(uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_U]), uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_U + 1u]),
                      uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_U + 2u]));
        vec3 v = vec3(uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_V]), uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_V + 1u]),
                      uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_V + 2u]));
        float near = length(center - viewpoint) - radius;
        if (near <= 0.0 || 2.0 * max(length(u), length(v)) * pixels_per_unit > spacing * near) break;
        chosen = node;
        if (node == 0u) break;
        node = (node - 1u) / 2u;
    }
    return chosen;
}

// The leaf of the first member under `node`.
uint velk_quad_first(uint node)
{
    while (node < g_leaves - 1u) node = node * 2u + 1u;
    return node - (g_leaves - 1u);
}

)" R"(
// Chooses for a member of a chunk: an instance's slot, or an object.
void velk_cut_member(uint member, vec3 viewpoint)
{
    g_no_rays = false;
    // Under a quad small enough: the first member under it records the quad, the rest nothing.
    if (g_tree != 0xFFFFFFFFu) {
        GlobalData globals = velk_globals.data[pc.globals_base];
        mat4 vp = globals.view_projection;
        float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * globals.viewport.y * 0.5;
        // In a patch for rays: the first member under it records the patch, and rays leave out
        // the records of every member under it.
        uint patch_node = velk_quad_patch(viewpoint, pixels_per_unit);
        if (patch_node != 0xFFFFFFFFu) {
            if (velk_quad_first(patch_node) == g_position) {
                velk_cut_root(VELK_CHOOSE_RECORD, g_tree, VELK_CUT_RAY_QUAD, g_tree + patch_node * VELK_QUAD_NODE_WORDS, viewpoint);
            }
            g_no_rays = true;
        }
        uint quad = velk_quad_over(viewpoint, pixels_per_unit);
        // Where nothing is small enough, a partition without its instances still has its leaf.
        if (quad == 0xFFFFFFFFu && g_leaves_only) quad = g_leaves - 1u + g_position;
        if (quad != 0xFFFFFFFFu) {
            if (velk_quad_first(quad) == g_position
                && uintBitsToFloat(cull_words.data[g_tree + quad * VELK_QUAD_NODE_WORDS + VELK_QUAD_NODE_RADIUS]) >= 0.0) {
                velk_cut_root(VELK_CHOOSE_RECORD, g_tree, VELK_CUT_QUAD, g_tree + quad * VELK_QUAD_NODE_WORDS, viewpoint);
            }
            return;
        }
    }
    if (g_leaves_only) return;
    // An instanced visual's object: its one item, by its id, through its visual's descriptor.
    uint at = pc.descriptors_word;
    for (uint k = 0u; k < pc.descriptor_count && k < 4096u; ++k) {
        uint base = cull_words.data[at + VELK_ITEM_DESCRIPTOR_OBJECT_BASE];
        if (base != 0xFFFFFFFFu && member >= base && member - base < cull_words.data[at + VELK_ITEM_DESCRIPTOR_SLOTS]) {
            uint item = cull_words.data[at + VELK_ITEM_DESCRIPTOR_FIRST] + member - base;
            uint w = pc.items_word + item * VELK_PLAIN_ITEM_WORDS;
            velk_cut_instance(item, cull_words.data[w + VELK_PLAIN_ITEM_INSTANCE], cull_words.data[w + VELK_PLAIN_ITEM_TABLE], viewpoint);
            return;
        }
        at += VELK_ITEM_DESCRIPTOR_HEADER_WORDS + min(cull_words.data[at + VELK_ITEM_DESCRIPTOR_MESHES], 4096u) * VELK_ITEM_MESH_WORDS;
    }
    // A plain object: each of its items. An object has a few at most; the bound only keeps a
    // broken chain from running on.
    uint object_slot = member & 0x3FFFFFFu;
    uint item = object_slot < pc.object_slots ? cull_words.data[pc.head_word + object_slot] : 0xFFFFFFFFu;
    for (uint n = 0u; n < 64u && item != 0xFFFFFFFFu; ++n) {
        if (cull_words.data[pc.objects_word + item] == member) {
            uint at = pc.items_word + item * VELK_PLAIN_ITEM_WORDS;
            velk_cut_instance(item, cull_words.data[at + VELK_PLAIN_ITEM_INSTANCE], cull_words.data[at + VELK_PLAIN_ITEM_TABLE], viewpoint);
        }
        item = cull_words.data[pc.next_word + item];
    }
}

void main()
{
    uint cut = pc.cut_word;
    uint chunk_index = gl_WorkGroupID.x;
    uint chunk = cull_words.data[cut + VELK_CUT_CHOOSE_WORD] + chunk_index * VELK_CHUNK_END;
    uint lane = gl_LocalInvocationIndex;
    uint member = cull_words.data[cull_words.data[chunk + VELK_CHUNK_FIRST_SLOT] + lane];
    uint state = velk_cut_state(cut, cull_words.data[chunk + VELK_CHUNK_PARTITION]);
    uint chunk_word = cull_words.data[cut + VELK_CUT_CHUNK_COUNTS_WORD] + chunk_index;
    // A work group is one chunk of one partition, so this is the same for all its threads. A
    // partition that found no room chooses on the next frame.
    bool choosing = pc.write == 0u || cull_words.data[state + VELK_PART_CHOOSING] != 0u;
    vec3 viewpoint = vec3(uintBitsToFloat(cull_words.data[state + VELK_PART_FROM]), uintBitsToFloat(cull_words.data[state + VELK_PART_FROM + 1u]),
                          uintBitsToFloat(cull_words.data[state + VELK_PART_FROM + 2u]));

    g_count = 0u;
    g_state = state;
    g_table = velk_cut_table(cut, true) + cull_words.data[state + VELK_PART_TABLE_FIRST];
    g_writing = false;
    g_no_rays = false;
    g_out_of_view = false;
    g_visited = 0u;
    g_view_cam = velk_globals.data[pc.globals_base].cam_pos.xyz;
    // Where this member is in its partition's list, which its quad tree's leaves follow.
    g_tree = 0xFFFFFFFFu;
    g_leaves = 0u;
    g_position = 0u;
    g_leaves_only = false;
    if (pc.partition_table != 0xFFFFFFFFu) {
        uint entry = pc.partition_table + cull_words.data[chunk + VELK_CHUNK_PARTITION] * VELK_CULL_ENTRY_WORDS;
        uint leaves = cull_words.data[entry + VELK_PARTITION_ENTRY_TREE_LEAVES] & ~VELK_QUAD_LEAVES_ONLY;
        uint position = cull_words.data[chunk + VELK_CHUNK_FIRST_SLOT] - cull_words.data[entry + VELK_PARTITION_ENTRY_FIRST_CHUNK] + lane;
        if (cull_words.data[entry + VELK_PARTITION_ENTRY_QUAD_TREE] != 0xFFFFFFFFu && position < leaves) {
            g_tree = cull_words.data[entry + VELK_PARTITION_ENTRY_QUAD_TREE];
            g_leaves = leaves;
            g_position = position;
            g_leaves_only = (cull_words.data[entry + VELK_PARTITION_ENTRY_TREE_LEAVES] & VELK_QUAD_LEAVES_ONLY) != 0u;
        }
    }
    // The same for all its threads: a work group is one chunk of one partition.
    if (!choosing) return;

    // The members' roots, in member order: counted, then put in the item list. A member whose
    // roots do not fit after the ones before it chooses alone, after the items.
    g_root_mode = VELK_ROOTS_COUNT;
    g_roots = 0u;
    if (member != 0xFFFFFFFFu) velk_cut_member(member, viewpoint);
    s_counts[lane] = g_roots;
    barrier();
    uint before = 0u;
    for (uint i = 0u; i < lane; ++i) before += s_counts[i];
    bool alone = g_roots != 0u && before + g_roots > VELK_CHOOSE_ITEMS;
    uint items = 0u;
    for (uint i = 0u; i < 64u && items + s_counts[i] <= VELK_CHOOSE_ITEMS; ++i) items += s_counts[i];
    if (g_roots != 0u && !alone) {
        g_root_mode = VELK_ROOTS_PUT;
        g_root_at = before;
        velk_cut_member(member, viewpoint);
    }
    barrier();

    // Expanded together, a level at a time, each node item into its children in order, until
    // there are enough nodes for each thread to walk a few small trees: the longest walk is then
    // one of those, not a whole large mesh's tree. Both lists stay in member order.
    uint from_list = 0u;
    for (uint round = 0u; round < 16u; ++round) {
        if (lane == 0u) s_nodes = 0u;
        barrier();
        for (uint i = lane; i < items; i += 64u) {
            if ((s_items[(from_list * VELK_CHOOSE_ITEMS + i) * 4u + 3u] & VELK_CHOOSE_KIND) == VELK_CHOOSE_NODE) atomicAdd(s_nodes, 1u);
        }
        barrier();
        uint nodes = s_nodes;
        barrier();
        if (nodes == 0u || nodes >= 128u || items * 2u > VELK_CHOOSE_ITEMS) break;
        uint to_list = 1u - from_list;
        for (uint i = lane; i < items; i += 64u) {
            uint w = (from_list * VELK_CHOOSE_ITEMS + i) * 4u;
            s_item_counts[i] = (s_items[w + 3u] & VELK_CHOOSE_KIND) == VELK_CHOOSE_NODE ? velk_cut_expand(w, viewpoint, false, 0u) : 1u;
        }
        barrier();
        if (lane == 0u) {
            uint run = 0u;
            for (uint i = 0u; i < items; ++i) {
                uint n = s_item_counts[i];
                s_item_counts[i] = run;
                run += n;
            }
            s_total = run;
        }
        barrier();
        for (uint i = lane; i < items; i += 64u) {
            uint w = (from_list * VELK_CHOOSE_ITEMS + i) * 4u;
            uint to = to_list * VELK_CHOOSE_ITEMS + s_item_counts[i];
            if ((s_items[w + 3u] & VELK_CHOOSE_KIND) == VELK_CHOOSE_NODE) {
                velk_cut_expand(w, viewpoint, true, to);
            } else {
                for (uint k = 0u; k < 4u; ++k) s_items[to * 4u + k] = s_items[w + k];
            }
        }
        items = s_total;
        from_list = to_list;
        barrier();
    }

    // Each item's records, counted: a record is one, a node's tree is walked.
    uint most = 0u;
    for (uint i = lane; i < items; i += 64u) {
        uint w = (from_list * VELK_CHOOSE_ITEMS + i) * 4u;
        g_count = 0u;
        g_no_rays = (s_items[w + 3u] & VELK_CHOOSE_NO_RAYS) != 0u;
        if ((s_items[w + 3u] & VELK_CHOOSE_KIND) == VELK_CHOOSE_RECORD) {
            velk_cut_record(s_items[w + 1u], s_items[w + 2u], s_items[w]);
        } else {
            velk_cut_walk(s_items[w], s_items[w + 1u], s_items[w + 2u], viewpoint);
        }
        s_item_counts[i] = g_count;
        most = max(most, g_count);
    }
    uint alone_count = 0u;
    if (alone) {
        g_root_mode = VELK_ROOTS_DIRECT;
        g_count = 0u;
        velk_cut_member(member, viewpoint);
        alone_count = g_count;
        most = max(most, g_count);
    }
    barrier();
    // Where each item's records start, in order, then those of the members alone.
    if (lane == 0u) {
        uint run = 0u;
        for (uint i = 0u; i < items; ++i) {
            uint n = s_item_counts[i];
            s_item_counts[i] = run;
            run += n;
        }
        s_total = run;
    }
    s_counts[lane] = alone_count;
    barrier();
    uint alone_at = s_total;
    for (uint i = 0u; i < lane; ++i) alone_at += s_counts[i];
    uint total = s_total;
    for (uint i = 0u; i < 64u; ++i) total += s_counts[i];
    // Some of it chosen coarse for being out of view: the partition chooses again on a turn.
    if (g_out_of_view) cull_words.data[state + VELK_PART_OUT_OF_VIEW] = 1u;

    if (pc.write == 0u) {
        // The longest walk and the most records of one walk, for the log.
        if (g_visited != 0u) atomicMax(cull_words.data[cut + VELK_CUT_LONGEST_WALK], g_visited);
        if (most != 0u) atomicMax(cull_words.data[cut + VELK_CUT_MOST_CHOSEN], most);
        if (lane == 0u) {
            cull_words.data[chunk_word] = total;
            if (total != 0u) atomicAdd(cull_words.data[state + VELK_PART_COUNT], total);
        }
        // This chunk's share of the partition's old records goes in the takeover table, for its
        // records written next to find: on its old pages, by its old page table in the last one
        // (VELK_PART_LAST_TABLE_FIRST, OLD_COUNT).
        uint old = cull_words.data[state + VELK_PART_OLD_COUNT];
        if (old != 0u) {
            uint chunks = cull_words.data[state + VELK_PART_CHUNKS];
            uint per = (old + chunks - 1u) / chunks;
            uint from = (chunk_index - cull_words.data[state + VELK_PART_CHUNK_START]) * per;
            uint to = min(from + per, old);
            uint old_table = velk_cut_table(cut, false) + cull_words.data[state + VELK_PART_LAST_TABLE_FIRST];
            for (uint i = from + lane; i < to; i += 64u) velk_takeover_put(cut, velk_cut_record_word(cut, velk_cut_place(old_table, i)));
        }
        return;
    }

    // Written: each item from its place in the chunk's range, then the members alone. Each record
    // takes over the light of the old record that was the same.
    uint first = cull_words.data[chunk_word];
    g_writing = true;
    g_takeover = cull_words.data[state + VELK_PART_OLD_COUNT] != 0u;
    g_took = 0u;
    g_count = 0u;
    for (uint i = lane; i < items; i += 64u) {
        uint w = (from_list * VELK_CHOOSE_ITEMS + i) * 4u;
        g_at = first + s_item_counts[i];
        g_no_rays = (s_items[w + 3u] & VELK_CHOOSE_NO_RAYS) != 0u;
        if ((s_items[w + 3u] & VELK_CHOOSE_KIND) == VELK_CHOOSE_RECORD) {
            velk_cut_record(s_items[w + 1u], s_items[w + 2u], s_items[w]);
        } else {
            velk_cut_walk(s_items[w], s_items[w + 1u], s_items[w + 2u], viewpoint);
        }
    }
    if (alone) {
        g_root_mode = VELK_ROOTS_DIRECT;
        g_at = first + alone_at;
        velk_cut_member(member, viewpoint);
    }
    if (g_count != 0u) atomicAdd(cull_words.data[cut + VELK_CUT_REWRITTEN], g_count);
    if (g_took != 0u) atomicAdd(cull_words.data[cut + VELK_CUT_TOOK_OVER], g_took);
}
)";

/**
 * Gives each partition that chose again this frame pages for its count, off the free stack, its
 * page table in the current one, and each of its chunks its first place in that table, in
 * order. One that finds too few free pages keeps its old records, pages and ray list for this
 * frame and chooses again on the next; one whose members changed (VELK_PART_SAME 0) has no
 * records for the frame instead, since its old ones may name what is gone. A thread per
 * partition.
 */
inline constexpr string_view instance_cut_place_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint cut_word;
} pc;
)" VELK_CUT_GLSL R"(
// Takes `count` pages off the free stack; the kth is velk_cut_taken(cut, base, k). False when
// there are not that many. The count of pages taken only ever moves forward, past a take that
// fits, so that no two takes get the same pages; a swap that fails means another take moved it.
bool velk_cut_take(uint cut, uint count, out uint base)
{
    uint free_pages = cull_words.data[cut + VELK_CUT_FREE_PAGES];
    uint taken = cull_words.data[cut + VELK_CUT_PAGES_TAKEN];
    base = 0u;
    for (uint tries = 0u; tries < 4096u; ++tries) {
        if (taken + count > free_pages) return false;
        uint seen = atomicCompSwap(cull_words.data[cut + VELK_CUT_PAGES_TAKEN], taken, taken + count);
        if (seen == taken) {
            base = taken;
            return true;
        }
        taken = seen;
    }
    return false;
}

uint velk_cut_taken(uint cut, uint base, uint k)
{
    return cull_words.data[cull_words.data[cut + VELK_CUT_STACK_WORD] + cull_words.data[cut + VELK_CUT_FREE_PAGES] - 1u - base - k];
}

void main()
{
    uint cut = pc.cut_word;
    uint index = gl_GlobalInvocationID.x;
    if (index >= cull_words.data[cut + VELK_CUT_PARTITIONS]) return;
    uint state = velk_cut_state(cut, index);
    if (cull_words.data[state + VELK_PART_CHOOSING] == 0u) return;
    uint pool_pages = cull_words.data[cut + VELK_CUT_PAGES];
    uint count = cull_words.data[state + VELK_PART_COUNT];
    uint pages = velk_cut_pages(count);
    uint base = 0u;
    uint first = 0u;
    // What the frame asks of the pool, placed or not, for the sweep to judge whether it is too
    // small (instance_cut_pages_src).
    if (pages != 0u) atomicAdd(cull_words.data[cut + VELK_CUT_PAGES_DEMAND], pages);
    bool placed = pages == 0u || velk_cut_take(cut, pages, base);
    if (placed && pages != 0u) {
        first = atomicAdd(cull_words.data[cut + VELK_CUT_TABLE_USED], pages);
        // Never so, since a page is in one table at most; the pages taken go free at the sweep.
        if (first + pages > pool_pages) placed = false;
    }
    if (!placed) {
        cull_words.data[state + VELK_PART_CHOOSING] = 0u;
        uint old = cull_words.data[state + VELK_PART_OLD_COUNT];
        uint old_pages = velk_cut_pages(old);
        uint was = cull_words.data[state + VELK_PART_LAST_TABLE_FIRST];
        uint at = old != 0u && cull_words.data[state + VELK_PART_SAME] != 0u
                ? atomicAdd(cull_words.data[cut + VELK_CUT_TABLE_USED], old_pages) : pool_pages;
        if (at + old_pages <= pool_pages && was + old_pages <= pool_pages) {
            // Its old records as they were, and it chooses again on the next frame.
            uint from = velk_cut_table(cut, false) + was;
            uint to = velk_cut_table(cut, true) + at;
            for (uint p = 0u; p < old_pages; ++p) cull_words.data[to + p] = cull_words.data[from + p];
            cull_words.data[state + VELK_PART_TABLE_FIRST] = at;
            cull_words.data[state + VELK_PART_COUNT] = old;
            cull_words.data[state + VELK_PART_RAY_COUNT] = cull_words.data[state + VELK_PART_OLD_RAY_COUNT];
            cull_words.data[state + VELK_PART_RETRY] = 1u;
        } else {
            cull_words.data[state + VELK_PART_COUNT] = 0u;
            cull_words.data[state + VELK_PART_RAY_COUNT] = 0u;
        }
        cull_words.data[state + VELK_PART_OLD_COUNT] = 0u;
        return;
    }
    // Its pages in order, each page's header naming it.
    uint table = velk_cut_table(cut, true) + first;
    uint headers = cull_words.data[cut + VELK_CUT_PAGE_HEADERS];
    for (uint p = 0u; p < pages; ++p) {
        uint page = velk_cut_taken(cut, base, p);
        cull_words.data[table + p] = page;
        cull_words.data[headers + page * VELK_CUT_PAGE_WORDS + VELK_CUT_PAGE_OWNER] = index;
        cull_words.data[headers + page * VELK_CUT_PAGE_WORDS + VELK_CUT_PAGE_PLACE] = p;
    }
    cull_words.data[state + VELK_PART_TABLE_FIRST] = first;
    // Each of its chunks' first place in its page table, in the order of its chunks.
    uint counts = cull_words.data[cut + VELK_CUT_CHUNK_COUNTS_WORD] + cull_words.data[state + VELK_PART_CHUNK_START];
    uint next = 0u;
    for (uint c = 0u; c < cull_words.data[state + VELK_PART_CHUNKS]; ++c) {
        uint chunk_records = cull_words.data[counts + c];
        cull_words.data[counts + c] = next;
        next += chunk_records;
    }
}
)";

// Where a cut record is in the world, for the shader that declares cull_words and
// cull_instances: a box around it, empty for a slot with no instance. `instances_base` and
// `items_word` are as at instance_cut_choose_src; `pad` grows it.
#define VELK_CUT_BOX_GLSL R"(
void velk_cut_record_box(uint record, uint instances_base, uint items_word, float pad, out vec3 lo, out vec3 hi)
{
    lo = vec3(1e30);
    hi = vec3(-1e30);
    uint entry = cull_words.data[record + VELK_RECORD_ENTRY]
               & ~(velk_cut_light_only(cull_words.data[record + VELK_RECORD_ENTRY]) ? VELK_CUT_LIGHT_ONLY : 0u);
    uint cluster = cull_words.data[record + VELK_RECORD_CLUSTER];
    uint slot = cull_words.data[record + VELK_RECORD_SLOT];
    if (entry == VELK_CUT_RAY_QUAD) {
        // A patch: its node's sphere, which holds all its members, in the world already.
        vec3 c = vec3(uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_CENTER]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_CENTER + 1u]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_CENTER + 2u]));
        float r = max(uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_RADIUS]), 0.0) + pad;
        lo = c - vec3(r);
        hi = c + vec3(r);
        return;
    }
    if (entry == VELK_CUT_QUAD) {
        // A quad: around its corners, in the world already.
        vec3 c = vec3(uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_CENTER]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_CENTER + 1u]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_CENTER + 2u]));
        vec3 u = vec3(uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_U]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_U + 1u]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_U + 2u]));
        vec3 v = vec3(uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_V]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_V + 1u]),
                      uintBitsToFloat(cull_words.data[cluster + VELK_QUAD_NODE_V + 2u]));
        vec3 reach = abs(u) + abs(v) + vec3(pad);
        lo = c - reach;
        hi = c + reach;
        return;
    }
    uint instance = instances_base + slot;
    if (items_word != 0xFFFFFFFFu) {
        uint item = items_word + slot * VELK_PLAIN_ITEM_WORDS;
        instance = cull_words.data[item];
        // Grown by how far a motion function can move it, both ways.
        pad += 2.0 * uintBitsToFloat(cull_words.data[item + VELK_PLAIN_ITEM_MOTION]);
    }
    if (instance == 0xFFFFFFFFu) return;
    mat4 world = cull_instances.data[instance].world_matrix;
    if (world[3][3] == 0.0) return;
    if (entry == 0xFFFFFFFFu && cluster == 0xFFFFFFFFu) {
        // An item rays see that its own pass draws: its instance's box.
        vec3 offset = cull_instances.data[instance].offset.xyz;
        vec3 size = cull_instances.data[instance].size.xyz;
        for (uint k = 0u; k < 8u; ++k) {
            vec3 corner = offset + size * vec3(float(k & 1u), float((k >> 1u) & 1u), float((k >> 2u) & 1u));
            vec3 at = mat3(world) * corner + world[3].xyz;
            lo = min(lo, at);
            hi = max(hi, at);
        }
        lo -= vec3(pad);
        hi += vec3(pad);
        return;
    }
    // A cluster's own sphere, or a whole mesh's: its table's, in the mesh's own coordinates.
    world = velk_sized_world(world, cull_instances.data[instance].offset.xyz, cull_instances.data[instance].size.xyz);
    uint sphere = entry != 0xFFFFFFFFu ? cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + cluster * VELK_LOD_CLUSTER_WORDS + VELK_LOD_CLUSTER_OWN_SPHERE : cluster;
    float scale = sqrt(max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)),
                           dot(world[2].xyz, world[2].xyz)));
    vec3 center = mat3(world) * vec3(uintBitsToFloat(cull_words.data[sphere]), uintBitsToFloat(cull_words.data[sphere + 1u]),
                                     uintBitsToFloat(cull_words.data[sphere + 2u]))
                + world[3].xyz;
    float radius = uintBitsToFloat(cull_words.data[sphere + 3u]) * scale + pad;
    lo = center - radius;
    hi = center + radius;
}
)"

/**
 * Builds the tree over each partition's records that chose again this frame (kCutNodeWords):
 * its ray list sorted by where its records are (Morton order, the widest after them, those
 * without a box last and left out), then leaves of four records in that order, and the radix
 * tree over them (VELK_LBVH_GLSL). Past kRaySortMax records the list keeps its order, which is
 * near to near, and the tree over it is balanced. `pad` grows every box, for instances that a
 * motion function moves, by twice how far it can move one, so that the tree holds wherever
 * they are while it is kept. Also the partition's box around all it draws, whether it is
 * always visited, and its counts for the log (VELK_PART_BOX, ALWAYS, LIGHT_ONLY, QUADS), which
 * instance_cut_visible_src goes by. A work group per partition.
 */
inline constexpr string_view instance_cut_tree_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct CullInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer CullInstances { CullInstance data[]; } cull_instances;
layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint cut_word;
    uint instances_base;
    uint items_word;
    float pad;
    uint globals_base;   // the view's, for sizes on screen
    float light_pixels;  // the light's level, as at instance_cut_choose_src
} pc;
)" VELK_CUT_GLSL VELK_CUT_BOX_GLSL VELK_SORT_GLSL VELK_LBVH_GLSL R"(
shared uint s_counts[64];
// After the sort: the key of each leaf of four records, that of its first, and how many records
// have a box, which the sort puts first.
shared uint s_leaf_key[kSortMax / 4u];
shared uint s_bounded;
// The box around what the partition draws and its counts, each thread's share, then added up.
shared vec3 s_lo[64];
shared vec3 s_hi[64];
shared uint s_always[64];
shared uint s_light_only[64];
shared uint s_quads[64];

// Adds `record`'s share to the partition's box and counts: none for one for light only, a patch
// for rays or a cluster for rays only, which are not drawn; a deforming mesh's or an item drawn
// by its own pass makes the partition always visited.
void velk_draw_share(uint record, inout vec3 lo, inout vec3 hi, inout uint always, inout uint light_only, inout uint quads)
{
    uint entry = cull_words.data[record + VELK_RECORD_ENTRY];
    if (velk_cut_light_only(entry)) {
        ++light_only;
        return;
    }
    if (entry == VELK_CUT_RAY_QUAD) return;
    if (entry == VELK_CUT_QUAD) {
        ++quads;
    } else if (entry != 0xFFFFFFFFu) {
        uint flags = cull_words.data[entry + VELK_CLUSTER_ENTRY_FLAGS];
        if ((flags & VELK_LOD_DEFORMING) != 0u) always = 1u;
    } else if (pc.items_word != 0xFFFFFFFFu) {
        uint item = pc.items_word + cull_words.data[record + VELK_RECORD_SLOT] * VELK_PLAIN_ITEM_WORDS;
        if (cull_words.data[item + VELK_PLAIN_ITEM_TABLE] == 0xFFFFFFFFu) always = 1u;
    }
    vec3 rlo, rhi;
    velk_cut_record_box(record, pc.instances_base, pc.items_word, pc.pad, rlo, rhi);
    lo = min(lo, rlo);
    hi = max(hi, rhi);
}

// How wide on screen, from `from`, the error of the sphere at `word` (centre, radius, error) is.
float velk_ray_pixels(uint word, mat4 world, float scale, vec3 from, float pixels_per_unit)
{
    vec3 center = mat3(world) * vec3(uintBitsToFloat(cull_words.data[word]), uintBitsToFloat(cull_words.data[word + 1u]),
                                     uintBitsToFloat(cull_words.data[word + 2u])) + world[3].xyz - from;
    float away = length(center) - uintBitsToFloat(cull_words.data[word + 3u]) * scale;
    return uintBitsToFloat(cull_words.data[word + 4u]) * scale * pixels_per_unit / max(away, 1e-3);
}

// Whether rays trace `record`, the record at `place` of the pool: at the light's level, seen
// from `viewpoint` where its partition chose. A record for light only is, and a patch for rays;
// a record of a member in a patch is not, the patch standing for it; a cluster drawn finer than
// the light, whose parent is not coarser than the light, is not, since a record for light only
// covers it.
bool velk_ray_record(uint record, uint place, vec3 viewpoint, float pixels_per_unit)
{
    uint marks = cull_words.data[pc.cut_word + VELK_CUT_RAY_MARKS_WORD];
    if ((cull_words.data[marks + (place >> 5u)] & (1u << (place & 31u))) != 0u) return false;
    uint entry = cull_words.data[record + VELK_RECORD_ENTRY];
    if (entry >= VELK_CUT_RAY_QUAD || velk_cut_light_only(entry)) return true;
    uint slot = cull_words.data[record + VELK_RECORD_SLOT];
    uint instance = pc.instances_base + slot;
    if (pc.items_word != 0xFFFFFFFFu) instance = cull_words.data[pc.items_word + slot * VELK_PLAIN_ITEM_WORDS];
    if (instance == 0xFFFFFFFFu) return false;
    CullInstance ci = cull_instances.data[instance];
    mat4 world = velk_sized_world(ci.world_matrix, ci.offset.xyz, ci.size.xyz);
    float scale = sqrt(max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)), dot(world[2].xyz, world[2].xyz)));
    uint cluster = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + cull_words.data[record + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS;
    return velk_ray_pixels(cluster + VELK_LOD_CLUSTER_PARENT_SPHERE, world, scale, viewpoint, pixels_per_unit) > pc.light_pixels;
}

void velk_store_box(uint at, vec3 lo, vec3 hi)
{
    cull_words.data[at] = floatBitsToUint(lo.x);
    cull_words.data[at + 1u] = floatBitsToUint(lo.y);
    cull_words.data[at + 2u] = floatBitsToUint(lo.z);
    cull_words.data[at + 3u] = floatBitsToUint(hi.x);
    cull_words.data[at + 4u] = floatBitsToUint(hi.y);
    cull_words.data[at + 5u] = floatBitsToUint(hi.z);
}

vec3 velk_load_vec3(uint at)
{
    return vec3(uintBitsToFloat(cull_words.data[at]), uintBitsToFloat(cull_words.data[at + 1u]),
                uintBitsToFloat(cull_words.data[at + 2u]));
}

// Writes the ray list entry at word `at` for the record at pool place `place`: the place and
// the record's box, grown by the pad.
void velk_write_ray_entry(uint at, uint place)
{
    vec3 rlo, rhi;
    velk_cut_record_box(velk_cut_record_word(pc.cut_word, place), pc.instances_base, pc.items_word, pc.pad, rlo, rhi);
    cull_words.data[at + VELK_CUT_RAY_INDEX] = place;
    velk_store_box(at + VELK_CUT_RAY_MIN, rlo, rhi);
}

// The box of the ray list entry at word `at`, and its centre and the radius of the sphere round
// it, which the sorts go by; false for a record without a box.
bool velk_ray_entry_box(uint at, out vec3 lo, out vec3 hi, out vec3 centre, out float radius)
{
    lo = velk_load_vec3(at + VELK_CUT_RAY_MIN);
    hi = velk_load_vec3(at + VELK_CUT_RAY_MAX);
    centre = (lo + hi) * 0.5;
    radius = length(hi - lo) * 0.5;
    return lo.x <= hi.x;
}

// The partition the tree is built over: the word of its page table in the current one, and how
// many of its ray list's entries its leaves of four hold.
uint g_table = 0u;
uint g_leaf_records = 0u;

// The word of entry `i` of the partition's ray list.
uint velk_tree_ray_word(uint i)
{
    return velk_cut_ray_word(pc.cut_word, velk_cut_place(g_table, i));
}

// Puts the `count` entries of the partition's ray list in Morton order of their spheres' centres
// within the box around them, so that the tree's leaves of four hold records near each other;
// records at least a quarter as wide as that box go after them, together, so that they widen as
// few nodes as they can; records without a box last. Leaves s_leaf_key and s_bounded. All
// threads of the group call it.
void velk_sort_ray_list(uint count, uint lane)
{
    vec3 lo = vec3(1e30);
    vec3 hi = vec3(-1e30);
    for (uint i = lane; i < count; i += 64u) {
        vec3 blo, bhi, c;
        float radius;
        if (!velk_ray_entry_box(velk_tree_ray_word(i), blo, bhi, c, radius)) continue;
        lo = min(lo, c);
        hi = max(hi, c);
    }
    s_lo[lane] = lo;
    s_hi[lane] = hi;
    if (lane == 0u) s_bounded = 0u;
    memoryBarrierShared();
    barrier();
    for (uint i = 0u; i < 64u; ++i) {
        lo = min(lo, s_lo[i]);
        hi = max(hi, s_hi[i]);
    }
    float extent = max(max(hi.x - lo.x, hi.y - lo.y), hi.z - lo.z);
    uint n = 1u;
    while (n < count) n <<= 1u;
    for (uint i = lane; i < n; i += 64u) {
        uint key = 0xFFFFFFFFu;
        if (i < count) {
            uint at = velk_tree_ray_word(i);
            vec3 blo, bhi, c;
            float radius;
            // 20 bits of Morton code under a bit for the wide ones; a record without a box last,
            // its code above every other.
            uint code = 0x1FFFFFu;
            if (velk_ray_entry_box(at, blo, bhi, c, radius)) {
                code = ((radius >= 0.25 * extent ? 1u : 0u) << 20u) | min(velk_morton(c, lo, hi) >> 10u, 0xFFFFEu);
                atomicAdd(s_bounded, 1u);
            }
            key = (code << kSortIndexBits) | i;
        }
        s_order[i] = key;
    }
    memoryBarrierShared();
    barrier();
    velk_sort_keys(n, lane, 64u);
    // Each leaf's key, its last record with a box's, so that a leaf holding a wide record goes
    // with the wide ones; before the keys give way to the records.
    for (uint g = lane; g * 4u < count; g += 64u) s_leaf_key[g] = s_order[min(g * 4u + 3u, max(s_bounded, 1u) - 1u)];
    memoryBarrierShared();
    barrier();
    // Each place's record in the new order, read before any entry is written again.
    for (uint i = lane; i < count; i += 64u) {
        s_order[i] = cull_words.data[velk_tree_ray_word(s_order[i] & kSortIndexMask) + VELK_CUT_RAY_INDEX];
    }
    memoryBarrierShared();
    barrier();
    for (uint i = lane; i < count; i += 64u) velk_write_ray_entry(velk_tree_ray_word(i), s_order[i]);
    memoryBarrierBuffer();
    barrier();
}

)" R"(
// Past kSortMax entries, the sort is a radix sort in the partition's node pages, before the tree
// is built on them: a page's kCutPageNodes nodes are four words for each of its kCutPageRecords
// entries, two buffers of (code, place in the list) pairs. A thread's run of entries is counted
// by digit, the counts summed in order of digit then thread, and the run moved in its order, so
// that each pass keeps the order of the one before.
shared uint s_radix[16u * 64u];
shared uint s_radix_sum[64];
// Whether the radix sort made the order: a leaf's key is then its code, made again from its entry
// (velk_lbvh_key); the box its codes are within.
bool g_large_sort = false;
vec3 g_sort_lo = vec3(0.0);
vec3 g_sort_hi = vec3(0.0);
float g_sort_extent = 0.0;

// Word `w` of the radix sort's scratch, in the partition's node pages.
uint velk_sort_scratch(uint w)
{
    return velk_lbvh_node_word(w / VELK_CUT_NODE_WORDS) + w % VELK_CUT_NODE_WORDS;
}

// The radix sort's code of the entry at word `at`: as velk_sort_ray_list's, all 30 bits of its
// Morton code under the bit for the wide ones, and above every other for one without a box.
uint velk_ray_code_large(uint at)
{
    vec3 blo, bhi, c;
    float radius;
    if (!velk_ray_entry_box(at, blo, bhi, c, radius)) return 0xFFFFFFFFu;
    return ((radius >= 0.25 * g_sort_extent ? 1u : 0u) << 30u) | velk_morton(c, g_sort_lo, g_sort_hi);
}

// velk_sort_ray_list for more than kSortMax entries. Leaves s_bounded. All threads of the group
// call it.
void velk_sort_ray_list_large(uint count, uint lane)
{
    vec3 lo = vec3(1e30);
    vec3 hi = vec3(-1e30);
    for (uint i = lane; i < count; i += 64u) {
        vec3 blo, bhi, c;
        float radius;
        if (!velk_ray_entry_box(velk_tree_ray_word(i), blo, bhi, c, radius)) continue;
        lo = min(lo, c);
        hi = max(hi, c);
    }
    s_lo[lane] = lo;
    s_hi[lane] = hi;
    if (lane == 0u) s_bounded = 0u;
    memoryBarrierShared();
    barrier();
    for (uint i = 0u; i < 64u; ++i) {
        lo = min(lo, s_lo[i]);
        hi = max(hi, s_hi[i]);
    }
    g_sort_lo = lo;
    g_sort_hi = hi;
    g_sort_extent = max(max(hi.x - lo.x, hi.y - lo.y), hi.z - lo.z);
    g_large_sort = true;
    for (uint i = lane; i < count; i += 64u) {
        uint code = velk_ray_code_large(velk_tree_ray_word(i));
        if (code != 0xFFFFFFFFu) atomicAdd(s_bounded, 1u);
        cull_words.data[velk_sort_scratch(2u * i)] = code;
        cull_words.data[velk_sort_scratch(2u * i + 1u)] = i;
    }
    memoryBarrierBuffer();
    memoryBarrierShared();
    barrier();
    uint run = (count + 63u) / 64u;
    uint begin = min(lane * run, count);
    uint end = min(begin + run, count);
    uint from = 0u;
    uint to = 2u * count;
    for (uint pass = 0u; pass < 8u; ++pass) {
        uint shift = 4u * pass;
        for (uint d = 0u; d < 16u; ++d) s_radix[d * 64u + lane] = 0u;
        memoryBarrierShared();
        barrier();
        for (uint i = begin; i < end; ++i) {
            uint d = (cull_words.data[velk_sort_scratch(from + 2u * i)] >> shift) & 15u;
            s_radix[d * 64u + lane] += 1u;
        }
        memoryBarrierShared();
        barrier();
        // Where each thread's entries of each digit go: the counts before it, digit then thread.
        uint sum = 0u;
        for (uint k = 0u; k < 16u; ++k) sum += s_radix[lane * 16u + k];
        s_radix_sum[lane] = sum;
        memoryBarrierShared();
        barrier();
        uint before = 0u;
        for (uint t = 0u; t < lane; ++t) before += s_radix_sum[t];
        for (uint k = 0u; k < 16u; ++k) {
            uint n = s_radix[lane * 16u + k];
            s_radix[lane * 16u + k] = before;
            before += n;
        }
        memoryBarrierShared();
        barrier();
        for (uint i = begin; i < end; ++i) {
            uint code = cull_words.data[velk_sort_scratch(from + 2u * i)];
            uint place = cull_words.data[velk_sort_scratch(from + 2u * i + 1u)];
            uint d = (code >> shift) & 15u;
            uint dst = s_radix[d * 64u + lane];
            s_radix[d * 64u + lane] = dst + 1u;
            cull_words.data[velk_sort_scratch(to + 2u * dst)] = code;
            cull_words.data[velk_sort_scratch(to + 2u * dst + 1u)] = place;
        }
        memoryBarrierBuffer();
        barrier();
        uint swap = from;
        from = to;
        to = swap;
    }
    // Each place's record in the new order, into the other buffer before any entry is written again.
    for (uint i = lane; i < count; i += 64u) {
        uint place = cull_words.data[velk_sort_scratch(from + 2u * i + 1u)];
        cull_words.data[velk_sort_scratch(to + i)] = cull_words.data[velk_tree_ray_word(place) + VELK_CUT_RAY_INDEX];
    }
    memoryBarrierBuffer();
    barrier();
    for (uint i = lane; i < count; i += 64u) velk_write_ray_entry(velk_tree_ray_word(i), cull_words.data[velk_sort_scratch(to + i)]);
    memoryBarrierBuffer();
    barrier();
}

uint velk_lbvh_key(uint g)
{
    if (g_large_sort) return velk_ray_code_large(velk_tree_ray_word(min(g * 4u + 3u, max(s_bounded, 1u) - 1u)));
    return s_leaf_key[g] >> kSortIndexBits;
}

// Leaf g: up to four entries from the ray list's place 4g, which never cross a page, as the first
// one's place in the pool times four plus how many less one.
uint velk_lbvh_leaf_ref(uint g)
{
    uint first = g * 4u;
    return 0x80000000u | (velk_cut_place(g_table, first) * 4u + min(4u, g_leaf_records - first) - 1u);
}

void velk_lbvh_leaf_box(uint ref, out vec3 lo, out vec3 hi)
{
    lo = vec3(1e30);
    hi = vec3(-1e30);
    uint first = (ref & 0x7FFFFFFFu) >> 2u;
    uint n = (ref & 3u) + 1u;
    for (uint r = 0u; r < n; ++r) {
        uint at = velk_cut_ray_word(pc.cut_word, first + r);
        lo = min(lo, velk_load_vec3(at + VELK_CUT_RAY_MIN));
        hi = max(hi, velk_load_vec3(at + VELK_CUT_RAY_MAX));
    }
}

// The tree's node i is on the partition's pages, named by its place in the pool.
uint velk_lbvh_node_word(uint i)
{
    return velk_cut_node_word(pc.cut_word, velk_cut_node_place(g_table, i));
}

uint velk_lbvh_node_ref(uint i)
{
    return velk_cut_node_place(g_table, i);
}

uint velk_lbvh_ref_word(uint ref)
{
    return velk_cut_node_word(pc.cut_word, ref);
}

void main()
{
    uint cut = pc.cut_word;
    if (gl_WorkGroupID.x >= cull_words.data[cut + VELK_CUT_PARTITIONS]) return;
    uint state = velk_cut_state(cut, gl_WorkGroupID.x);
    // The same for the whole group.
    if (cull_words.data[state + VELK_PART_CHOOSING] == 0u) return;
    uint records_count = cull_words.data[state + VELK_PART_COUNT];
    g_table = velk_cut_table(cut, true) + cull_words.data[state + VELK_PART_TABLE_FIRST];
    uint lane = gl_LocalInvocationIndex;

    // The records rays trace, in their order: each thread a run of them, counted, then placed
    // after the runs before.
    GlobalData g = velk_globals.data[pc.globals_base];
    mat4 vp = g.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
    vec3 viewpoint = vec3(uintBitsToFloat(cull_words.data[state + VELK_PART_FROM]), uintBitsToFloat(cull_words.data[state + VELK_PART_FROM + 1u]),
                          uintBitsToFloat(cull_words.data[state + VELK_PART_FROM + 2u]));
    uint per = (records_count + 63u) / 64u;
    uint from = min(lane * per, records_count);
    uint to = min(from + per, records_count);
    uint mine = 0u;
    vec3 draw_lo = vec3(1e30);
    vec3 draw_hi = vec3(-1e30);
    uint always = 0u;
    uint light_only = 0u;
    uint quads = 0u;
    for (uint r = from; r < to; ++r) {
        uint place = velk_cut_place(g_table, r);
        uint record = velk_cut_record_word(cut, place);
        if (velk_ray_record(record, place, viewpoint, pixels_per_unit)) ++mine;
        velk_draw_share(record, draw_lo, draw_hi, always, light_only, quads);
    }
    s_counts[lane] = mine;
    s_lo[lane] = draw_lo;
    s_hi[lane] = draw_hi;
    s_always[lane] = always;
    s_light_only[lane] = light_only;
    s_quads[lane] = quads;
    barrier();
    if (lane == 0u) {
        for (uint i = 1u; i < 64u; ++i) {
            draw_lo = min(draw_lo, s_lo[i]);
            draw_hi = max(draw_hi, s_hi[i]);
            always |= s_always[i];
            light_only += s_light_only[i];
            quads += s_quads[i];
        }
        for (uint k = 0u; k < 3u; ++k) {
            cull_words.data[state + VELK_PART_BOX + k] = floatBitsToUint(draw_lo[k]);
            cull_words.data[state + VELK_PART_BOX + 3u + k] = floatBitsToUint(draw_hi[k]);
        }
        cull_words.data[state + VELK_PART_ALWAYS] = always;
        cull_words.data[state + VELK_PART_LIGHT_ONLY] = light_only;
        cull_words.data[state + VELK_PART_QUADS] = quads;
    }
    uint at = 0u;
    for (uint i = 0u; i < lane; ++i) at += s_counts[i];
    for (uint r = from; r < to; ++r) {
        uint place = velk_cut_place(g_table, r);
        if (!velk_ray_record(velk_cut_record_word(cut, place), place, viewpoint, pixels_per_unit)) continue;
        velk_write_ray_entry(velk_tree_ray_word(at), place);
        ++at;
    }
    uint count = 0u;
    for (uint i = 0u; i < 64u; ++i) count += s_counts[i];
    if (lane == 0u) cull_words.data[state + VELK_PART_RAY_COUNT] = count;
    memoryBarrierBuffer();
    barrier();
    if (count == 0u) return;
    // Where they are, those without a box last and left out of the leaves: sorted in the group's
    // memory as far as it holds them, past that in the partition's node pages.
    bool sorted = count > 4u;
    uint leaf_records = count;
    if (count > kSortMax) {
        velk_sort_ray_list_large(count, lane);
        leaf_records = s_bounded;
    } else if (sorted) {
        velk_sort_ray_list(count, lane);
        leaf_records = s_bounded;
    }
    g_leaf_records = leaf_records;
    uint node_count = velk_cut_tree_nodes(count);
    velk_lbvh_build_bounded(node_count, (leaf_records + 3u) / 4u, lane, 64u, !sorted);
    if (lane == 0u) {
        cull_words.data[state + VELK_PART_ROOT] = velk_lbvh_node_ref(node_count - 1u);
        cull_words.data[state + VELK_PART_BALANCED] = sorted && g_lbvh_balanced ? max(g_lbvh_tried_height, 1u) : 0u;
    }
}
)";

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

/**
 * Builds a view's top tree for rays (kRayTopHeaderWords) over the trees of its kept cuts'
 * partitions, as they are this frame: each leaf's box is its partition's root's. Up to
 * kRaySortMax leaves, a radix tree (VELK_LBVH_GLSL) over those with records for rays in Morton
 * order of their boxes' centres; past that, a balanced tree over all of them in the order of the
 * cuts and of each cut's partition table, those without records not referenced. One work group.
 */
inline constexpr string_view instance_cut_top_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 256) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint top_word;
} pc;
)" VELK_CUT_GLSL VELK_SORT_GLSL VELK_LBVH_GLSL R"(
// The box around the leaves' centres: its min, then its max, as velk_float_order keeps them.
shared uint s_box[6];
// How many leaves have records for rays, which the sort puts first.
shared uint s_traced;
// Whether the tree as built no longer fits its leaves, and how many leaves its nodes name.
shared uint s_changed;
shared uint s_named;

// A float as a uint that orders as the float does, for atomicMin and atomicMax.
uint velk_float_order(float f)
{
    uint b = floatBitsToUint(f);
    return (b & 0x80000000u) != 0u ? ~b : b | 0x80000000u;
}

float velk_float_unorder(uint u)
{
    return uintBitsToFloat((u & 0x80000000u) != 0u ? u & 0x7FFFFFFFu : ~u);
}

vec3 velk_load_vec3(uint at)
{
    return vec3(uintBitsToFloat(cull_words.data[at]), uintBitsToFloat(cull_words.data[at + 1u]),
                uintBitsToFloat(cull_words.data[at + 2u]));
}

// The box of leaf `leaf` of the top tree whose leaves start at `leaf_word`: its partition's
// root's, empty for one without records for rays. False for that.
bool velk_leaf_box(uint top, uint leaf_word, uint leaf, out vec3 lo, out vec3 hi)
{
    lo = vec3(1e30);
    hi = vec3(-1e30);
    uint at = leaf_word + leaf * VELK_RAY_TOP_LEAF_WORDS;
    uint cut = cull_words.data[top + VELK_RAY_TOP_HEADER_WORDS + cull_words.data[at + VELK_RAY_TOP_LEAF_CUT] * VELK_RAY_TOP_CUT_WORDS
                               + VELK_RAY_TOP_CUT_CUT];
    uint state = velk_cut_state(cut, cull_words.data[at + VELK_RAY_TOP_LEAF_PARTITION]);
    if (cull_words.data[state + VELK_PART_RAY_COUNT] == 0u) return false;
    uint root = velk_cut_node_word(cut, cull_words.data[state + VELK_PART_ROOT]);
    lo = min(velk_load_vec3(root), velk_load_vec3(root + 6u));
    hi = max(velk_load_vec3(root + 3u), velk_load_vec3(root + 9u));
    return true;
}

void velk_store_box(uint at, vec3 lo, vec3 hi)
{
    cull_words.data[at] = floatBitsToUint(lo.x);
    cull_words.data[at + 1u] = floatBitsToUint(lo.y);
    cull_words.data[at + 2u] = floatBitsToUint(lo.z);
    cull_words.data[at + 3u] = floatBitsToUint(hi.x);
    cull_words.data[at + 4u] = floatBitsToUint(hi.y);
    cull_words.data[at + 5u] = floatBitsToUint(hi.z);
}

// The top tree being built: its word and its leaves' and nodes', and whether its leaves are
// sorted.
uint g_top = 0u;
uint g_leaf_word = 0u;
uint g_nodes = 0u;
bool g_sorted = false;

// The top tree's nodes are together, named by their index.
uint velk_lbvh_node_word(uint i)
{
    return g_nodes + i * VELK_CUT_NODE_WORDS;
}

uint velk_lbvh_node_ref(uint i)
{
    return i;
}

uint velk_lbvh_ref_word(uint ref)
{
    return g_nodes + ref * VELK_CUT_NODE_WORDS;
}

uint velk_lbvh_key(uint g)
{
    return s_order[g] >> kSortIndexBits;
}

// Leaf g in order: its partition's leaf, none for one without records for rays.
uint velk_lbvh_leaf_ref(uint g)
{
    uint leaf = g_sorted ? s_order[g] & kSortIndexMask : g;
    vec3 lo, hi;
    return velk_leaf_box(g_top, g_leaf_word, leaf, lo, hi) ? 0x80000000u | leaf : 0xFFFFFFFFu;
}

void velk_lbvh_leaf_box(uint ref, out vec3 lo, out vec3 hi)
{
    velk_leaf_box(g_top, g_leaf_word, ref & 0x7FFFFFFFu, lo, hi);
}

// Whether leaf `leaf` of the top tree whose leaves start at `leaf_word` has records for rays.
bool velk_leaf_traced(uint top, uint leaf_word, uint leaf)
{
    uint at = leaf_word + leaf * VELK_RAY_TOP_LEAF_WORDS;
    uint cut = cull_words.data[top + VELK_RAY_TOP_HEADER_WORDS + cull_words.data[at + VELK_RAY_TOP_LEAF_CUT] * VELK_RAY_TOP_CUT_WORDS
                               + VELK_RAY_TOP_CUT_CUT];
    return cull_words.data[velk_cut_state(cut, cull_words.data[at + VELK_RAY_TOP_LEAF_PARTITION]) + VELK_PART_RAY_COUNT] != 0u;
}

// Whether the tree built before still fits its leaves: it was built, no partition of its cuts
// chose again this frame (a partition's tree, and so its root's box, changes only then), and its
// nodes name exactly the leaves with records for rays. All threads of the group call it.
bool velk_top_unchanged(uint top, uint cuts, uint leaf_word, uint leaves, uint nodes, uint node_count, uint lane)
{
    if (lane == 0u) {
        s_changed = cull_words.data[top + VELK_RAY_TOP_BUILT] == 0u ? 1u : 0u;
        s_named = 0u;
        s_traced = 0u;
    }
    memoryBarrierShared();
    barrier();
    for (uint c = lane; c < cuts; c += 256u) {
        uint cut = cull_words.data[top + VELK_RAY_TOP_HEADER_WORDS + c * VELK_RAY_TOP_CUT_WORDS + VELK_RAY_TOP_CUT_CUT];
        if (cull_words.data[cut + VELK_CUT_CHOOSE_GROUPS] != 0u) atomicOr(s_changed, 1u);
    }
    for (uint i = lane; i < leaves; i += 256u) {
        if (velk_leaf_traced(top, leaf_word, i)) atomicAdd(s_traced, 1u);
    }
    for (uint i = lane; i < node_count; i += 256u) {
        for (uint k = 0u; k < 2u; ++k) {
            uint ref = cull_words.data[nodes + i * VELK_CUT_NODE_WORDS + VELK_CUT_NODE_LEFT_REF + k];
            if (ref == 0xFFFFFFFFu || (ref & 0x80000000u) == 0u) continue;
            uint leaf = ref & 0x7FFFFFFFu;
            if (leaf >= leaves || !velk_leaf_traced(top, leaf_word, leaf)) atomicOr(s_changed, 1u);
            else atomicAdd(s_named, 1u);
        }
    }
    memoryBarrierShared();
    barrier();
    bool unchanged = s_changed == 0u && s_named == s_traced;
    barrier();
    return unchanged;
}

void main()
{
    uint top = pc.top_word;
    uint cuts = cull_words.data[top + VELK_RAY_TOP_CUTS];
    uint leaves = cull_words.data[top + VELK_RAY_TOP_LEAVES];
    if (leaves == 0u) return;
    uint leaf_word = top + VELK_RAY_TOP_HEADER_WORDS + cuts * VELK_RAY_TOP_CUT_WORDS;
    uint nodes = leaf_word + leaves * 2u;
    uint lane = gl_LocalInvocationIndex;
    uint node_count = leaves > 1u ? leaves - 1u : 1u;
    // Built before over the same partition trees: kept as it is.
    if (velk_top_unchanged(top, cuts, leaf_word, leaves, nodes, node_count, lane)) return;

    // The leaves in Morton order of their boxes' centres within the box around them all, so
    // that partitions near each other share nodes; those at least a quarter as wide as that box
    // after them, together, so that they widen as few nodes as they can; those without records
    // for rays last. Past kSortMax leaves, in their order.
    bool sorted = leaves <= kSortMax;
    if (sorted) {
        if (lane < 6u) s_box[lane] = lane < 3u ? 0xFFFFFFFFu : 0u;
        if (lane == 0u) s_traced = 0u;
        memoryBarrierShared();
        barrier();
        for (uint i = lane; i < leaves; i += 256u) {
            vec3 blo, bhi;
            if (!velk_leaf_box(top, leaf_word, i, blo, bhi)) continue;
            vec3 c = (blo + bhi) * 0.5;
            for (uint k = 0u; k < 3u; ++k) {
                atomicMin(s_box[k], velk_float_order(c[k]));
                atomicMax(s_box[3u + k], velk_float_order(c[k]));
            }
        }
        memoryBarrierShared();
        barrier();
        vec3 lo = vec3(velk_float_unorder(s_box[0]), velk_float_unorder(s_box[1]), velk_float_unorder(s_box[2]));
        vec3 hi = vec3(velk_float_unorder(s_box[3]), velk_float_unorder(s_box[4]), velk_float_unorder(s_box[5]));
        float extent = max(max(hi.x - lo.x, hi.y - lo.y), hi.z - lo.z);
        uint n = 1u;
        while (n < leaves) n <<= 1u;
        for (uint i = lane; i < n; i += 256u) {
            uint key = 0xFFFFFFFFu;
            if (i < leaves) {
                vec3 blo, bhi;
                // 20 bits of Morton code under a bit for the wide ones; one without records last.
                uint code = 0x1FFFFFu;
                if (velk_leaf_box(top, leaf_word, i, blo, bhi)) {
                    bool wide = length(bhi - blo) * 0.5 >= 0.25 * extent;
                    code = ((wide ? 1u : 0u) << 20u) | min(velk_morton((blo + bhi) * 0.5, lo, hi) >> 10u, 0xFFFFEu);
                    atomicAdd(s_traced, 1u);
                }
                key = (code << kSortIndexBits) | i;
            }
            s_order[i] = key;
        }
        memoryBarrierShared();
        barrier();
        velk_sort_keys(n, lane, 256u);
    }

    // Sorted, only the leaves with records for rays, which come first; else all of them in a
    // balanced tree, those without records not there for the walk (no reference).
    g_top = top;
    g_leaf_word = leaf_word;
    g_nodes = nodes;
    g_sorted = sorted;
    velk_lbvh_build_bounded(node_count, sorted ? s_traced : leaves, lane, 256u, !sorted);
    if (lane == 0u) cull_words.data[top + VELK_RAY_TOP_BUILT] = 1u;
}
)";

/// Runs of a kept cut's records that instance_cut_draw_src walks (VELK_CUT_VISIBLE_WORD): at
/// most a run per page of the pool and one more per partition.
inline uint32_t cut_visible_runs(uint32_t capacity, uint32_t partitions)
{
    return capacity / kCutPageRecords + partitions + 1u;
}

/**
 * Lists the records instance_cut_draw_src walks: those of each partition whose box around what
 * it draws (VELK_PART_BOX) is in view, or that is always visited, a run a page (its first record's
 * pool place and the end of those in use), the group counts of the drawing dispatch in
 * VELK_CUT_VISIBLE_GROUPS. With `whole` set, every partition's. Adds up the partitions' counts
 * for the log. A thread per partition.
 */
inline constexpr string_view instance_cut_visible_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint cut_word;
    uint globals_base;
    uint whole;       // 1 to list every partition, in view or not
    uint quads_word;  // the view's quad list, for its count of quads in cuts, or 0xFFFFFFFF
} pc;
)" VELK_CUT_GLSL VELK_TRACE_GLSL R"(
// Whether the box from `lo` to `hi`, relative to the camera, is outside one of the view's
// planes, tested as instance_cut_draw_src tests spheres: clip z by the wider of its two
// conventions.
bool velk_box_outside(mat4 vp, vec3 lo, vec3 hi)
{
    vec3 c = (lo + hi) * 0.5;
    vec3 e = (hi - lo) * 0.5;
    vec3 r3 = vec3(vp[0][3], vp[1][3], vp[2][3]);
    for (int k = 0; k < 3; ++k) {
        vec3 rk = vec3(vp[0][k], vp[1][k], vp[2][k]);
        for (int s = 0; s < 2; ++s) {
            vec3 n = s == 0 ? r3 + rk : r3 - rk;
            float d = s == 0 ? vp[3][3] + vp[3][k] : vp[3][3] - vp[3][k];
            if (dot(n, c) + d + dot(abs(n), e) < 0.0) return true;
        }
    }
    return false;
}

void main()
{
    uint cut = pc.cut_word;
    uint index = gl_GlobalInvocationID.x;
    uint partitions = cull_words.data[cut + VELK_CUT_PARTITIONS];
    if (index >= partitions) return;
    uint state = velk_cut_state(cut, index);
    uint count = cull_words.data[state + VELK_PART_COUNT];
    if (count == 0u) return;
    uint light_only = cull_words.data[state + VELK_PART_LIGHT_ONLY];
    if (light_only != 0u) atomicAdd(cull_words.data[cut + VELK_CUT_LIGHT_ONLY_RECORDS], light_only);
    uint quads = cull_words.data[state + VELK_PART_QUADS];
    if (pc.quads_word != 0xFFFFFFFFu && quads != 0u) atomicAdd(cull_words.data[pc.quads_word + VELK_QUAD_LIST_IN_CUTS], quads);

    // Traced: a partition whose box the traced pixel's ray passes, whether listed or why not.
    GlobalData g = velk_globals.data[pc.globals_base];
    vec3 lo = vec3(uintBitsToFloat(cull_words.data[state + VELK_PART_BOX]), uintBitsToFloat(cull_words.data[state + VELK_PART_BOX + 1u]),
                   uintBitsToFloat(cull_words.data[state + VELK_PART_BOX + 2u]));
    vec3 hi = vec3(uintBitsToFloat(cull_words.data[state + VELK_PART_BOX + 3u]), uintBitsToFloat(cull_words.data[state + VELK_PART_BOX + 4u]),
                   uintBitsToFloat(cull_words.data[state + VELK_PART_BOX + 5u]));
    bool boxed = all(lessThanEqual(lo, hi));
    bool traced = g.trace != VELK_TRACE_NONE && boxed && velk_trace_box(g, lo - g.cam_pos.xyz, hi - g.cam_pos.xyz);
    if (pc.whole == 0u && cull_words.data[state + VELK_PART_ALWAYS] == 0u) {
        // Nothing drawn: only records for light only, or patches for rays.
        if (!boxed) return;
        if (velk_box_outside(g.view_projection, lo - g.cam_pos.xyz, hi - g.cam_pos.xyz)) {
            if (traced) velk_trace_note(g, VELK_TRACE_PARTITION, 1u, index, count, 0u);
            return;
        }
    }

    uint table = velk_cut_table(cut, true) + cull_words.data[state + VELK_PART_TABLE_FIRST];
    uint runs = velk_cut_pages(count);
    uint room = cull_words.data[cut + VELK_CUT_CAPACITY] / VELK_CUT_PAGE_RECORDS + partitions + 1u;
    uint at = atomicAdd(cull_words.data[cut + VELK_CUT_VISIBLE_GROUPS], runs);
    if (traced) velk_trace_note(g, VELK_TRACE_PARTITION, at + runs > room ? 2u : 0u, index, count, 0u);
    uint list = cull_words.data[cut + VELK_CUT_VISIBLE_WORD];
    for (uint r = 0u; r < runs && at + r < room; ++r) {
        uint first = cull_words.data[table + r] * VELK_CUT_PAGE_RECORDS;
        cull_words.data[list + (at + r) * 2u] = first;
        cull_words.data[list + (at + r) * 2u + 1u] = first + min(count - r * VELK_CUT_PAGE_RECORDS, VELK_CUT_PAGE_RECORDS);
    }
}
)";

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
inline constexpr string_view instance_cut_draw_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct CullInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer CullInstances { CullInstance data[]; } cull_instances;
layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;
layout(set = 1, binding = 12, std430) readonly buffer LodNodes { uint data[]; } lod_nodes;

layout(push_constant) uniform PC {
    uint cut_word;
    uint instances_base;
    uint table_word;
    uint globals_base;
    float max_distance;
    float lod_max_pixels;
    uint chosen_word;
    uint chosen_capacity;
    uint pyramid_word;     // the previous frame's depth pyramid, or 0xFFFFFFFF for none
    uint retest_word;
    uint retest_capacity;
    uint tiny_word;
    uint tiny_capacity;
    float tiny_pixels;
    uint whole;            // 1 to draw every record, in view or not (ICamera::choose_as)
    uint items_word;       // plain items records name in place of slots, or 0xFFFFFFFF (instance_cut_choose_src)
    uint quads_word;       // the view's quad list (kQuadListHeaderWords), or 0xFFFFFFFF
    uint quad_capacity;
    float quad_pad;        // what a quad's sphere grows by, for instances a motion function moves
    uint item_records_word;  // by plain item, the record of one its own pass draws (InstanceCullPass::together)
} pc;
)" VELK_CUT_GLSL VELK_OCCLUSION_GLSL VELK_TINY_ADD_GLSL VELK_TRACE_GLSL R"(
uint g_instance;
uint g_table;
uint g_traced = 0xFFFFFFFFu;  // what came of the traced record this thread draws, or 0xFFFFFFFF

void velk_lod_use(uint entry, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[pc.chosen_word + VELK_CHOSEN_LIST_COUNT], 1u);
    if (g_traced != 0xFFFFFFFFu) g_traced = at >= pc.chosen_capacity ? 4u : 0u;
    if (at >= pc.chosen_capacity) return;
    uint record = pc.chosen_word + VELK_CHOSEN_LIST_RECORDS + at * VELK_CHOSEN_END;
    cull_words.data[record + VELK_CHOSEN_ENTRY] = entry;
    cull_words.data[record + VELK_CHOSEN_CLUSTER] = cluster;
    cull_words.data[record + VELK_CHOSEN_SLOT] = slot;
    cull_words.data[record + VELK_CHOSEN_CUT_RECORD] = cut_record;
    atomicAdd(cull_words.data[cull_words.data[entry + VELK_CLUSTER_ENTRY_COUNTS] + cluster], 1u);
}

// Lists a cluster for the second test, false when there is no room for it. Levels of detail
// were chosen already, so where from no longer matters: the camera stands in.
bool velk_retest(uint entry, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[pc.retest_word + VELK_RETEST_LIST_COUNT], 1u);
    if (at >= pc.retest_capacity) return false;
    uint record = pc.retest_word + VELK_RETEST_LIST_RECORDS + at * VELK_RETEST_END;
    cull_words.data[record + VELK_RETEST_INSTANCE] = g_instance;
    cull_words.data[record + VELK_RETEST_SLOT] = slot;
    cull_words.data[record + VELK_RETEST_TABLE] = g_table;
    cull_words.data[record + VELK_RETEST_ENTRY] = entry;
    cull_words.data[record + VELK_RETEST_CLUSTER] = cluster;
    cull_words.data[record + VELK_RETEST_CUT_RECORD] = cut_record;
    cull_words.data[record + VELK_RETEST_CHOSEN_WORD] = pc.chosen_word;
    cull_words.data[record + VELK_RETEST_CHOSEN_CAPACITY] = pc.chosen_capacity;
    return true;
}

void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     bool deforming)
{
}
)" VELK_LOD_SELECT_GLSL VELK_LOD_TAKE_GLSL R"(
// Whether a sphere around `center`, relative to the camera, is outside the view.
bool velk_cut_outside(mat4 vp, vec3 center, float radius)
{
    vec4 clip = vp * vec4(center, 1.0);
    vec3 r0 = vec3(vp[0][0], vp[1][0], vp[2][0]);
    vec3 r1 = vec3(vp[0][1], vp[1][1], vp[2][1]);
    vec3 r2 = vec3(vp[0][2], vp[1][2], vp[2][2]);
    vec3 r3 = vec3(vp[0][3], vp[1][3], vp[2][3]);
    float w_reach = radius * length(r3);
    return abs(clip.x) > clip.w + w_reach + radius * length(r0) || abs(clip.y) > clip.w + w_reach + radius * length(r1)
        || abs(clip.z) > clip.w + w_reach + radius * length(r2);
}

void main()
{
    uint cut = pc.cut_word;
    // A group a run of the records of partitions in view (instance_cut_visible_src).
    uint run = gl_WorkGroupID.x;
    if (run >= cull_words.data[cut + VELK_CUT_CAPACITY] / VELK_CUT_PAGE_RECORDS + cull_words.data[cut + VELK_CUT_PARTITIONS] + 1u) return;
    uint runs = cull_words.data[cut + VELK_CUT_VISIBLE_WORD] + run * 2u;
    uint place = cull_words.data[runs] + gl_LocalInvocationIndex;
    if (place >= cull_words.data[runs + 1u]) return;
    uint record = velk_cut_record_word(cut, place);
    uint entry = cull_words.data[record + VELK_RECORD_ENTRY];
    uint cluster_or_table = cull_words.data[record + VELK_RECORD_CLUSTER];
    uint slot = cull_words.data[record + VELK_RECORD_SLOT];

    // For light only, or a patch for rays only: not drawn.
    if (velk_cut_light_only(entry) || entry == VELK_CUT_RAY_QUAD) return;

    // A quad: in view, it goes to the view's quad list, never past its room, its record in the
    // column after.
    if (entry == VELK_CUT_QUAD) {
        if (pc.quads_word == 0xFFFFFFFFu) return;
        uint w = cluster_or_table;
        GlobalData globals = velk_globals.data[pc.globals_base];
        vec3 center = vec3(uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER]),
                           uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER + 1u]),
                           uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER + 2u]))
                    - globals.cam_pos.xyz;
        float radius = uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_RADIUS]) + pc.quad_pad;
        if (pc.whole == 0u && velk_cut_outside(globals.view_projection, center, radius)) return;
        uint at = atomicAdd(cull_words.data[pc.quads_word + VELK_QUAD_LIST_COUNT], 1u);
        if (at < pc.quad_capacity) {
            cull_words.data[pc.quads_word + VELK_QUAD_LIST_QUADS + at] = w;
            cull_words.data[pc.quads_word + VELK_QUAD_LIST_QUADS + pc.quad_capacity + at] = record;
        } else {
            atomicAdd(cull_words.data[pc.quads_word + VELK_QUAD_LIST_COUNT], 0xFFFFFFFFu);
        }
        return;
    }

    // A record names its item: the instance, its slot and its table (kPlainItemWords). An
    // instance without a place in its raster region is not drawn.
    uint instance = pc.instances_base + slot;
    uint table_word = pc.table_word;
    if (pc.items_word != 0xFFFFFFFFu) {
        uint at = pc.items_word + slot * VELK_PLAIN_ITEM_WORDS;
        uint item = slot;
        instance = cull_words.data[at];
        slot = cull_words.data[at + VELK_PLAIN_ITEM_SLOT];
        table_word = cull_words.data[at + VELK_PLAIN_ITEM_TABLE];
        // Rays only: drawn by its own pass, whose pixels find their record by item.
        if (table_word == 0xFFFFFFFFu) {
            cull_words.data[pc.item_records_word + item] = record;
            return;
        }
        if (instance == 0xFFFFFFFFu) return;
    }
    CullInstance ci = cull_instances.data[instance];
    mat4 world = velk_sized_world(ci.world_matrix, ci.offset.xyz, ci.size.xyz);
    if (world[3][3] == 0.0) return;
    uint table = table_word + ci.params.y * VELK_TABLE_END;
    GlobalData globals = velk_globals.data[pc.globals_base];
    vec3 cam = globals.cam_pos.xyz;
    mat4 vp = globals.view_projection;
    float scale2 = max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)), dot(world[2].xyz, world[2].xyz));
    float scale = sqrt(scale2);

    // The whole instance: near enough.
    vec3 center = velk_rel_position(world, vec3(uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER]),
                                                uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER + 1u]),
                                                uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER + 2u])), globals);
    float radius = uintBitsToFloat(cull_words.data[table + VELK_TABLE_RADIUS]) * scale;
    if (pc.max_distance > 0.0 && length(center) - radius > pc.max_distance) return;
    g_instance = instance;
    g_table = table_word;

    if (entry == 0xFFFFFFFFu) {
        if (pc.whole == 0u && velk_cut_outside(vp, center, radius)) return;
        uint at = atomicAdd(cull_words.data[cluster_or_table + VELK_TABLE_COUNT], 1u);
        uint capacity = cull_words.data[cluster_or_table + VELK_TABLE_CAPACITY];
        if (at < capacity) {
            uint list = cull_words.data[cluster_or_table + VELK_TABLE_LIST];
            cull_words.data[list + at] = slot;
            cull_words.data[list + capacity + at] = record;
        } else {
            // No room: the count, which the draw's instance count is, gives it back.
            atomicAdd(cull_words.data[cluster_or_table + VELK_TABLE_COUNT], 0xFFFFFFFFu);
        }
        return;
    }

    uint c = cluster_or_table;
    uint cluster = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + c * VELK_LOD_CLUSTER_WORDS;
    // Its cluster's errors changed after its partition chose, as loading or dropping detail
    // changes them: the partition, its page's owner, chooses again on the next frame.
    uint changed = cull_words.data[cluster + VELK_LOD_CLUSTER_CHANGED];
    if (changed != 0u) {
        uint place = (record - cull_words.data[pc.cut_word + VELK_CUT_RECORD_POOL]) / VELK_RECORD_END;
        uint owner = cull_words.data[cull_words.data[pc.cut_word + VELK_CUT_PAGE_HEADERS]
                                     + (place / VELK_CUT_PAGE_RECORDS) * VELK_CUT_PAGE_WORDS + VELK_CUT_PAGE_OWNER];
        if (owner < cull_words.data[pc.cut_word + VELK_CUT_PARTITIONS]) {
            uint owner_state = velk_cut_state(pc.cut_word, owner);
            if (changed > cull_words.data[owner_state + VELK_PART_CHOSE]) cull_words.data[owner_state + VELK_PART_RETRY] = 1u;
        }
    }
    uint entry_flags = cull_words.data[entry + VELK_CLUSTER_ENTRY_FLAGS];
    // A deforming mesh's spheres are where it is at rest, which says nothing about where it is.
    bool deforming = (entry_flags & VELK_LOD_DEFORMING) != 0u;
    float own_radius = uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale;
    vec3 own_center = velk_lod_center(cluster + VELK_LOD_CLUSTER_OWN_SPHERE, world, cam);
    // Traced: what comes of a cluster the traced pixel's ray passes.
    bool traced = globals.trace != VELK_TRACE_NONE && velk_trace_sphere(globals, own_center, own_radius);
    if (!deforming && pc.whole == 0u) {
        if (velk_cut_outside(vp, own_center, own_radius)) {
            if (traced) velk_trace_note(globals, VELK_TRACE_DRAW, 1u, entry, c, record);
            return;
        }
        // Tested where it was last frame, against what was drawn then. A blended batch's own pass
        // draws only the first phase; its depth test hides what is behind.
        float test_radius = uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_SPHERE + 3u]) * scale;
        if (pc.pyramid_word != 0xFFFFFFFFu && (entry_flags & VELK_LOD_RAYS_ONLY) == 0u
            && velk_behind_pyramid(pc.pyramid_word, globals.prev_view_projection,
                                   velk_lod_center(cluster + VELK_LOD_CLUSTER_SPHERE, world, cam) + globals.cam_moved, test_radius)
            && velk_retest(entry, c, slot, record)) {
            if (traced) velk_trace_note(globals, VELK_TRACE_DRAW, 2u, entry, c, record);
            return;
        }
    }
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * globals.viewport.y * 0.5;
    // Chosen or the list full, as velk_lod_use says; else compute raster took it.
    if (traced) g_traced = 3u;
    velk_lod_take(entry, c, slot, record, world, scale, cam, cam, pixels_per_unit);
    if (traced) velk_trace_note(globals, VELK_TRACE_DRAW, g_traced, entry, c, record);
}
)";

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
inline constexpr string_view instance_cull_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct CullInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer CullInstances { CullInstance data[]; } cull_instances;
layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;
layout(set = 1, binding = 12, std430) readonly buffer LodNodes { uint data[]; } lod_nodes;

layout(push_constant) uniform PC {
    uint instances_base;
    uint slot_count;
    uint table_word;
    uint globals_base;
    float max_distance;
    float lod_max_pixels;
    uint items_word;
    uint chosen_word;
    uint chosen_capacity;
    uint pyramid_word;     // the previous frame's depth pyramid, or 0xFFFFFFFF for none
    uint retest_word;
    uint retest_capacity;
    uint tiny_word;        // the list of tiny clusters, see kTinyHeaderWords
    uint tiny_capacity;
    float tiny_pixels;     // 0 for none
} pc;
)" VELK_OCCLUSION_GLSL VELK_TINY_ADD_GLSL VELK_TRACE_GLSL R"(
uint g_instance;
uint g_table;
vec3 g_lod_cam;

void velk_lod_use(uint entry, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[pc.chosen_word + VELK_CHOSEN_LIST_COUNT], 1u);
    if (at >= pc.chosen_capacity) return;
    uint record = pc.chosen_word + VELK_CHOSEN_LIST_RECORDS + at * VELK_CHOSEN_END;
    cull_words.data[record + VELK_CHOSEN_ENTRY] = entry;
    cull_words.data[record + VELK_CHOSEN_CLUSTER] = cluster;
    cull_words.data[record + VELK_CHOSEN_SLOT] = slot;
    cull_words.data[record + VELK_CHOSEN_CUT_RECORD] = cut_record;
    atomicAdd(cull_words.data[cull_words.data[entry + VELK_CLUSTER_ENTRY_COUNTS] + cluster], 1u);
}

// Lists something for the second test, false when there is no room for it.
bool velk_retest(uint entry, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[pc.retest_word + VELK_RETEST_LIST_COUNT], 1u);
    if (at >= pc.retest_capacity) return false;
    uint record = pc.retest_word + VELK_RETEST_LIST_RECORDS + at * VELK_RETEST_END;
    cull_words.data[record + VELK_RETEST_INSTANCE] = g_instance;
    cull_words.data[record + VELK_RETEST_SLOT] = slot;
    cull_words.data[record + VELK_RETEST_TABLE] = g_table;
    cull_words.data[record + VELK_RETEST_ENTRY] = entry;
    cull_words.data[record + VELK_RETEST_CLUSTER] = cluster;
    cull_words.data[record + VELK_RETEST_CUT_RECORD] = cut_record;
    cull_words.data[record + VELK_RETEST_CHOSEN_WORD] = pc.chosen_word;
    cull_words.data[record + VELK_RETEST_CHOSEN_CAPACITY] = pc.chosen_capacity;
    return true;
}

bool velk_lod_occluded(uint word, mat4 world, float scale, vec3 cam, uint entry, uint cluster, uint slot);
void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     bool deforming);
)" VELK_LOD_SELECT_GLSL VELK_LOD_TAKE_GLSL R"(
// Tested where it was last frame, against what was drawn then.
bool velk_lod_occluded(uint word, mat4 world, float scale, vec3 cam, uint entry, uint cluster, uint slot)
{
    if (pc.pyramid_word == 0xFFFFFFFFu) return false;
    GlobalData g = velk_globals.data[pc.globals_base];
    float radius = uintBitsToFloat(cull_words.data[word + 3u]) * scale;
    return velk_behind_pyramid(pc.pyramid_word, g.prev_view_projection, velk_lod_center(word, world, cam) + g.cam_moved, radius)
        && velk_retest(entry, cluster, slot, 0xFFFFFFFFu);
}

// Drawn unless hidden; a deforming mesh's spheres are where it is at rest, which says nothing
// about whether it is hidden. Chosen every frame, so from no kept cut record.
void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     bool deforming)
{
    // Clusters for rays only: their batch draws itself.
    if ((cull_words.data[entry + VELK_CLUSTER_ENTRY_FLAGS] & VELK_LOD_RAYS_ONLY) != 0u) return;
    uint cluster = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + c * VELK_LOD_CLUSTER_WORDS;
    // Traced: chosen (0) or held back by occlusion (2), as the kept cut's draw notes it, with no
    // record.
    GlobalData g = velk_globals.data[pc.globals_base];
    bool traced = g.trace != VELK_TRACE_NONE
               && velk_trace_sphere(g, velk_lod_center(cluster + VELK_LOD_CLUSTER_OWN_SPHERE, world, g.cam_pos.xyz),
                                    uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale);
    if (deforming || !velk_lod_occluded(cluster + VELK_LOD_CLUSTER_SPHERE, world, scale, cam, entry, c, slot)) {
        velk_lod_take(entry, c, slot, 0xFFFFFFFFu, world, scale, cam, lod_cam, pixels_per_unit);
        if (traced) velk_trace_note(g, VELK_TRACE_DRAW, 0u, entry, c, 0xFFFFFFFFu);
    } else if (traced) {
        velk_trace_note(g, VELK_TRACE_DRAW, 2u, entry, c, 0xFFFFFFFFu);
    }
}

void main()
{
    uint slot = gl_GlobalInvocationID.x;
    GlobalData globals = velk_globals.data[pc.globals_base];
    g_lod_cam = globals.cam_pos.xyz;
    if (slot >= pc.slot_count) return;

    uint instance = pc.instances_base + slot;
    uint table_word = pc.table_word;
    uint item_index = slot;
    if (pc.items_word != 0xFFFFFFFFu) {
        uint item = pc.items_word + item_index * VELK_PLAIN_ITEM_WORDS;
        instance = cull_words.data[item];
        slot = cull_words.data[item + VELK_PLAIN_ITEM_SLOT];
        table_word = cull_words.data[item + VELK_PLAIN_ITEM_TABLE];
        // Rays only: drawn by its own pass. Or an instance without a place in its raster region.
        if (table_word == 0xFFFFFFFFu || instance == 0xFFFFFFFFu) return;
    }

    CullInstance ci = cull_instances.data[instance];
    mat4 world = velk_sized_world(ci.world_matrix, ci.offset.xyz, ci.size.xyz);
    if (world[3][3] == 0.0) return;

    uint table = table_word + ci.params.y * VELK_TABLE_END;
    vec4 sphere = vec4(uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER]),
                       uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER + 1u]),
                       uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER + 2u]),
                       uintBitsToFloat(cull_words.data[table + VELK_TABLE_RADIUS]));
    vec3 center = velk_rel_position(world, sphere.xyz, globals);
    float scale2 = max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)),
                       dot(world[2].xyz, world[2].xyz));
    float radius = sphere.w * sqrt(scale2);
    uint first_entry = cull_words.data[table + VELK_TABLE_FIRST_ENTRY];
    uint entries = cull_words.data[table + VELK_TABLE_ENTRY_COUNT];
    // A deforming mesh's sphere is where it was at rest, which says nothing about whether it
    // is in view or hidden.
    bool deforming = entries != 0u && (cull_words.data[first_entry + VELK_CLUSTER_ENTRY_FLAGS] & VELK_LOD_DEFORMING) != 0u;

    // Traced: an instance the traced pixel's ray passes, culled every frame (kTraceInstance).
    bool traced = globals.trace != VELK_TRACE_NONE && velk_trace_sphere(globals, center, radius);
    if (!deforming && pc.max_distance > 0.0 && length(center) - radius > pc.max_distance) {
        if (traced) velk_trace_note(globals, 8u, 1u, first_entry, slot, entries);
        return;
    }

    // The sphere against each clip plane. Planes are rows of the matrix added to or
    // taken from its last row, so their normals' lengths are at most the sum of the
    // rows' lengths, which keeps this conservative. Near and far are tested with the
    // wider of the two conventions for clip z.
    mat4 vp = globals.view_projection;
    vec4 clip = vp * vec4(center, 1.0);
    vec3 r0 = vec3(vp[0][0], vp[1][0], vp[2][0]);
    vec3 r1 = vec3(vp[0][1], vp[1][1], vp[2][1]);
    vec3 r2 = vec3(vp[0][2], vp[1][2], vp[2][2]);
    vec3 r3 = vec3(vp[0][3], vp[1][3], vp[2][3]);
    float w_reach = radius * length(r3);
    if (!deforming) {
        if (abs(clip.x) > clip.w + w_reach + radius * length(r0) || abs(clip.y) > clip.w + w_reach + radius * length(r1)
            || abs(clip.z) > clip.w + w_reach + radius * length(r2)) {
            if (traced) velk_trace_note(globals, 8u, 2u, first_entry, slot, entries);
            return;
        }
    }

    g_instance = instance;
    g_table = table_word;
    if (!deforming && pc.pyramid_word != 0xFFFFFFFFu
        && velk_behind_pyramid(pc.pyramid_word, globals.prev_view_projection, center + globals.cam_moved, radius)
        && velk_retest(0xFFFFFFFFu, 0u, slot, 0xFFFFFFFFu)) {
        if (traced) velk_trace_note(globals, 8u, 3u, first_entry, slot, entries);
        return;
    }
    if (traced) velk_trace_note(globals, 8u, 0u, first_entry, slot, entries);

    if (pc.items_word == 0xFFFFFFFFu) {
        uint index = atomicAdd(cull_words.data[table + VELK_TABLE_COUNT], 1u);
        uint capacity = cull_words.data[table + VELK_TABLE_CAPACITY];
        if (index < capacity) {
            uint list = cull_words.data[table + VELK_TABLE_LIST];
            cull_words.data[list + index] = slot;
            cull_words.data[list + capacity + index] = 0xFFFFFFFFu;
        } else {
            // No room: the count, which the draw's instance count is, gives it back.
            atomicAdd(cull_words.data[table + VELK_TABLE_COUNT], 0xFFFFFFFFu);
        }
    }

    for (uint e = 0u; e < entries; ++e) {
        velk_lod_visit(first_entry + e * VELK_CLUSTER_ENTRY_WORDS, slot, world, sqrt(scale2), globals.cam_pos.xyz, g_lod_cam, vp,
                       globals.viewport.y, pc.lod_max_pixels);
    }
}
)";

/**
 * Tests what failed instance_cull_compute_src's occlusion test again, against the pyramid of
 * what this frame has drawn so far, and adds what is visible after what that test passed.
 */
inline constexpr string_view instance_retest_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct CullInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer CullInstances { CullInstance data[]; } cull_instances;
layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;
layout(set = 1, binding = 12, std430) readonly buffer LodNodes { uint data[]; } lod_nodes;

layout(push_constant) uniform PC {
    uint retest_word;
    uint retest_capacity;
    uint globals_base;
    uint pyramid_word;
    float lod_max_pixels;
    uint tiny_word;
    uint tiny_capacity;
    float tiny_pixels;
} pc;
)" VELK_OCCLUSION_GLSL VELK_TINY_ADD_GLSL R"(
uint g_chosen_word;
uint g_chosen_capacity;

void velk_lod_use(uint entry, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[g_chosen_word + VELK_CHOSEN_LIST_COUNT], 1u);
    if (at >= g_chosen_capacity) return;
    uint record = g_chosen_word + VELK_CHOSEN_LIST_RECORDS + at * VELK_CHOSEN_END;
    cull_words.data[record + VELK_CHOSEN_ENTRY] = entry;
    cull_words.data[record + VELK_CHOSEN_CLUSTER] = cluster;
    cull_words.data[record + VELK_CHOSEN_SLOT] = slot;
    cull_words.data[record + VELK_CHOSEN_CUT_RECORD] = cut_record;
    atomicAdd(cull_words.data[cull_words.data[entry + VELK_CLUSTER_ENTRY_COUNTS] + cluster], 1u);
}

bool velk_lod_occluded(uint word, mat4 world, float scale, vec3 cam);
void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     bool deforming);
)" VELK_LOD_SELECT_GLSL VELK_LOD_TAKE_GLSL R"(
bool velk_lod_occluded(uint word, mat4 world, float scale, vec3 cam)
{
    GlobalData g = velk_globals.data[pc.globals_base];
    float radius = uintBitsToFloat(cull_words.data[word + 3u]) * scale;
    return velk_behind_pyramid(pc.pyramid_word, g.view_projection, velk_lod_center(word, world, cam), radius);
}

// Of a whole instance chosen every frame, so from no kept cut record.
void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     bool deforming)
{
    uint cluster = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + c * VELK_LOD_CLUSTER_WORDS;
    if (deforming || !velk_lod_occluded(cluster + VELK_LOD_CLUSTER_SPHERE, world, scale, cam)) {
        velk_lod_take(entry, c, slot, 0xFFFFFFFFu, world, scale, cam, lod_cam, pixels_per_unit);
    }
}

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= min(cull_words.data[pc.retest_word + VELK_RETEST_LIST_COUNT], pc.retest_capacity)) return;

    uint record = pc.retest_word + VELK_RETEST_LIST_RECORDS + index * VELK_RETEST_END;
    uint instance = cull_words.data[record + VELK_RETEST_INSTANCE];
    uint slot = cull_words.data[record + VELK_RETEST_SLOT];
    uint table_word = cull_words.data[record + VELK_RETEST_TABLE];
    uint entry = cull_words.data[record + VELK_RETEST_ENTRY];
    g_chosen_word = cull_words.data[record + VELK_RETEST_CHOSEN_WORD];
    g_chosen_capacity = cull_words.data[record + VELK_RETEST_CHOSEN_CAPACITY];

    CullInstance ci = cull_instances.data[instance];
    mat4 world = velk_sized_world(ci.world_matrix, ci.offset.xyz, ci.size.xyz);
    float scale = sqrt(max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)),
                           dot(world[2].xyz, world[2].xyz)));
    GlobalData globals = velk_globals.data[pc.globals_base];
    // Its levels of detail are chosen from the camera, as the first test chose them.
    vec3 cam = globals.cam_pos.xyz;
    vec3 lod_cam = cam;

    if (entry != 0xFFFFFFFFu) {
        uint cluster = cull_words.data[record + VELK_RETEST_CLUSTER];
        uint sphere = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + cluster * VELK_LOD_CLUSTER_WORDS + VELK_LOD_CLUSTER_SPHERE;
        if (!velk_lod_occluded(sphere, world, scale, cam)) {
            mat4 vp = globals.view_projection;
            float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * globals.viewport.y * 0.5;
            velk_lod_take(entry, cluster, slot, cull_words.data[record + VELK_RETEST_CUT_RECORD], world, scale, cam, lod_cam,
                          pixels_per_unit);
        }
        return;
    }

    uint table = table_word + cull_instances.data[instance].params.y * VELK_TABLE_END;
    vec3 local_center = vec3(uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER]),
                             uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER + 1u]),
                             uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER + 2u]));
    float radius = uintBitsToFloat(cull_words.data[table + VELK_TABLE_RADIUS]) * scale;
    if (velk_behind_pyramid(pc.pyramid_word, globals.view_projection, velk_rel_position(world, local_center, globals), radius)) {
        return;
    }

    // After the positions the first test filled. A table without a list has no capacity.
    uint position = cull_words.data[table + VELK_TABLE_COUNT] + atomicAdd(cull_words.data[table + VELK_TABLE_SECOND_COUNT], 1u);
    uint capacity = cull_words.data[table + VELK_TABLE_CAPACITY];
    if (position < capacity) {
        uint list = cull_words.data[table + VELK_TABLE_LIST];
        cull_words.data[list + position] = slot;
        cull_words.data[list + capacity + position] = 0xFFFFFFFFu;
    } else {
        // No room: the count, which the second draw's instance count is, gives it back, so that
        // it never reads past the list.
        atomicAdd(cull_words.data[table + VELK_TABLE_SECOND_COUNT], 0xFFFFFFFFu);
    }

    uint first_entry = cull_words.data[table + VELK_TABLE_FIRST_ENTRY];
    uint entries = cull_words.data[table + VELK_TABLE_ENTRY_COUNT];
    for (uint e = 0u; e < entries; ++e) {
        velk_lod_visit(first_entry + e * VELK_CLUSTER_ENTRY_WORDS, slot, world, scale, cam, lod_cam, globals.view_projection,
                       globals.viewport.y, pc.lod_max_pixels);
    }
}
)";

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
inline constexpr string_view instance_cluster_runs_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint entry;
    uint entries_word;
    uint entry_count;
    uint chosen_word;
    uint second_phase;
} pc;

void main()
{
    uint invocation = gl_GlobalInvocationID.x;
    if (invocation >= pc.entry_count) return;
    uint entry = pc.entry;
    if (pc.entries_word != 0xFFFFFFFFu) {
        entry = cull_words.data[pc.entries_word + invocation];
    }
    if (invocation == 0u) {
        cull_words.data[pc.chosen_word + VELK_CHOSEN_LIST_GROUPS] = (cull_words.data[pc.chosen_word + VELK_CHOSEN_LIST_COUNT] + 63u) / 64u;
        cull_words.data[pc.chosen_word + VELK_CHOSEN_LIST_GROUPS + 1u] = 1u;
        cull_words.data[pc.chosen_word + VELK_CHOSEN_LIST_GROUPS + 2u] = 1u;
    }

    uint capacity = cull_words.data[entry + VELK_CLUSTER_ENTRY_CAPACITY];
    uint table = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE];
    uint clusters = cull_words.data[entry + VELK_CLUSTER_ENTRY_CLUSTERS];
    uint counts = cull_words.data[entry + VELK_CLUSTER_ENTRY_COUNTS];
    uint cursors = cull_words.data[entry + VELK_CLUSTER_ENTRY_CURSORS];
    uint commands = cull_words.data[entry + (pc.second_phase != 0u ? 11u : 6u)];

    uint first = pc.second_phase != 0u ? cull_words.data[entry + VELK_CLUSTER_ENTRY_FIRST_POSITIONS] : 0u;
    uint draws = 0u;
    for (uint c = 0u; c < clusters; ++c) {
        uint count = min(cull_words.data[counts + c], capacity - first);
        cull_words.data[counts + c] = 0u;
        cull_words.data[cursors + c] = first;
        if (count != 0u) {
            uint cluster = table + c * VELK_LOD_CLUSTER_WORDS;
            uint command = commands + draws * 5u;
            cull_words.data[command + 0u] = cull_words.data[cluster + VELK_LOD_CLUSTER_TRIANGLES] * 3u;
            cull_words.data[command + 1u] = count;
            cull_words.data[command + 2u] = cull_words.data[cluster + VELK_LOD_CLUSTER_INDEX];
            cull_words.data[command + 3u] = 0u;
            cull_words.data[command + 4u] = first;
            ++draws;
        }
        first += count;
    }
    if (pc.second_phase != 0u) {
        cull_words.data[entry + VELK_CLUSTER_ENTRY_SECOND_DRAWS] = draws;
    } else {
        cull_words.data[entry + VELK_CLUSTER_ENTRY_DRAWS] = draws;
        cull_words.data[entry + VELK_CLUSTER_ENTRY_FIRST_POSITIONS] = first;
    }
}
)";

/**
 * Sorts the records instance_cull_compute_src wrote into the runs of their clusters.
 */
inline constexpr string_view instance_cluster_fill_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint chosen_word;
    uint chosen_capacity;
} pc;

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= min(cull_words.data[pc.chosen_word + VELK_CHOSEN_LIST_COUNT], pc.chosen_capacity)) return;
    uint record = pc.chosen_word + VELK_CHOSEN_LIST_RECORDS + index * VELK_CHOSEN_END;
    uint entry = cull_words.data[record + VELK_CHOSEN_ENTRY];
    uint at = atomicAdd(cull_words.data[cull_words.data[entry + VELK_CLUSTER_ENTRY_CURSORS] + cull_words.data[record + VELK_CHOSEN_CLUSTER]], 1u);
    uint capacity = cull_words.data[entry + VELK_CLUSTER_ENTRY_CAPACITY];
    if (at < capacity) {
        uint list = cull_words.data[entry + VELK_CLUSTER_ENTRY_LIST];
        cull_words.data[list + at] = cull_words.data[record + VELK_CHOSEN_SLOT];
        cull_words.data[list + capacity + at] = cull_words.data[record + VELK_CHOSEN_CUT_RECORD];
        cull_words.data[list + capacity * 2u + at] = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE]
                                                   + cull_words.data[record + VELK_CHOSEN_CLUSTER] * VELK_LOD_CLUSTER_WORDS;
    }
}
)";

/**
 * Writes the group counts of the dispatch that draws the clusters culling listed for compute,
 * one group each, in rows of at most 65535 (what every device takes along one axis), and how
 * many of them are drawn, as kTinyHeaderWords describes.
 */
inline constexpr string_view instance_tiny_args_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 1) in;

layout(set = 1, binding = 16, std430) buffer TinyWords { uint data[]; } tiny_words;

layout(push_constant) uniform PC {
    uint list_word;
    uint capacity;
} pc;

void main()
{
    uint count = min(tiny_words.data[pc.list_word + VELK_TINY_LIST_COUNT], pc.capacity);
    uint row = clamp(count, 1u, 65535u);
    tiny_words.data[pc.list_word + VELK_TINY_LIST_GROUPS] = count == 0u ? 0u : row;
    tiny_words.data[pc.list_word + VELK_TINY_LIST_GROUPS + 1u] = (count + row - 1u) / row;
    tiny_words.data[pc.list_word + VELK_TINY_LIST_GROUPS + 2u] = 1u;
    tiny_words.data[pc.list_word + VELK_TINY_LIST_DRAWN] = count;
}
)";

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
#define VELK_TINY_DRAW_GLSL R"(
layout(local_size_x = 128) in;

struct TinyInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer TinyInstances { TinyInstance data[]; } tiny_instances;
layout(set = 1, binding = 16, std430) buffer TinyWords { uint data[]; } tiny_words;

layout(push_constant) uniform PC {
    uint list_word;
    uint planes_word;
    uint width;
    uint height;
    uint depth_id;  // the hardware's depth
    uint mode;
    uint most;
    uint wide_word;  // one pass: the 64-bit words, a pixel each
} pc;

#ifdef VELK_TINY_ONE_PASS
layout(set = 1, binding = 16, std430) buffer TinyWide { uint64_t data[]; } tiny_wide;
#endif

uint tiny_tie(uint record_index, uint triangle)
{
    uint h = record_index * 256u + triangle;
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    h ^= h >> 16u;
    return h & 0xFFu;
}

float tiny_edge(vec2 a, vec2 b, vec2 p)
{
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

void main()
{
    uint index = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
    if (index >= tiny_words.data[pc.list_word + VELK_TINY_LIST_DRAWN]) return;
    uint record = pc.list_word + VELK_TINY_LIST_RECORDS + index * VELK_TINY_END;
    uint draw = tiny_words.data[record + VELK_TINY_DRAW];
    VelkDrawData d = velk_draw_data.data[draw];
    uint cluster = tiny_words.data[record + VELK_TINY_CLUSTER];
    TinyInstance inst = tiny_instances.data[d.instances_base + tiny_words.data[record + VELK_TINY_SLOT]];
    GlobalData g = velk_globals.data[d.globals_base];
    // The cluster's record says where its indices are, and the vertices they count from.
    uint first = tiny_words.data[cluster + VELK_LOD_CLUSTER_INDEX];
    uint vertices = tiny_words.data[cluster + VELK_LOD_CLUSTER_VERTEX];
    uint count = tiny_words.data[cluster + VELK_LOD_CLUSTER_TRIANGLES];
    vec2 size = vec2(float(pc.width), float(pc.height));
    uint plane = pc.width * pc.height;

    for (uint t = gl_LocalInvocationID.x; t < count; t += 128u) {
        vec3 s[3];
        bool behind = false;
        for (uint k = 0u; k < 3u; ++k) {
            uint at = vertices + velk_mesh_words.data[first + t * 3u + k] * VELK_VERTEX3D_WORDS;
            vec3 local = vec3(uintBitsToFloat(velk_mesh_words.data[at]), uintBitsToFloat(velk_mesh_words.data[at + 1u]),
                              uintBitsToFloat(velk_mesh_words.data[at + 2u]));
            vec4 clip = g.view_projection
                * vec4(velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g), 1.0);
            behind = behind || clip.w <= 0.0;
            s[k] = vec3((clip.xy / clip.w * 0.5 + 0.5) * size, clip.z / clip.w);
        }
        float area = tiny_edge(s[0].xy, s[1].xy, s[2].xy);
        if (behind || area == 0.0) continue;

        vec2 lo = min(min(s[0].xy, s[1].xy), s[2].xy);
        vec2 hi = max(max(s[0].xy, s[1].xy), s[2].xy);
        ivec2 from = max(ivec2(ceil(lo - 0.5)), ivec2(0));
        ivec2 to = min(ivec2(floor(hi - 0.5)), ivec2(int(pc.width), int(pc.height)) - 1);
        to = min(to, from + int(pc.most) - 1);
#ifdef VELK_TINY_ONE_PASS
        uint64_t payload = uint64_t((index << 8u) | t);
#else
        uint tie = tiny_tie(index, t);
#endif
        vec3 depths = vec3(s[0].z, s[1].z, s[2].z);
        for (int y = from.y; y <= to.y; ++y) {
            for (int x = from.x; x <= to.x; ++x) {
                vec2 p = vec2(float(x), float(y)) + 0.5;
                vec3 b = vec3(tiny_edge(s[1].xy, s[2].xy, p), tiny_edge(s[2].xy, s[0].xy, p), tiny_edge(s[0].xy, s[1].xy, p)) / area;
                if (b.x < 0.0 || b.y < 0.0 || b.z < 0.0) continue;
                float depth = dot(b, depths);
                if (depth <= 0.0 || depth > 1.0) continue;
                if (texelFetch(velk_textures[nonuniformEXT(pc.depth_id)], ivec2(x, y), 0).r >= depth) continue;
                uint pixel = uint(y) * pc.width + uint(x);
#ifdef VELK_TINY_ONE_PASS
                atomicMax(tiny_wide.data[pc.wide_word / 2u + pixel], (uint64_t(floatBitsToUint(depth)) << 32) | payload);
#else
                uint bits = (floatBitsToUint(depth) & ~0xFFu) | tie;
                if (pc.mode == 0u) {
                    atomicMax(tiny_words.data[pc.planes_word + pixel], bits);
                } else if (tiny_words.data[pc.planes_word + pixel] == bits) {
                    uint ids = pc.planes_word + plane + pixel * VELK_PIXEL_RECORD_WORDS;
                    tiny_words.data[ids] = (draw + 1u) | VELK_IDS_TINY | (t << VELK_IDS_TRIANGLE_SHIFT);
                    tiny_words.data[ids + 1u] = record;
                }
#endif
            }
        }
    }
}
)"

inline constexpr string_view instance_tiny_draw_src = "#version 450\n#include \"velk.glsl\"\n" VELK_TINY_DRAW_GLSL;

/// Clusters instance_tiny_draw_one_pass_src can tell apart: its 64-bit words' low half holds a
/// cluster's place in the list above the triangle's 8 bits.
inline constexpr uint32_t kTinyOnePassRecords = 1u << 24u;

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
inline constexpr string_view instance_tiny_unpack_src = R"(
#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 1, binding = 16, std430) buffer TinyWords { uint data[]; } tiny_words;
layout(set = 1, binding = 16, std430) buffer TinyWide { uint64_t data[]; } tiny_wide;

layout(push_constant) uniform PC {
    uint list_word;
    uint planes_word;
    uint width;
    uint height;
    uint wide_word;
} pc;

void main()
{
    uvec2 coord = gl_GlobalInvocationID.xy;
    if (coord.x >= pc.width || coord.y >= pc.height) return;
    uint pixel = coord.y * pc.width + coord.x;
    uint64_t packed = tiny_wide.data[pc.wide_word / 2u + pixel];
    if (packed == 0ul) return;
    tiny_wide.data[pc.wide_word / 2u + pixel] = 0ul;
    uint low = uint(packed & 0xFFFFFFFFul);
    uint index = low >> 8u;
    uint t = low & 0xFFu;
    if (index >= tiny_words.data[pc.list_word + VELK_TINY_LIST_DRAWN]) return;
    uint record = pc.list_word + VELK_TINY_LIST_RECORDS + index * VELK_TINY_END;
    tiny_words.data[pc.planes_word + pixel] = uint(packed >> 32);
    uint ids = pc.planes_word + pc.width * pc.height + pixel * VELK_PIXEL_RECORD_WORDS;
    tiny_words.data[ids] = (tiny_words.data[record + VELK_TINY_DRAW] + 1u) | VELK_IDS_TINY | (t << VELK_IDS_TRIANGLE_SHIFT);
    tiny_words.data[ids + 1u] = record;
}
)";

/// The culling's passes, as the GPU timer names them: which partitions keep their records;
/// for those that do not, counting their records (with the copies for those that do), placing
/// them and writing them; the trees for rays, the light pages kept, and the draw lists.
inline constexpr uint32_t kCullStages = 7;
inline constexpr const char* kCullStageNames[kCullStages] = {
    "instance.decide", "instance.count", "instance.place", "instance.write",
    "instance.trees", "instance.light_marks", "instance.draws"};
/// The same for a view chosen as another camera.
inline constexpr const char* kCullOtherStageNames[kCullStages] = {
    "other:instance.decide", "other:instance.count", "other:instance.place", "other:instance.write",
    "other:instance.trees", "other:instance.light_marks", "other:instance.draws"};

/**
 * @brief One view's GPU culling passes, kept by the render path. Set @c dirty when the
 *        view's batches change.
 */
struct InstanceCullPass
{
    /// The culling, as passes timed apart, in order: kCullStageNames.
    IRenderPass::Ptr passes[kCullStages];
    bool dirty = true;
    /// The view is chosen as another camera: its passes are timed under kCullOtherStageNames.
    bool chosen_by_other = false;
    /// The GPU time the light trace aims for a frame (ICamera::light_trace_ms); 0 for no limit.
    float light_trace_ms = 4.f;
    vector<uint32_t> count_words;  ///< Reset before every cull.
    IGpuBuffer* arena_buffer = nullptr;
    size_t arena_size = 0;
    uint32_t arena_generation = 0;  ///< Changes when the arena's buffer is replaced, which draws naming it must follow.
    /// What reserve_instance_cull worked out this frame, for emit_instance_cull_pass to record:
    /// whether it reserved, whether the recording must be made again, whether the scene's
    /// objects have a kept cut, and the light pool's word.
    bool reserved = false;
    bool reserved_record = false;
    bool reserved_plain_cut = false;
    uint32_t reserved_light_pool_word = kCullNoSlot;
    uint64_t partition_tables = 0;  ///< Which tables the recording uses.
    /// The batches culled together, plain objects: see instance_cull_compute_src. Holds the
    /// clusters chosen, the entries, the items (kPlainItemWords: instance index, slot, table, draw; no table for one
    /// drawn by its own pass that rays see), each item's
    /// object id, each item's next item of the same object, by object slot its first item, and
    /// by item the word of the kept cut record of one drawn by its own pass, 0 for none, written
    /// each frame.
    ArenaRegion together;
    /// Each batch drawn by its own pass that rays see, its first item (in `word`) and its items.
    vector<CutDrawWords::OwnRecords> own_items;
    /// Each batch drawn by its own pass whose clusters (IBatch::get_ray_cull) the culling lays out.
    vector<const IBatch*> own_clusters;
    uint32_t together_items = 0;
    uint32_t together_entries = 0;
    uint32_t together_chosen = 0;  ///< How many records fit.
    uint32_t together_object_slots = 0;  ///< Object slots the first items are by.
    /// Items whose mesh had no ray data yet when they were made (kPlainItemWords): asked again
    /// each frame until it has.
    struct PendingMesh
    {
        IMeshPrimitive::Ptr primitive;
        uint32_t first_item = 0;
        uint32_t count = 0;
    };
    vector<PendingMesh> pending_meshes;
    /// The instanced visuals' items, the last of the items: where they start and how many, and
    /// their descriptors (kItemDescriptorHeaderWords then kItemMeshWords a mesh each), as last
    /// written, in their region, which instance_items_src reads. And each visual's slots when its
    /// items were made, by key: more slots make the items again.
    uint32_t instanced_first_item = 0;
    uint32_t instanced_items = 0;
    vector<uint32_t> item_descriptors;
    uint32_t item_descriptor_count = 0;
    ArenaRegion item_descriptor_region;
    vector<std::pair<const void*, uint32_t>> instanced_slots;
    /// Whether any of the items is moved by a motion function, which rays walking the cut are told.
    bool items_move = false;
    ArenaRegion retest;            ///< What failed the first occlusion test.
    uint32_t retest_capacity = 0;
    /// Where clusters with triangles too small on screen for the hardware go, set by the path
    /// before culling: see kTinyHeaderWords. kCullNoSlot for none.
    uint32_t tiny_word = kCullNoSlot;
    uint32_t tiny_capacity = 0;
    float tiny_pixels = 0.f;  ///< Clusters whose mean triangle is less than this across on screen go to compute.
    uint32_t recorded_tiny_word = kCullNoSlot;  ///< What the recordings use.
    float recorded_tiny_pixels = 0.f;
    bool recorded_whole_cut = false;
    uint32_t pyramid_word = kCullNoSlot;  ///< Which pyramid the recording reads.
    IRenderPass::Ptr retest_pass;
    bool retest_dirty = true;
    /// A partitioned visual's kept cut for this view: see kCutHeaderWords.
    struct Cut
    {
        const void* key = nullptr;  ///< The visual, as InstanceCullInfo::key.
        ArenaRegion region;
        uint32_t partitions = 0;
        uint32_t capacity = 0;      ///< Records the pool holds, kCutPageRecords a page.
        uint32_t node_capacity = 0; ///< Tree nodes the pool holds, kCutPageNodes a page.
        uint64_t made_frame = 0;    ///< The frame it was made, for watching its pool closely at first.
        bool short_pages = false;   ///< Partitions lately asked for pages they did not get.
    };
    vector<Cut> cuts;
    /// The view's quads drawn (kQuadListHeaderWords), filled each frame by the draw pass from
    /// the kept cuts' quad records, and how many it holds.
    ArenaRegion quads;
    uint32_t quad_capacity = 0;
    /// The top tree for rays over all the cuts' partitions (kRayTopHeaderWords), and what its
    /// cuts and leaves were written for.
    ArenaRegion ray_top;
    uint64_t ray_top_layout = 0;
    bool ray_top_fresh = false;  ///< Made this frame for a new layout, its words still to be written.
    /// The top tree's cuts' words, in its order (kRayTopCutWords), for passes over each cut.
    vector<uint32_t> ray_top_cuts;
    /// Visuals whose pools were too small, with the records they wanted: theirs are made
    /// again with room for those.
    vector<std::pair<const void*, uint32_t>> cut_growth;
    uint64_t cut_check_frame = 0;  ///< When the pools' needs were last read back.
    uint64_t cut_log_frame = 0;    ///< When the count of partitions choosing again was last logged.
    uint32_t cut_moves = 0;        ///< What the count was then.
    bool cut_moves_known = false;  ///< Whether that count is comparable with the next.
    /// The light of the kept cuts' records, which the culling frees each frame what no record
    /// names of, and the path gives records the first time it draws them (light_claim_src).
    LightPool light_pool;

    /// The trace of one pixel (kTraceHeaderWords): its region, made when the path asks for one by
    /// setting trace_x, which it is then written with, and when it was last logged.
    ArenaRegion trace;
    uint32_t trace_x = kTraceNone;
    uint32_t trace_y = 0;
    uint32_t trace_entry = kTraceNone;  ///< A cluster entry whose walks are noted wherever they go.
    uint64_t trace_log_frame = 0;

    /// What kept cut records name in the batches, as the batches were last made, so that a cut
    /// outlives batches made again (instance_cut_rename_src): each batch's tables, by their
    /// word, with each mesh's first cluster entry and how many, and the visual for one that has
    /// a kept cut of its own.
    struct BatchTables
    {
        const void* key = nullptr;
        uint32_t table_word = 0;
        vector<uint32_t> first_entry;
        vector<uint32_t> entry_count;
    };
    vector<BatchTables> batch_tables;
    /// The items as last made: whose object each is, which mesh (none for one drawn by its own
    /// pass, an instanced visual's key for its items), which of that object's items with that
    /// mesh, and its batch's tables.
    struct ItemName
    {
        uint32_t object = 0;
        const void* primitive = nullptr;
        uint32_t ordinal = 0;
        uint32_t table_word = 0;
    };
    vector<ItemName> item_names;
    /// Kept cuts whose records name the batches made again this frame, and what each old word
    /// and item becomes, for one frame.
    struct Rename
    {
        const void* key = nullptr;
        vector<uint32_t> pairs;  ///< Old word, new word, sorted by the old.
        vector<uint32_t> items;  ///< By old item, the new one: plain items only.
        /// The instanced visuals' blocks of items that carry over, three words each: the old
        /// first item, how many carry over and the new first item. An item past the plain items
        /// in none of them is of a visual that went.
        vector<uint32_t> blocks;
        uint32_t pairs_word = 0; ///< Where they are in rename_words.
        uint32_t items_word = 0;
        uint32_t blocks_word = 0;
    };
    vector<Rename> renames;
    ArenaRegion rename_words;
    bool renamed = false;  ///< The recording renames, so the next frame records without.
};

/// Words per mesh in a batch's culling table: VELK_BATCH_TABLE (cull_layout.h).
inline constexpr uint32_t kBatchTableWords = kTableEnd;

/**
 * Writes the instanced visuals' items (kPlainItemWords), a thread per item from `first_item` on:
 * its instance's place in the raster region from its visual's slot table (none without a place,
 * or before the table is there), its instance's index there, and by its instance's mesh
 * (params.y) that mesh's draw, ray shape kind and ray data. The visual is found in the descriptors
 * (kItemDescriptorHeaderWords then kItemMeshWords a mesh each), by the items it covers.
 */
inline constexpr string_view instance_items_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct CullInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer CullInstances { CullInstance data[]; } cull_instances;
layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint descriptors_word;
    uint descriptor_count;
    uint items_word;
    uint first_item;
    uint item_count;
    uint objects_word;  // each item's object id, by item
    uint next_word;     // each item's next item of the same object, by item
} pc;

void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.item_count) return;
    uint item = pc.first_item + i;
    uint w = pc.items_word + item * VELK_PLAIN_ITEM_WORDS;

    // Its visual: the descriptor whose items it is among.
    uint found = 0xFFFFFFFFu;
    uint at = pc.descriptors_word;
    for (uint k = 0u; k < pc.descriptor_count && k < 4096u; ++k) {
        uint first = cull_words.data[at + VELK_ITEM_DESCRIPTOR_FIRST];
        uint slots = cull_words.data[at + VELK_ITEM_DESCRIPTOR_SLOTS];
        if (item >= first && item - first < slots) {
            found = at;
            break;
        }
        at += VELK_ITEM_DESCRIPTOR_HEADER_WORDS + min(cull_words.data[at + VELK_ITEM_DESCRIPTOR_MESHES], 4096u) * VELK_ITEM_MESH_WORDS;
    }
    // What does not change with its place: its table, how far it can move, its object, and no
    // next item, since an instanced object has one item.
    uint slot = found != 0xFFFFFFFFu ? item - cull_words.data[found + VELK_ITEM_DESCRIPTOR_FIRST] : 0u;
    cull_words.data[w + VELK_PLAIN_ITEM_TABLE] = found != 0xFFFFFFFFu ? cull_words.data[found + VELK_ITEM_DESCRIPTOR_TABLE] : 0xFFFFFFFFu;
    cull_words.data[w + VELK_PLAIN_ITEM_MOTION] = found != 0xFFFFFFFFu ? cull_words.data[found + VELK_ITEM_DESCRIPTOR_MOTION] : 0u;
    uint object_base = found != 0xFFFFFFFFu ? cull_words.data[found + VELK_ITEM_DESCRIPTOR_OBJECT_BASE] : 0xFFFFFFFFu;
    cull_words.data[pc.objects_word + item] = object_base != 0xFFFFFFFFu ? object_base + slot : 0xFFFFFFFFu;
    cull_words.data[pc.next_word + item] = 0xFFFFFFFFu;
    uint place = 0xFFFFFFFFu;
    if (found != 0xFFFFFFFFu && cull_words.data[found + VELK_ITEM_DESCRIPTOR_SLOT_TABLE] != 0u) {
        place = cull_words.data[cull_words.data[found + VELK_ITEM_DESCRIPTOR_SLOT_TABLE] + slot];
    }
    if (place == 0xFFFFFFFFu) {
        cull_words.data[w + VELK_PLAIN_ITEM_INSTANCE] = 0xFFFFFFFFu;
        cull_words.data[w + VELK_PLAIN_ITEM_SLOT] = 0xFFFFFFFFu;
        cull_words.data[w + VELK_PLAIN_ITEM_DRAW] = 0xFFFFFFFFu;
        cull_words.data[w + VELK_PLAIN_ITEM_KIND] = 255u;
        cull_words.data[w + VELK_PLAIN_ITEM_MESH] = 0xFFFFFFFFu;
        return;
    }
    uint instance = cull_words.data[found + VELK_ITEM_DESCRIPTOR_INSTANCES] + place;
    cull_words.data[w + VELK_PLAIN_ITEM_INSTANCE] = instance;
    cull_words.data[w + VELK_PLAIN_ITEM_SLOT] = place;
    uint mesh = cull_instances.data[instance].params.y;
    if (mesh < cull_words.data[found + VELK_ITEM_DESCRIPTOR_MESHES]) {
        uint m = found + VELK_ITEM_DESCRIPTOR_HEADER_WORDS + mesh * VELK_ITEM_MESH_WORDS;
        cull_words.data[w + VELK_PLAIN_ITEM_DRAW] = cull_words.data[m + VELK_ITEM_MESH_DRAW];
        cull_words.data[w + VELK_PLAIN_ITEM_KIND] = cull_words.data[m + VELK_ITEM_MESH_KIND];
        cull_words.data[w + VELK_PLAIN_ITEM_MESH] = cull_words.data[m + VELK_ITEM_MESH_MESH];
    }
}
)";

/**
 * Names a kept cut's records again after their batches were made again: each record a page of
 * its pool holds for a partition gets its cluster entry, or a whole mesh's table, from `pairs`
 * (old word, new word, sorted by the old), its plain item from `items` (by old item), or its
 * instanced visual's item from `blocks`. A record whose words do not carry over becomes
 * kCutStale. A thread per record the pool holds.
 */
inline constexpr string_view instance_cut_rename_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint cut_word;
    uint pairs_word;
    uint pair_count;
    uint items_word;
    uint item_count;  // the old plain items, which `items` names again one by one
    uint blocks_word; // past them, the instanced visuals' blocks: old first, how many, new first
    uint block_count;
} pc;
)" VELK_CUT_GLSL R"(
// `word` named again, or VELK_CUT_STALE when the pairs do not have it.
uint velk_rename(uint word)
{
    uint lo = 0u;
    uint hi = pc.pair_count;
    while (lo < hi) {
        uint mid = (lo + hi) / 2u;
        uint old = cull_words.data[pc.pairs_word + mid * 2u];
        if (old == word) return cull_words.data[pc.pairs_word + mid * 2u + 1u];
        if (old < word) lo = mid + 1u;
        else hi = mid;
    }
    return VELK_CUT_STALE;
}

void main()
{
    uint cut = pc.cut_word;
    uint place = gl_GlobalInvocationID.x;
    if (place >= cull_words.data[cut + VELK_CUT_CAPACITY]) return;
    // Only the records in use: the page's partition holds this many from its place.
    uint header = cull_words.data[cut + VELK_CUT_PAGE_HEADERS] + (place / VELK_CUT_PAGE_RECORDS) * VELK_CUT_PAGE_WORDS;
    uint owner = cull_words.data[header + VELK_CUT_PAGE_OWNER];
    if (owner >= cull_words.data[cut + VELK_CUT_PARTITIONS]) return;
    uint at = cull_words.data[header + VELK_CUT_PAGE_PLACE] * VELK_CUT_PAGE_RECORDS + place % VELK_CUT_PAGE_RECORDS;
    if (at >= cull_words.data[velk_cut_state(cut, owner) + VELK_PART_COUNT]) return;
    uint record = velk_cut_record_word(cut, place);
    uint entry = cull_words.data[record + VELK_RECORD_ENTRY];
    if (entry == VELK_CUT_QUAD || entry == VELK_CUT_RAY_QUAD || entry == VELK_CUT_STALE) return;
    bool stale = false;
    if (entry == 0xFFFFFFFFu) {
        uint table = cull_words.data[record + VELK_RECORD_MESH_TABLE];
        if (table != 0xFFFFFFFFu) {
            uint renamed = velk_rename(table);
            stale = renamed == VELK_CUT_STALE;
            cull_words.data[record + VELK_RECORD_MESH_TABLE] = renamed;
        }
    } else {
        uint renamed = velk_rename(entry & ~VELK_CUT_LIGHT_ONLY);
        stale = renamed == VELK_CUT_STALE;
        cull_words.data[record + VELK_RECORD_ENTRY] = stale ? VELK_CUT_STALE : renamed | (entry & VELK_CUT_LIGHT_ONLY);
    }
    uint item = cull_words.data[record + VELK_RECORD_SLOT];
    if (item < pc.item_count) {
        uint renamed = cull_words.data[pc.items_word + item];
        stale = stale || renamed == 0xFFFFFFFFu;
        cull_words.data[record + VELK_RECORD_SLOT] = renamed;
    } else if (item != 0xFFFFFFFFu) {
        // An instanced visual's item: where its block went, or stale with its visual gone.
        uint renamed = 0xFFFFFFFFu;
        for (uint b = 0u; b < pc.block_count && b < 4096u; ++b) {
            uint w = pc.blocks_word + b * 3u;
            uint first = cull_words.data[w];
            if (item >= first && item - first < cull_words.data[w + 1u]) {
                renamed = cull_words.data[w + 2u] + item - first;
                break;
            }
        }
        stale = stale || renamed == 0xFFFFFFFFu;
        cull_words.data[record + VELK_RECORD_SLOT] = renamed;
    }
    // What does not carry over: its partition chooses again (instance_cut_stale_src).
    if (stale) cull_words.data[record + VELK_RECORD_ENTRY] = VELK_CUT_STALE;
}
)";

/**
 * After a rename, makes each partition holding a record that did not carry over (kCutStale)
 * choose again this frame: its table entry's chunk word no longer matches, so the decide pass
 * chooses it from nothing, without taking over its old records' light, and has none should no
 * free pages be there for it. A work group per partition, over its records by its page table of
 * the last frame, which the decide pass then reads.
 */
inline constexpr string_view instance_cut_stale_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint cut_word;
} pc;
)" VELK_CUT_GLSL R"(
shared uint s_stale;

void main()
{
    uint cut = pc.cut_word;
    uint index = gl_WorkGroupID.x;
    if (index >= cull_words.data[cut + VELK_CUT_PARTITIONS]) return;
    uint state = velk_cut_state(cut, index);
    uint count = cull_words.data[state + VELK_PART_COUNT];
    if (count == 0u) return;
    if (gl_LocalInvocationIndex == 0u) s_stale = 0u;
    barrier();
    uint first = cull_words.data[state + VELK_PART_TABLE_FIRST];
    uint pages = cull_words.data[cut + VELK_CUT_PAGES];
    // The table that was current last frame, which the begin pass made the last one.
    uint table = velk_cut_table(cut, false) + first;
    if (first + velk_cut_pages(count) <= pages) {
        for (uint r = gl_LocalInvocationIndex; r < count; r += 64u) {
            uint place = velk_cut_place(table, r);
            if (place < cull_words.data[cut + VELK_CUT_CAPACITY]
                && cull_words.data[velk_cut_record_word(cut, place) + VELK_RECORD_ENTRY] == VELK_CUT_STALE) {
                s_stale = 1u;
            }
        }
    }
    barrier();
    if (gl_LocalInvocationIndex == 0u && s_stale != 0u) cull_words.data[state + VELK_PART_CHUNK_FIRST] = 0xFFFFFFFFu;
}
)";

/**
 * Lays out the runs of draws by cluster and fills them, after a phase of culling.
 */
template <class Command>
inline void record_cluster_layout(Command& cmd, InstanceCullPass& state, IGpuPipeline* runs_pipeline,
                                  IGpuPipeline* fill_pipeline, IGpuBuffer* arena_buffer, uint32_t together_word,
                                  uint32_t together_entries_word, bool second_phase)
{
    const bool together = state.together_items != 0;
    if (!together) {
        return;
    }
    struct RunsPushC
    {
        uint32_t entry;
        uint32_t entries_word;
        uint32_t entry_count;
        uint32_t chosen_word;
        uint32_t second_phase;
    };
    const auto record_runs = [&](const RunsPushC& pc, uint32_t groups) {
        DispatchCall dc{};
        dc.pipeline = runs_pipeline;
        dc.groups_x = groups;
        dc.groups_y = 1;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(pc);
        std::memcpy(dc.root_constants, &pc, sizeof(pc));
        cmd.record_dispatch(dc);
    };
    const uint32_t phase = second_phase ? 1u : 0u;
    record_runs({0u, together_entries_word, state.together_entries, together_word, phase}, (state.together_entries + 63u) / 64u);
    cmd.record_global_barrier();
    struct FillPushC
    {
        uint32_t chosen_word;
        uint32_t chosen_capacity;
    };
    const auto record_fill = [&](const FillPushC& pc) {
        DispatchCall dc{};
        dc.pipeline = fill_pipeline;
        dc.groups_x = 1;
        dc.groups_y = 1;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(pc);
        std::memcpy(dc.root_constants, &pc, sizeof(pc));
        cmd.record_dispatch_indirect(dc, arena_buffer, uint64_t(pc.chosen_word + kChosenListGroups) * sizeof(uint32_t));
    };
    record_fill({together_word, state.together_chosen});
    cmd.record_global_barrier();
}

/// What the kept cut of a view's plain objects is found by, among the visuals' (InstanceCullPass::Cut::key).
inline constexpr int kPlainCutKey = 0;

/// The word of @p state's kept cut of the visual @p key, or kCullNoSlot when it has none.
inline uint32_t instance_cut_word(const InstanceCullPass& state, const void* key)
{
    for (const auto& cut : state.cuts) {
        if (cut.key == key) {
            return static_cast<uint32_t>(cut.region.offset() / sizeof(uint32_t));
        }
    }
    return kCullNoSlot;
}

/// The word of @p state's quad list (kQuadListHeaderWords), or kCullNoSlot.
inline uint32_t instance_quad_word(const InstanceCullPass& state)
{
    return state.quads.valid() ? static_cast<uint32_t>(state.quads.offset() / sizeof(uint32_t)) : kCullNoSlot;
}

/// What the top tree over @p state's kept cuts holds, as a hash: its cut (the scene's objects',
/// when @p plain_cut, over @p plain_count partitions), where that cut's items are, and the quad
/// list. Its region is made again when this changes.
inline uint64_t instance_ray_top_layout(const InstanceCullPass& state, bool plain_cut, uint32_t plain_count)
{
    uint64_t layout = 0xcbf29ce484222325ULL;
    if (!plain_cut) {
        return layout;
    }
    uint32_t cut = kCullNoSlot;
    for (const auto& c : state.cuts) {
        if (c.key == &kPlainCutKey) {
            cut = static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t));
        }
    }
    if (cut == kCullNoSlot) {
        return layout;
    }
    const uint32_t together_word = state.together_items != 0 ? static_cast<uint32_t>(state.together.offset() / sizeof(uint32_t)) : 0u;
    const uint32_t items_word = together_word + kCullChosenHeaderWords + state.together_chosen * kCullChosenRecordWords
                              + state.together_entries;
    for (uint32_t v : {cut, 0u, items_word, plain_count, state.items_move ? 1u : 0u, instance_quad_word(state)}) {
        layout = (layout ^ v) * 0x100000001b3ULL;
    }
    return layout;
}

/// The word of @p state's records of the items drawn by their own pass (InstanceCullPass::together),
/// one an item, or kCullNoSlot.
inline uint32_t instance_item_records_word(const InstanceCullPass& state)
{
    if (!state.together.valid() || state.together_items == 0) {
        return kCullNoSlot;
    }
    return static_cast<uint32_t>(state.together.offset() / sizeof(uint32_t)) + kCullChosenHeaderWords
         + state.together_chosen * kCullChosenRecordWords + state.together_entries
         + state.together_items * (kPlainItemWords + 2u) + state.together_object_slots;
}

/// What @p state leaves for the draws its batches' own culling does not list (CutDrawWords).
inline CutDrawWords instance_cut_draw_words(const InstanceCullPass& state)
{
    CutDrawWords words;
    words.quads_word = instance_quad_word(state);
    words.quad_capacity = state.quad_capacity;
    const uint32_t records = instance_item_records_word(state);
    if (records != kCullNoSlot) {
        for (const auto& own : state.own_items) {
            words.own_records.push_back({own.batch, records + own.word, own.count});
        }
    }
    words.own_clusters = state.own_clusters;
    return words;
}

/// The word of @p state's top tree for rays (kRayTopHeaderWords), or kCullNoSlot.
inline uint32_t instance_ray_top_word(const InstanceCullPass& state)
{
    return state.ray_top.valid() ? static_cast<uint32_t>(state.ray_top.offset() / sizeof(uint32_t)) : kCullNoSlot;
}

/// Tells the shaders of the view at @p view_globals_base where its top tree for rays is
/// (FrameGlobals::ray_top). The globals are written again every frame before the render path
/// runs, so this goes after every culling.
inline void publish_ray_top(const InstanceCullPass& state, uint32_t view_globals_base, FrameContext& ctx)
{
    if (!ctx.globals_arena) {
        return;
    }
    const uint32_t word = instance_ray_top_word(state);
    ctx.globals_arena->update_at(uint64_t(view_globals_base) * sizeof(FrameGlobals) + offsetof(FrameGlobals, ray_top), &word,
                                 sizeof(word));
}

/// The pixel VELK_TRACE_PIXEL=x,y names for the culling's trace (InstanceCullPass::trace), a
/// diagnostic logged every 60 frames; @p x is kTraceNone without it.
inline void trace_pixel_from_env(uint32_t& x, uint32_t& y)
{
    x = kTraceNone;
    y = 0;
    unsigned px = 0;
    unsigned py = 0;
#ifdef _WIN32
    char* val = nullptr;
    size_t len = 0;
    if (_dupenv_s(&val, &len, "VELK_TRACE_PIXEL") != 0 || !val) return;
    const bool read = std::sscanf(val, "%u,%u", &px, &py) == 2;
    std::free(val);
#else
    const char* val = std::getenv("VELK_TRACE_PIXEL");
    const bool read = val && std::sscanf(val, "%u,%u", &px, &py) == 2;
#endif
    if (read) {
        x = px;
        y = py;
    }
}

/// The cluster entry VELK_TRACE_ENTRY names, whose walks the trace notes wherever they go, as
/// the trace's log prints entries; kTraceNone without it.
inline uint32_t trace_entry_from_env()
{
    unsigned entry = 0;
#ifdef _WIN32
    char* val = nullptr;
    size_t len = 0;
    if (_dupenv_s(&val, &len, "VELK_TRACE_ENTRY") != 0 || !val) return kTraceNone;
    const bool read = std::sscanf(val, "%u", &entry) == 1;
    std::free(val);
#else
    const char* val = std::getenv("VELK_TRACE_ENTRY");
    const bool read = val && std::sscanf(val, "%u", &entry) == 1;
#endif
    return read ? entry : kTraceNone;
}

/// Tells the culling shaders of the view at @p view_globals_base where its trace is
/// (FrameGlobals::trace, kTraceNone for none). Before the culling runs, after the globals are
/// written for the frame.
inline void publish_trace(const InstanceCullPass& state, uint32_t view_globals_base, FrameContext& ctx)
{
    if (!ctx.globals_arena) {
        return;
    }
    const uint32_t word = state.trace.valid() && state.trace_x != kTraceNone
        ? static_cast<uint32_t>(state.trace.offset() / sizeof(uint32_t))
        : kTraceNone;
    ctx.globals_arena->update_at(uint64_t(view_globals_base) * sizeof(FrameGlobals) + offsetof(FrameGlobals, trace), &word,
                                 sizeof(word));
}

/// How many tree nodes the pool of @p state's kept cut of @p key holds, 0 without one.
inline uint32_t instance_cut_node_capacity(const InstanceCullPass& state, const void* key)
{
    for (const auto& cut : state.cuts) {
        if (cut.key == key) {
            return cut.node_capacity;
        }
    }
    return 0;
}

/// What an item of @p batch says for rays (kPlainItemWords 3 to 5): its draw's index in the draw
/// data, its ray shape kind (a triangle mesh is tested as one; anything else as its instance's
/// rect, unless its visual names an analytic shape of its own), and its mesh's ray data,
/// kInvalidMeshStaticBase until the mesh has it.
inline void batch_ray_words(IBatch& batch, FrameContext& ctx, uint32_t& draw, uint32_t& kind, uint32_t& mesh)
{
    draw = batch.draw_data_region_size() != 0 ? static_cast<uint32_t>(batch.draw_data_region_offset() / sizeof(DrawDataHeader))
                                              : kCullNoSlot;
    auto primitive_ptr = batch.primitive();
    const bool triangles = primitive_ptr && primitive_ptr->get_topology() == MeshTopology::TriangleList
                        && primitive_ptr->get_index_count() != 0;
    kind = triangles ? kRayShapeMesh : kRayShapeRect;
    if (auto* analytic = interface_cast<IAnalyticShape>(batch.shader_source().get())) {
        const uint32_t registered = ctx.snippets ? ctx.snippets->register_intersect(analytic, *ctx.render_ctx) : 0u;
        kind = registered != 0 ? registered : analytic->get_shape_kind();
    }
    mesh = kInvalidMeshStaticBase;
    if (auto* primitive = kind == kRayShapeMesh ? interface_cast<IMeshPrimitiveInternal>(primitive_ptr.get()) : nullptr) {
        mesh = primitive->ensure_rt_data(*ctx.resources);
    }
}

/**
 * @brief Adds @p batches' GPU culling to @p graph, if any of them are culled. Runs
 *        before the passes that draw them. With @p whole_cut, all of each kept cut is drawn, in
 *        view or not, for a view that shows another camera's choice. @p plain_table_word and
 *        @p plain_count are the scene's plain partitions (IBvh::get_plain_partitions): plain
 *        objects get a kept cut over them; without, plain objects are chosen every frame.
 */
/**
 * Everything a view's culling allocates this frame, and the work on the CPU that decides it:
 * called for every view before any view's passes are recorded (IRenderPath::reserve), since
 * growing the culling arena replaces the buffer that recorded passes name.
 * emit_instance_cull_pass then records what this worked out.
 */
inline void reserve_instance_cull(InstanceCullPass& state, const vector<IBatch::Ptr>* batches, FrameContext& ctx,
                                  uint32_t plain_table_word = kCullNoSlot, uint32_t plain_count = 0)
{
    VELK_PERF_SCOPE("instance_cull.reserve");
    state.reserved = false;
    if (!batches || !ctx.resources || !ctx.backend || !ctx.render_ctx) {
        return;
    }

    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) {
        return;
    }

    bool record = false;
    // Last frame's recording named records again; this one does not.
    if (state.renamed) {
        state.renamed = false;
        state.renames.clear();
        record = true;
    }
    // An instanced visual with another number of slots than when its items were made: its
    // items made again.
    for (const auto& batch : *batches) {
        InstanceCullInfo info{};
        if (!batch || !batch->get_instance_cull(info) || !info.instanced) {
            continue;
        }
        const auto known = std::find_if(state.instanced_slots.begin(), state.instanced_slots.end(),
                                        [&](const auto& k) { return k.first == info.key; });
        if (known == state.instanced_slots.end() || known->second != info.slot_table_count) {
            if (!state.dirty) {
                VELK_LOG(I, "instance culling: items made again, a visual's slots %u, were %u", info.slot_table_count,
                         known == state.instanced_slots.end() ? 0u : known->second);
            }
            state.dirty = true;
        }
    }
    const bool batches_changed = state.dirty;
    // What the kept cuts' records named in the batches before they were made again.
    vector<InstanceCullPass::BatchTables> old_tables;
    vector<InstanceCullPass::ItemName> old_items;
    if (state.dirty) {
        VELK_PERF_SCOPE("instance_cull.items");
        state.dirty = false;
        record = true;
        state.count_words.clear();
        state.pending_meshes.clear();
        state.own_items.clear();
        state.own_clusters.clear();
        old_tables = std::move(state.batch_tables);
        old_items = std::move(state.item_names);
        state.batch_tables.clear();
        state.item_names.clear();
        // A batch's tables, read as it wrote them, once each.
        const auto note_tables = [&](const InstanceCullInfo& info) {
            for (const auto& t : state.batch_tables) {
                if (t.table_word == info.table_word) return;
            }
            InstanceCullPass::BatchTables t;
            t.key = info.cull_together ? nullptr : info.key;
            t.table_word = info.table_word;
            for (uint32_t m = 0; m < info.table_count; ++m) {
                uint32_t words[2] = {};
                arena->read_at((uint64_t(info.table_word) + uint64_t(m) * kBatchTableWords + kTableFirstEntry) * sizeof(uint32_t),
                               words, sizeof(words));
                t.first_entry.push_back(words[0]);
                t.entry_count.push_back(words[1]);
            }
            state.batch_tables.push_back(std::move(t));
        };
        vector<uint32_t> items;
        vector<uint32_t> item_objects;  ///< By item: its object id.
        vector<uint32_t> entries;
        uint64_t chosen = 0;
        uint64_t retest_records = 0;
        vector<const void*> counted;
        // A plain item for each instance of @p batch: its index, its slot, its table, its
        // draw, what rays test it as, its mesh's ray data and how far it can move
        // (kPlainItemWords), and its object id, the last word of its ElementInstance.
        const auto add_items = [&](IBatch& batch, uint32_t instances_base, uint32_t slot_count, uint32_t table_word) {
            const array_view<const uint8_t> instances = batch.instance_data();
            const uint32_t stride = batch.instance_stride();
            uint32_t draw;
            uint32_t kind;
            uint32_t mesh;
            batch_ray_words(batch, ctx, draw, kind, mesh);
            if (kind == kRayShapeMesh && mesh == kInvalidMeshStaticBase && slot_count != 0) {
                state.pending_meshes.push_back({batch.primitive(), static_cast<uint32_t>(items.size() / kPlainItemWords), slot_count});
            }
            for (uint32_t slot = 0; slot < slot_count; ++slot) {
                const size_t at = items.size();
                items.resize(at + kPlainItemWords, 0u);
                items[at + kPlainItemInstance] = instances_base + slot;
                items[at + kPlainItemSlot] = slot;
                items[at + kPlainItemTable] = table_word;
                items[at + kPlainItemDraw] = draw;
                items[at + kPlainItemKind] = kind;
                items[at + kPlainItemMesh] = mesh;
                uint32_t object = kCullNoSlot;
                if (stride >= 128u && size_t(slot + 1u) * stride <= instances.size()) {
                    std::memcpy(&object, instances.begin() + size_t(slot) * stride + 124u, sizeof(object));
                }
                item_objects.push_back(object);
                // Drawn by its own pass, its mesh is no part of what its record names.
                state.item_names.push_back({object, table_word != kCullNoSlot ? batch.primitive().get() : nullptr, 0u, table_word});
            }
        };
        // The instanced visuals, each once, whose items come after the plain ones: an item per
        // slot, its object id the visual's base plus the slot.
        struct InstancedVisual
        {
            const void* key;
            uint32_t slots;
            uint32_t object_base;
            uint32_t table_word;
            float displacement;
        };
        vector<InstancedVisual> instanced;
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (!batch) {
                continue;
            }
            if (!batch->get_instance_cull(info)) {
                // Drawn by its own pass, but rays see it, by its clusters when it has them for
                // that (a blended batch): an item with its table for the plain objects' kept cut,
                // whose clusters' draws the culling lays out for its pass (kLodRaysOnly).
                // Only a mesh: an analytic shape (a glyph) keeps its own intersect.
                uint32_t ray_draw;
                uint32_t ray_kind;
                uint32_t ray_mesh;
                batch_ray_words(*batch, ctx, ray_draw, ray_kind, ray_mesh);
                if (plain_table_word != kCullNoSlot && ray_kind == kRayShapeMesh && batch->get_ray_cull(info)) {
                    note_tables(info);
                    chosen += info.cluster_capacity;
                    entries.push_back(info.cluster_entry_word);
                    state.own_clusters.push_back(batch.get());
                    add_items(*batch, info.instances_base, info.slot_count, info.table_word);
                    continue;
                }
                // Else an item without a table.
                if (plain_table_word != kCullNoSlot && batch->instance_stride() == 128u && batch->instance_count() != 0
                    && batch->instance_region_size() != 0) {
                    state.own_items.push_back(
                        {batch.get(), static_cast<uint32_t>(items.size() / kPlainItemWords), batch->instance_count()});
                    add_items(*batch, static_cast<uint32_t>(batch->instance_region_offset() / 128u), batch->instance_count(),
                              kCullNoSlot);
                }
                continue;
            }
            if (std::find(counted.begin(), counted.end(), info.key) == counted.end()) {
                counted.push_back(info.key);
                retest_records += uint64_t(info.slot_count) + info.chosen_capacity + info.cluster_capacity;
            }
            note_tables(info);
            if (info.instanced) {
                // Its clusters go to the one chosen list and are laid out by their entry's draw;
                // a mesh without clusters is drawn from its visible list by its count.
                if (info.cluster_count != 0) {
                    chosen += info.cluster_capacity;
                    entries.push_back(info.cluster_entry_word);
                } else if (std::find(state.count_words.begin(), state.count_words.end(), info.count_word)
                           == state.count_words.end()) {
                    state.count_words.push_back(info.count_word);
                }
                if (std::find_if(instanced.begin(), instanced.end(), [&](const InstancedVisual& v) { return v.key == info.key; })
                    == instanced.end()) {
                    instanced.push_back({info.key, info.slot_table_count, info.object_base, info.table_word, info.max_displacement});
                }
            } else if (info.cull_together) {
                chosen += info.cluster_capacity;
                entries.push_back(info.cluster_entry_word);
                add_items(*batch, info.instances_base, info.slot_count, info.table_word);
            } else if (std::find(state.count_words.begin(), state.count_words.end(), info.count_word)
                       == state.count_words.end()) {
                state.count_words.push_back(info.count_word);
            }
        }
        // The instanced visuals' items, an item per slot after the plain items: instance_items_src
        // writes them whole each frame, with their objects and next words, from the visuals'
        // descriptors, so they are only counted here. The visuals keep their order, new ones
        // after, and each one's block of items is renamed as one.
        const uint32_t old_plain_items = state.instanced_first_item;
        const vector<std::pair<const void*, uint32_t>> old_instanced = std::move(state.instanced_slots);
        vector<std::pair<const void*, uint32_t>> instanced_slots;
        for (const auto& [key, slots] : old_instanced) {
            for (const InstancedVisual& v : instanced) {
                if (v.key == key) instanced_slots.push_back({v.key, v.slots});
            }
        }
        for (const InstancedVisual& v : instanced) {
            const bool known = std::find_if(instanced_slots.begin(), instanced_slots.end(),
                                            [&](const std::pair<const void*, uint32_t>& s) { return s.first == v.key; })
                            != instanced_slots.end();
            if (!known) instanced_slots.push_back({v.key, v.slots});
        }
        state.items_move = false;
        uint32_t instanced_items = 0;
        for (const InstancedVisual& v : instanced) {
            state.items_move = state.items_move || v.displacement > 0.f;
            instanced_items += v.slots;
        }
        state.instanced_slots = std::move(instanced_slots);
        state.instanced_first_item = static_cast<uint32_t>(items.size() / kPlainItemWords);
        state.instanced_items = instanced_items;
        // Each visual still here: its old first item, the slots that carry over, its new first.
        vector<uint32_t> blocks;
        {
            uint32_t old_first = old_plain_items;
            for (const auto& [key, old_slots] : old_instanced) {
                uint32_t new_first = state.instanced_first_item;
                for (const auto& [new_key, new_slots] : state.instanced_slots) {
                    if (new_key == key) {
                        blocks.push_back(old_first);
                        blocks.push_back(std::min(old_slots, new_slots));
                        blocks.push_back(new_first);
                        break;
                    }
                    new_first += new_slots;
                }
                old_first += old_slots;
            }
        }
        // Which of its object's items with that mesh each item is.
        {
            vector<uint32_t> order(state.item_names.size());
            for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
            const auto& names = state.item_names;
            std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                if (names[a].object != names[b].object) return names[a].object < names[b].object;
                if (names[a].primitive != names[b].primitive) return names[a].primitive < names[b].primitive;
                return a < b;
            });
            for (size_t k = 0; k < order.size(); ++k) {
                const bool same = k > 0 && names[order[k]].object == names[order[k - 1]].object
                               && names[order[k]].primitive == names[order[k - 1]].primitive;
                state.item_names[order[k]].ordinal = same ? state.item_names[order[k - 1]].ordinal + 1u : 0u;
            }
        }

        // What each kept cut's records name in the old batches, named in the new: each visual's
        // tables, and the plain items with theirs. A cut whose names do not all carry over is made
        // again below.
        state.renames.clear();
        const auto find_tables = [](const vector<InstanceCullPass::BatchTables>& all, uint32_t word) {
            const InstanceCullPass::BatchTables* found = nullptr;
            for (const auto& t : all) {
                found = t.table_word == word ? &t : found;
            }
            return found;
        };
        const auto pair_tables = [](const InstanceCullPass::BatchTables& from, const InstanceCullPass::BatchTables& to,
                                    vector<std::pair<uint32_t, uint32_t>>& pairs) {
            if (from.first_entry.size() != to.first_entry.size() || from.entry_count != to.entry_count) return false;
            for (uint32_t m = 0; m < from.first_entry.size(); ++m) {
                pairs.push_back({from.table_word + m * kBatchTableWords, to.table_word + m * kBatchTableWords});
                for (uint32_t e = 0; e < from.entry_count[m]; ++e) {
                    pairs.push_back({from.first_entry[m] + e * kClusterEntryWords, to.first_entry[m] + e * kClusterEntryWords});
                }
            }
            return true;
        };
        const auto add_rename = [&](const void* key, vector<std::pair<uint32_t, uint32_t>>& pairs, vector<uint32_t> item_map,
                                    vector<uint32_t> blocks) {
            std::sort(pairs.begin(), pairs.end());
            InstanceCullPass::Rename rename;
            rename.key = key;
            for (const auto& [from, to] : pairs) {
                rename.pairs.push_back(from);
                rename.pairs.push_back(to);
            }
            rename.items = std::move(item_map);
            rename.blocks = std::move(blocks);
            state.renames.push_back(std::move(rename));
        };
        for (const auto& from : old_tables) {
            if (!from.key) continue;
            for (const auto& to : state.batch_tables) {
                vector<std::pair<uint32_t, uint32_t>> pairs;
                if (to.key == from.key && pair_tables(from, to, pairs)) {
                    add_rename(from.key, pairs, {}, {});
                    break;
                }
            }
        }
        // The plain items by name; the instanced visuals' items, while those are as they were, as
        // a block after the plain items, moved by however many more or fewer plain items there
        // are. Instanced visuals that changed make the cut again.
        if (!old_items.empty() || !old_instanced.empty()) {
            // The new items by object, mesh and which of those, to look the old ones up in.
            vector<uint32_t> by_name(state.item_names.size());
            for (uint32_t i = 0; i < by_name.size(); ++i) by_name[i] = i;
            const auto& names = state.item_names;
            const auto less = [](const InstanceCullPass::ItemName& a, const InstanceCullPass::ItemName& b) {
                if (a.object != b.object) return a.object < b.object;
                if (a.primitive != b.primitive) return a.primitive < b.primitive;
                return a.ordinal < b.ordinal;
            };
            std::sort(by_name.begin(), by_name.end(), [&](uint32_t a, uint32_t b) { return less(names[a], names[b]); });
            // What carries over is named again; what does not (an item gone, a table whose entries
            // changed) is left out, and the partitions whose records name it choose again
            // (instance_cut_stale_src).
            vector<uint32_t> item_map(old_items.size(), kCullNoSlot);
            vector<std::pair<uint32_t, uint32_t>> table_pairs;
            vector<uint32_t> unpaired;  ///< Old tables that went to more than one new table.
            for (uint32_t i = 0; i < old_items.size(); ++i) {
                const auto& name = old_items[i];
                const auto* at = std::lower_bound(by_name.begin(), by_name.end(), name,
                                                  [&](uint32_t j, const InstanceCullPass::ItemName& n) { return less(names[j], n); });
                if (name.object == kCullNoSlot || at == by_name.end() || less(name, names[*at])) continue;
                item_map[i] = *at;
                if (name.table_word == kCullNoSlot) continue;
                const uint32_t to = names[*at].table_word;
                const auto* known = std::find_if(table_pairs.begin(), table_pairs.end(),
                                                 [&](const std::pair<uint32_t, uint32_t>& p) { return p.first == name.table_word; });
                if (known == table_pairs.end()) {
                    table_pairs.push_back({name.table_word, to});
                } else if (known->second != to) {
                    unpaired.push_back(name.table_word);
                }
            }
            vector<std::pair<uint32_t, uint32_t>> pairs;
            for (const auto& [from_word, to_word] : table_pairs) {
                if (std::find(unpaired.begin(), unpaired.end(), from_word) != unpaired.end()) continue;
                const auto* from = find_tables(old_tables, from_word);
                const auto* to = find_tables(state.batch_tables, to_word);
                if (from && to) pair_tables(*from, *to, pairs);
            }
            // The instanced visuals' tables, which the block's items name.
            for (const auto& [key, slots] : state.instanced_slots) {
                for (const auto& from : old_tables) {
                    if (from.key != key) continue;
                    const auto* to = std::find_if(state.batch_tables.begin(), state.batch_tables.end(),
                                                  [&](const InstanceCullPass::BatchTables& t) { return t.key == key; });
                    if (to != state.batch_tables.end()) pair_tables(from, *to, pairs);
                }
            }
            add_rename(&kPlainCutKey, pairs, std::move(item_map), std::move(blocks));
        }

        const uint32_t plain_items = static_cast<uint32_t>(items.size() / kPlainItemWords);
        state.together_items = plain_items + state.instanced_items;
        state.together_entries = static_cast<uint32_t>(entries.size());
        state.together_chosen = static_cast<uint32_t>(chosen);
        state.together = {};
        // Which plain items each object has, by the slot of its id: the first, then each one's
        // next. An instanced object is found through its visual's descriptor instead.
        uint32_t object_slots = 0;
        for (uint32_t object : item_objects) {
            if (object != kCullNoSlot) {
                object_slots = std::max(object_slots, (object & kObjectSlotMask) + 1u);
            }
        }
        vector<uint32_t> heads(object_slots, kCullNoSlot);
        vector<uint32_t> nexts(item_objects.size(), kCullNoSlot);
        for (uint32_t item = static_cast<uint32_t>(item_objects.size()); item-- > 0;) {
            const uint32_t object = item_objects[item];
            if (object != kCullNoSlot) {
                nexts[item] = heads[object & kObjectSlotMask];
                heads[object & kObjectSlotMask] = item;
            }
        }
        state.together_object_slots = object_slots;
        if (state.together_items != 0) {
            const uint64_t list_words = kCullChosenHeaderWords + chosen * kCullChosenRecordWords;
            const uint64_t all = state.together_items;
            // Entries, items, their objects and next words, the heads, then the item records,
            // written every frame. The instanced visuals' parts are written by instance_items_src.
            const uint64_t words = list_words + entries.size() + all * (kPlainItemWords + 3u) + heads.size();
            state.together = arena->alloc(words * sizeof(uint32_t));
            if (state.together.valid()) {
                const uint64_t entries_at = state.together.offset() + list_words * sizeof(uint32_t);
                const uint64_t items_at = entries_at + entries.size() * sizeof(uint32_t);
                const uint64_t objects_at = items_at + all * kPlainItemWords * sizeof(uint32_t);
                const uint64_t nexts_at = objects_at + all * sizeof(uint32_t);
                const uint64_t heads_at = nexts_at + all * sizeof(uint32_t);
                arena->write_at(entries_at, entries.data(), entries.size() * sizeof(uint32_t));
                arena->write_at(items_at, items.data(), items.size() * sizeof(uint32_t));
                arena->write_at(objects_at, item_objects.data(), item_objects.size() * sizeof(uint32_t));
                arena->write_at(nexts_at, nexts.data(), nexts.size() * sizeof(uint32_t));
                arena->write_at(heads_at, heads.data(), heads.size() * sizeof(uint32_t));
            } else {
                state.together_items = 0;
                state.together_entries = 0;
            }
        }
        state.retest_capacity = static_cast<uint32_t>(std::min<uint64_t>(retest_records, kCullRetestMaxRecords));
        state.retest = arena->alloc((uint64_t(kCullRetestHeaderWords) + uint64_t(state.retest_capacity) * kCullRetestRecordWords)
                                    * sizeof(uint32_t));
        if (!state.retest.valid()) {
            state.retest_capacity = 0;
        }
    }
    // Meshes whose ray data has come since their items were made.
    if (!state.pending_meshes.empty() && state.together.valid()) {
        const uint64_t items_byte = state.together.offset()
            + (uint64_t(kCullChosenHeaderWords) + uint64_t(state.together_chosen) * kCullChosenRecordWords
               + state.together_entries) * sizeof(uint32_t);
        for (size_t i = 0; i < state.pending_meshes.size();) {
            const auto& pending = state.pending_meshes[i];
            auto* primitive = interface_cast<IMeshPrimitiveInternal>(pending.primitive.get());
            const uint32_t mesh = primitive ? primitive->ensure_rt_data(*ctx.resources) : kInvalidMeshStaticBase;
            if (mesh == kInvalidMeshStaticBase && primitive) {
                ++i;
                continue;
            }
            for (uint32_t k = 0; k < pending.count; ++k) {
                arena->update_at(items_byte + (uint64_t(pending.first_item + k) * kPlainItemWords + kPlainItemMesh) * sizeof(uint32_t),
                                 &mesh, sizeof(mesh));
            }
            state.pending_meshes.erase(state.pending_meshes.begin() + static_cast<ptrdiff_t>(i));
        }
    }
    // The instanced visuals' descriptors, which instance_items_src reads to write their items:
    // made each frame from their batches, since a slot table moves as its visual grows and a
    // mesh's ray data can come late, and written when they change.
    {
        VELK_PERF_SCOPE("instance_cull.descriptors");
        vector<uint32_t> descriptors;
        uint32_t first = state.instanced_first_item;
        for (const auto& [key, slots] : state.instanced_slots) {
            const size_t header = descriptors.size();
            descriptors.resize(header + kItemDescriptorHeaderWords, 0u);
            descriptors[header + kItemDescriptorFirst] = first;
            descriptors[header + kItemDescriptorSlots] = slots;
            uint32_t meshes = 0;
            for (const auto& batch : *batches) {
                InstanceCullInfo info{};
                if (!batch || !batch->get_instance_cull(info) || !info.instanced || info.key != key) {
                    continue;
                }
                descriptors[header + kItemDescriptorInstances] = info.instances_base;
                descriptors[header + kItemDescriptorSlotTable] = info.slot_table_count >= slots ? info.slot_table_word : 0u;
                descriptors[header + kItemDescriptorTable] = info.table_word;
                std::memcpy(&descriptors[header + kItemDescriptorMotion], &info.max_displacement, sizeof(uint32_t));
                descriptors[header + kItemDescriptorObjectBase] = info.object_base != 0u ? info.object_base : kCullNoSlot;
                if (info.mesh_index >= meshes) {
                    descriptors.resize(header + kItemDescriptorHeaderWords + size_t(info.mesh_index + 1u) * kItemMeshWords, 0u);
                    for (uint32_t m = meshes; m <= info.mesh_index; ++m) {
                        uint32_t* words = descriptors.data() + header + kItemDescriptorHeaderWords + size_t(m) * kItemMeshWords;
                        words[kItemMeshDraw] = kCullNoSlot;
                        words[kItemMeshKind] = kRayShapeMesh;
                        words[kItemMeshMesh] = kInvalidMeshStaticBase;
                    }
                    meshes = info.mesh_index + 1u;
                }
                uint32_t* words = descriptors.data() + header + kItemDescriptorHeaderWords + size_t(info.mesh_index) * kItemMeshWords;
                batch_ray_words(*batch, ctx, words[kItemMeshDraw], words[kItemMeshKind], words[kItemMeshMesh]);
            }
            descriptors[header + kItemDescriptorMeshes] = meshes;
            first += slots;
        }
        if (descriptors != state.item_descriptors) {
            const uint64_t bytes = descriptors.size() * sizeof(uint32_t);
            const bool fresh = !state.item_descriptor_region.valid() || state.item_descriptor_region.size() < bytes;
            if (fresh && bytes != 0) {
                state.item_descriptor_region = arena->alloc(bytes * 2u);
                record = true;
            }
            if (state.item_descriptor_region.valid() && bytes != 0) {
                if (fresh) {
                    arena->write_at(state.item_descriptor_region.offset(), descriptors.data(), bytes);
                } else {
                    arena->update_at(state.item_descriptor_region.offset(), descriptors.data(), bytes);
                }
            }
            record = record || state.item_descriptor_count != state.instanced_slots.size();
            state.item_descriptor_count = static_cast<uint32_t>(state.instanced_slots.size());
            state.item_descriptors = std::move(descriptors);
        }
    }

    if (state.count_words.empty() && state.together_items == 0) {
        return;
    }

    // The BVH replaces the scene's table when it fills up.
    uint64_t partition_tables = 0;
    partition_tables = partition_tables * 31u + plain_table_word;
    partition_tables = partition_tables * 31u + plain_count;
    // The scene's objects, plain and instanced, get a kept cut when the scene has partitions.
    const bool plain_cut = plain_table_word != kCullNoSlot && plain_count != 0 && state.together_items != 0;
    const bool tables_changed = partition_tables != state.partition_tables;
    record = record || tables_changed;
    state.partition_tables = partition_tables;

    // Whether a cut's pool was too small lately: every frame for kCutYoungFrames after it is made
    // and while partitions ask for pages they do not get, else now and then, reading the arena
    // being slow. Until it grows, partitions that find no free pages keep their old records.
    constexpr uint64_t kCutCheckFrames = 16;
    constexpr uint64_t kCutYoungFrames = 32;
    if (!state.cuts.empty()) {
        const bool now_and_then = ctx.present_counter >= state.cut_check_frame + kCutCheckFrames;
        if (now_and_then) {
            state.cut_check_frame = ctx.present_counter;
        }
        for (auto& cut : state.cuts) {
            if (!now_and_then && !cut.short_pages && ctx.present_counter >= cut.made_frame + kCutYoungFrames) {
                continue;
            }
            uint32_t short_pages = 0;
            arena->read_at(cut.region.offset() + uint64_t(kCutShortPages) * sizeof(uint32_t), &short_pages, sizeof(short_pages));
            cut.short_pages = short_pages != 0;
            uint32_t wanted_pages = 0;
            arena->read_at(cut.region.offset() + uint64_t(kCutPagesWanted) * sizeof(uint32_t), &wanted_pages, sizeof(wanted_pages));
            const uint32_t wanted = wanted_pages * kCutPageRecords;
            if (wanted > cut.capacity) {
                state.cut_growth.push_back({cut.key, wanted});
            }
        }
    }

    // The scene's partitions get a kept cut for this view, every partition choosing on its first
    // frame. It is made again when the partitions may have been laid out again, so that none
    // keeps records meant for another, or when its pool needs room; else it carries over.
    // (`dirty` also follows the camera, so it alone says nothing about partitions.)
    if (batches_changed || tables_changed || !state.cut_growth.empty()) {
        record = true;
        // What gets a cut: the scene's objects, by their items, whose indices change with the
        // batches.
        struct Wants
        {
            const void* key;
            uint32_t partitions;
            uint32_t members;  ///< Objects, which its partitions' chunks hold.
            bool by_item;
        };
        vector<Wants> wants;
        if (plain_cut) {
            wants.push_back({&kPlainCutKey, plain_count, state.together_items, true});
        }
        vector<InstanceCullPass::Cut> kept;
        vector<const void*> renamed_keys;  ///< Cuts kept across batches made again.
        for (const Wants& info : wants) {
            uint32_t wanted = 0;
            for (const auto& [key, records] : state.cut_growth) {
                wanted = key == info.key ? std::max(wanted, records) : wanted;
            }
            auto old = std::find_if(state.cuts.begin(), state.cuts.end(),
                                    [&](const InstanceCullPass::Cut& c) { return c.key == info.key; });
            // Batches made again keep a cut whose records can all be named in the new ones.
            const bool named = !batches_changed
                            || std::find_if(state.renames.begin(), state.renames.end(),
                                            [&](const InstanceCullPass::Rename& r) { return r.key == info.key; })
                                   != state.renames.end();
            if (!tables_changed && wanted == 0 && named && old != state.cuts.end() && old->partitions == info.partitions) {
                kept.push_back(std::move(*old));
                if (batches_changed) {
                    renamed_keys.push_back(info.key);
                }
                continue;
            }
            // Made again from nothing: every record chooses again and its light starts over.
            if (old != state.cuts.end()) {
                VELK_LOG(I, "kept cut made again:%s%s%s%s", tables_changed ? " tables changed" : "",
                         wanted != 0 ? " out of room" : "", !named ? " its batches' words did not carry over" : "",
                         old->partitions != info.partitions ? " partitions changed" : "");
            }
            InstanceCullPass::Cut cut;
            cut.key = info.key;
            cut.partitions = info.partitions;
            cut.made_frame = ctx.present_counter;
            // The pool: whole pages, room for the records kept, and for the new and the old records
            // of the partitions choosing again at once, with a page a partition for the part-filled
            // last pages of both.
            const uint32_t capacity = std::max({info.members * 2u + 4096u + info.partitions * kCutPageRecords, wanted + wanted / 2u,
                                                old != state.cuts.end() ? old->capacity : 0u});
            const uint32_t pages = (capacity + kCutPageRecords - 1u) / kCutPageRecords;
            cut.capacity = pages * kCutPageRecords;
            cut.node_capacity = pages * kCutPageNodes;
            // Every partition's chunks at most: each one's members, and a part-filled chunk each.
            const uint32_t chunks = (info.members + kCullChunkSlots - 1u) / kCullChunkSlots + info.partitions;
            const uint32_t choose_word = kCutHeaderWords;
            const uint32_t counts_word = choose_word + chunks * kCullChunkListWords;
            const uint32_t state_word = counts_word + chunks;
            // The pages' headers, the free stack and the page marks, then the two page tables.
            const uint32_t headers_word = state_word + info.partitions * kCutPartitionWords;
            const uint32_t stack_word = headers_word + pages * kCutPageWords;
            const uint32_t page_marks_word = stack_word + pages;
            const uint32_t tables_word = page_marks_word + pages / 32u + 1u;
            // The pool's records, ray list entries and tree nodes.
            const uint32_t record_pool = tables_word + pages * 2u;
            const uint32_t ray_pool = record_pool + cut.capacity * kCutRecordWords;
            const uint32_t node_pool = ray_pool + cut.capacity * kCutRayWords;
            // The takeover table: at least two slots a record, so that lookups stay short.
            uint32_t takeover_slots = 1;
            while (takeover_slots < cut.capacity * 2u) takeover_slots <<= 1;
            const uint32_t takeover_word = node_pool + cut.node_capacity * kCutNodeWords;
            // The ray marks, a bit a record of the pool.
            const uint32_t marks_word = takeover_word + takeover_slots * kCutTakeoverWords;
            // The runs the drawing walks, two words each.
            const uint32_t visible_word = marks_word + cut.capacity / 32u + 1u;
            const size_t words = size_t(visible_word) + size_t(cut_visible_runs(cut.capacity, info.partitions)) * 2u;
            cut.region = arena->alloc(words * sizeof(uint32_t));
            if (!cut.region.valid()) {
                VELK_LOG(E, "instance culling: no room for a kept cut of %u records", cut.capacity);
                continue;
            }
            const auto base = static_cast<uint32_t>(cut.region.offset() / sizeof(uint32_t));
            vector<uint32_t> initial;
            initial.resize(tables_word, 0u);
            for (uint32_t args : {kCutChooseGroups, kCutDrawGroups, kCutVisibleGroups}) {
                initial[args + 1u] = 1u;
                initial[args + 2u] = 1u;
            }
            initial[kCutPages] = pages;
            initial[kCutFreePages] = pages;
            initial[kCutCapacity] = cut.capacity;
            initial[kCutRecordPool] = base + record_pool;
            initial[kCutRayPool] = base + ray_pool;
            initial[kCutNodePool] = base + node_pool;
            initial[kCutPageHeaders] = base + headers_word;
            initial[kCutStackWord] = base + stack_word;
            initial[kCutPageMarks] = base + page_marks_word;
            initial[kCutTables] = base + tables_word;
            initial[kCutTables + 1u] = base + tables_word + pages;
            initial[kCutPartitions] = info.partitions;
            initial[kCutChooseWord] = base + choose_word;
            initial[kCutStatesWord] = base + state_word;
            initial[kCutChunkCountsWord] = base + counts_word;
            initial[kCutTakeoverWord] = base + takeover_word;
            initial[kCutTakeoverSlots] = takeover_slots;
            initial[kCutRayMarksWord] = base + marks_word;
            initial[kCutVisibleWord] = base + visible_word;
            // Every page free, the lowest taken first; the marks clear.
            for (uint32_t p = 0; p < pages; ++p) {
                initial[headers_word + p * kCutPageWords + kCutPageOwner] = kCullNoSlot;
                initial[stack_word + p] = pages - 1u - p;
            }
            // Chosen from nowhere, and from a table entry no partition has: all choose.
            constexpr float kFar = 1e30f;
            uint32_t far_bits;
            std::memcpy(&far_bits, &kFar, sizeof(far_bits));
            for (uint32_t p = 0; p < info.partitions; ++p) {
                uint32_t* partition = initial.data() + state_word + p * kCutPartitionWords;
                partition[kPartFrom] = partition[kPartFrom + 1u] = partition[kPartFrom + 2u] = far_bits;
                partition[kPartChunkFirst] = kCullNoSlot;
            }
            arena->write_at(cut.region.offset(), initial.data(), initial.size() * sizeof(uint32_t));
            // The takeover table starts empty: no slot of frame 0, which the begin pass leaves at once.
            vector<uint32_t> empty_table(size_t(takeover_slots) * kCutTakeoverWords, 0u);
            arena->write_at(cut.region.offset() + uint64_t(takeover_word) * sizeof(uint32_t), empty_table.data(),
                            empty_table.size() * sizeof(uint32_t));
            kept.push_back(std::move(cut));
            // The total of the counts is not comparable with the last one any more.
            state.cut_log_frame = ctx.present_counter;
            state.cut_moves_known = false;
        }
        state.cuts = std::move(kept);
        state.cut_growth.clear();

        // The kept cuts' records named in the new batches, by the recording, this frame only.
        vector<InstanceCullPass::Rename> renames;
        uint64_t rename_words = 0;
        for (auto& rename : state.renames) {
            if (std::find(renamed_keys.begin(), renamed_keys.end(), rename.key) != renamed_keys.end()) {
                rename_words += rename.pairs.size() + rename.items.size() + rename.blocks.size();
                renames.push_back(std::move(rename));
            }
        }
        state.renames = std::move(renames);
        state.rename_words = rename_words != 0 ? arena->alloc(rename_words * sizeof(uint32_t)) : ArenaRegion{};
        if (!state.rename_words.valid()) {
            state.renames.clear();
        }
        uint64_t at = state.rename_words.valid() ? state.rename_words.offset() / sizeof(uint32_t) : 0u;
        for (auto& rename : state.renames) {
            rename.pairs_word = static_cast<uint32_t>(at);
            arena->write_at(at * sizeof(uint32_t), rename.pairs.data(), rename.pairs.size() * sizeof(uint32_t));
            at += rename.pairs.size();
            rename.items_word = static_cast<uint32_t>(at);
            arena->write_at(at * sizeof(uint32_t), rename.items.data(), rename.items.size() * sizeof(uint32_t));
            at += rename.items.size();
            rename.blocks_word = static_cast<uint32_t>(at);
            arena->write_at(at * sizeof(uint32_t), rename.blocks.data(), rename.blocks.size() * sizeof(uint32_t));
            at += rename.blocks.size();
        }
        state.renamed = !state.renames.empty();

        // The view's quad list: room for a quad per member of every kept cut, instances and
        // plain objects alike, which no set of quads that each stand for members of their own
        // can pass, and one per partition, whose chunks may be part filled.
        uint32_t quads = 0;
        for (const Wants& info : wants) {
            quads += info.members + info.partitions;
        }
        if (quads != state.quad_capacity) {
            state.quad_capacity = 0;
            // A quad's word, and after them all each one's record.
            state.quads = quads != 0 ? arena->alloc((uint64_t(kQuadListHeaderWords) + uint64_t(quads) * 2u) * sizeof(uint32_t))
                                     : ArenaRegion{};
            if (state.quads.valid()) {
                uint32_t header[kQuadListHeaderWords] = {};
                header[kQuadListVertices] = 4u;
                header[kQuadListDrawCount] = 1u;
                header[kQuadListDraw] = kCullNoSlot;
                arena->write_at(state.quads.offset(), header, sizeof(header));
                state.quad_capacity = quads;
            }
        }
    }
    const auto cut_word = [&](const void* key) {
        for (const auto& c : state.cuts) {
            if (c.key == key) {
                return static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t));
            }
        }
        return kCullNoSlot;
    };

    // How often partitions chose again lately, now and then.
    constexpr uint64_t kCutLogFrames = 120;
    if (!state.cuts.empty() && ctx.present_counter >= state.cut_log_frame + kCutLogFrames) {
        uint32_t moves = 0;
        uint32_t partitions = 0;
        uint32_t records = 0;
        uint32_t light_only = 0;
        uint32_t pages = 0;
        uint32_t pages_used = 0;
        for (const auto& c : state.cuts) {
            uint32_t header[kCutHeaderWords] = {};
            arena->read_at(c.region.offset(), header, sizeof(header));
            moves += header[kCutChoseAgain];
            partitions += c.partitions;
            const uint32_t light = header[kCutLastLightOnlyRecords];
            light_only += light;
            pages += header[kCutPages];
            pages_used += header[kCutPages] - std::min(header[kCutFreePages], header[kCutPages]);
            vector<uint32_t> states(size_t(c.partitions) * kCutPartitionWords);
            arena->read_at(uint64_t(header[kCutStatesWord]) * sizeof(uint32_t), states.data(), states.size() * sizeof(uint32_t));
            for (uint32_t p = 0; p < c.partitions; ++p) records += states[size_t(p) * kCutPartitionWords + kPartCount];
        }
        const auto frames = static_cast<double>(ctx.present_counter - state.cut_log_frame);
        if (state.cut_moves_known) {
            VELK_LOG(I, "kept cut: %u records on %u of %u pages, %u for light only; partitions choosing again: %.1f a frame, "
                        "of %u table entries",
                     records, pages_used, pages, light_only, static_cast<double>(moves - state.cut_moves) / frames, partitions);
            if (state.quads.valid()) {
                uint32_t quads = 0;
                uint32_t in_cut = 0;
                arena->read_at(state.quads.offset() + kQuadListCount * sizeof(uint32_t), &quads, sizeof(quads));
                arena->read_at(state.quads.offset() + kQuadListInCuts * sizeof(uint32_t), &in_cut, sizeof(in_cut));
                VELK_LOG(I, "quads %u in cut, %u in view", in_cut, quads);
            }
        }
        state.cut_moves = moves;
        state.cut_moves_known = true;
        state.cut_log_frame = ctx.present_counter;
    }

    // The trace of one pixel, while the path asks for one: made at once; its notes logged every
    // 60 frames, summed by what they say, then cleared.
    if (state.trace_x != kTraceNone) {
        if (!state.trace.valid()) {
            state.trace = arena->alloc((size_t(kTraceHeaderWords) + size_t(kTraceCapacity) * kTraceNoteWords) * sizeof(uint32_t));
            if (state.trace.valid()) {
                const uint32_t header[kTraceHeaderWords] = {state.trace_x, state.trace_y, 0u, kTraceCapacity, state.trace_entry, 0u};
                arena->update_at(state.trace.offset(), header, sizeof(header));
                state.trace_log_frame = ctx.present_counter;
            }
        } else if (ctx.present_counter >= state.trace_log_frame + 60u) {
            state.trace_log_frame = ctx.present_counter;
            uint32_t count = 0;
            arena->read_at(state.trace.offset() + 2u * sizeof(uint32_t), &count, sizeof(count));
            const uint32_t made = count;
            count = std::min(count, kTraceCapacity);
            vector<uint32_t> notes(size_t(count) * kTraceNoteWords);
            if (count != 0) {
                arena->read_at(state.trace.offset() + kTraceHeaderWords * sizeof(uint32_t), notes.data(),
                               notes.size() * sizeof(uint32_t));
            }
            const uint32_t none = 0;
            arena->update_at(state.trace.offset() + 2u * sizeof(uint32_t), &none, sizeof(none));
            // Summed by what came of what: stage and outcome, then entry and cluster, or partition.
            struct Sum
            {
                uint32_t what, a, b, c, n;
            };
            vector<Sum> sums;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t* n = notes.data() + size_t(i) * kTraceNoteWords;
                auto it = std::find_if(sums.begin(), sums.end(),
                                       [&](const Sum& s) { return s.what == n[0] && s.a == n[1] && s.b == n[2] && s.c == n[3]; });
                if (it == sums.end()) {
                    sums.push_back({n[0], n[1], n[2], n[3], 1u});
                } else {
                    ++it->n;
                }
            }
            std::sort(sums.begin(), sums.end(), [](const Sum& l, const Sum& r) {
                return l.what != r.what ? (l.what & 255u) < (r.what & 255u) || ((l.what & 255u) == (r.what & 255u) && l.what < r.what)
                                        : l.a != r.a ? l.a < r.a : l.b < r.b;
            });
            VELK_LOG(I, "trace at pixel %u,%u: %u notes since the last log%s", state.trace_x, state.trace_y, made,
                     made > kTraceCapacity ? " (only the first kept)" : "");
            static constexpr const char* kChoose[] = {"chosen", "LEFT OUT past its count"};
            static constexpr const char* kPartition[] = {"listed", "OUTSIDE the view", "runs PAST THE ROOM"};
            static constexpr const char* kDraw[] = {"chosen", "OUTSIDE the view", "held back by occlusion", "compute raster",
                                                    "CHOSEN LIST FULL"};
            static constexpr const char* kRaster[] = {"reached the shader", "CUT OUT by its alpha", "written"};
            for (const Sum& s : sums) {
                const uint32_t stage = s.what & 255u;
                const uint32_t outcome = s.what >> 8u;
                if (stage == kTracePartition) {
                    VELK_LOG(I, "  partition %u (%u records): %s, %u times", s.a, s.b, outcome < 3u ? kPartition[outcome] : "?",
                             s.n);
                    continue;
                }
                if (stage == kTraceNumbers) {
                    float stored = 0.f;
                    float pixels = 0.f;
                    std::memcpy(&stored, &s.b, sizeof(stored));
                    std::memcpy(&pixels, &s.c, sizeof(pixels));
                    const uint32_t which = s.a & 0x0F000000u;
                    const uint32_t ref = s.a & ~0x0F000000u;
                    const char* name = which == 0x08000000u ? "least error under"
                                     : which == 0x04000000u ? "largest parent error under"
                                     : which == 0x02000000u ? "own error of cluster"
                                                            : "parent error of cluster";
                    VELK_LOG(I, "  numbers: %s %s %u: stored %g, %.3f px, %u times", name,
                             which >= 0x04000000u ? ((ref & 0x80000000u) != 0u ? "leaf" : "node") : "",
                             ref & 0x7FFFFFFFu, stored, pixels, s.n);
                    continue;
                }
                if (stage == kTraceShape) {
                    VELK_LOG(I, "  shape: %s %u is child %u of node %u", (s.a & 0x80000000u) != 0u ? "cluster" : "node",
                             s.a & 0x7FFFFFFFu, s.c, s.b);
                    continue;
                }
                if (stage == kTraceInstance) {
                    static constexpr const char* kInstance[] = {"walked", "TOO FAR", "OUTSIDE the view", "held back by occlusion"};
                    uint32_t triangles = 0;
                    for (const auto& batch : *batches) {
                        InstanceCullInfo info{};
                        if (batch && batch->get_instance_cull(info) && info.cluster_entry_word == s.a) {
                            if (auto* primitive = batch->primitive().get()) triangles = primitive->get_index_count() / 3u;
                            break;
                        }
                    }
                    VELK_LOG(I, "  instance of entry %u (%u triangles, %u entries) slot %u, culled every frame: %s, %u times", s.a,
                             triangles, s.c, s.b, outcome < 4u ? kInstance[outcome] : "?", s.n);
                    continue;
                }
                if (stage == kTraceBlended) {
                    uint32_t triangles = 0;
                    for (const auto& batch : *batches) {
                        if (batch && batch->draw_data_region_offset() / sizeof(DrawDataHeader) == s.a) {
                            if (auto* primitive = batch->primitive().get()) triangles = primitive->get_index_count() / 3u;
                            break;
                        }
                    }
                    float opacity = 0.f;
                    std::memcpy(&opacity, &s.c, sizeof(opacity));
                    VELK_LOG(I, "  blended draw %u (%u triangles) ids %08x drawn over it with opacity %.3f, %u times", s.a,
                             triangles, s.b, opacity, s.n);
                    continue;
                }
                if (stage == kTraceResolve) {
                    VELK_LOG(I, "  resolve: took the %s raster's ids %08x %08x (draw %d), %u times",
                             outcome == 0u ? "hardware" : "compute", s.a, s.b,
                             s.a == 0u ? -1 : int((s.a & 0x007FFFFFu) - 1u), s.n);
                    continue;
                }
                if (stage == kTraceForward) {
                    uint32_t triangles = 0;
                    bool blended = false;
                    for (const auto& batch : *batches) {
                        if (batch && batch->draw_data_region_offset() / sizeof(DrawDataHeader) == s.a) {
                            if (auto* primitive = batch->primitive().get()) triangles = primitive->get_index_count() / 3u;
                            blended = batch->pipeline_options().blend_mode == BlendMode::Alpha;
                            break;
                        }
                    }
                    float depth = 0.f;
                    std::memcpy(&depth, &s.c, sizeof(depth));
                    VELK_LOG(I, "  forward draw %u (%u triangles%s) ids %08x depth %.6f: drawn, %u times", s.a, triangles,
                             blended ? ", blended" : "", s.b, depth, s.n);
                    continue;
                }
                if (stage == kTraceRaster) {
                    // The batch whose draw record it is: its triangles, and whether blended.
                    uint32_t triangles = 0;
                    for (const auto& batch : *batches) {
                        if (batch && batch->draw_data_region_offset() / sizeof(DrawDataHeader) == s.a) {
                            if (auto* primitive = batch->primitive().get()) triangles = primitive->get_index_count() / 3u;
                            break;
                        }
                    }
                    float depth = 0.f;
                    std::memcpy(&depth, &s.c, sizeof(depth));
                    VELK_LOG(I, "  raster draw %u (%u triangles) ids %08x depth %.6f: %s, %u times", s.a, triangles, s.b, depth,
                             outcome < 3u ? kRaster[outcome] : "?", s.n);
                    continue;
                }
                // The batch whose entry it is: its triangles, and whether blended.
                uint32_t triangles = 0;
                bool blended = false;
                uint64_t draw = 0;
                for (const auto& batch : *batches) {
                    InstanceCullInfo info{};
                    if (!batch || !(batch->get_instance_cull(info) || batch->get_ray_cull(info))
                        || info.cluster_entry_word != s.a) {
                        continue;
                    }
                    if (auto* primitive = batch->primitive().get()) triangles = primitive->get_index_count() / 3u;
                    blended = batch->pipeline_options().blend_mode == BlendMode::Alpha;
                    draw = batch->draw_data_region_offset() / sizeof(DrawDataHeader);
                    break;
                }
                static constexpr const char* kWalk[] = {"?", "?", "chosen", "for light only", "skipped, all under its node too coarse",
                                                        "skipped, all under its node have a parent that will do",
                                                        "too coarse", "its parent will do",
                                                        "NODE skipped, all under it too coarse",
                                                        "NODE skipped, all under it have a parent that will do",
                                                        "NODE skipped, out of view, all under it have a parent at the light's level",
                                                        "chosen as a root record"};
                const char* how = stage == kTraceChoose ? (outcome < 2u ? kChoose[outcome] : "?")
                                : stage == kTraceDraw  ? (outcome < 5u ? kDraw[outcome] : "?")
                                : stage == kTraceWalk  ? (outcome < 12u ? kWalk[outcome] : "?")
                                                       : "?";
                if (stage == kTraceWalk) {
                    VELK_LOG(I, "  walk entry %u (draw %llu, %u triangles) %s %u, partition state %u: %s, %u times", s.a,
                             static_cast<unsigned long long>(draw), triangles, (s.b & 0x40000000u) != 0u ? "node" : "cluster",
                             s.b & ~0x40000000u, s.c, how, s.n);
                    continue;
                }
                VELK_LOG(I, "  %s entry %u (draw %llu, %u triangles%s) cluster %u: %s, %u times",
                         stage == kTraceChoose ? "choose" : stage == kTraceDraw ? "draw" : "?", s.a,
                         static_cast<unsigned long long>(draw), triangles, blended ? ", blended" : "", s.b, how, s.n);
            }
        }
    }

    // The light pool, with a segment to start, and more when records asked for pages that were
    // not there. Its pages are swept every frame, so a new segment's are free on the next.
    if (!state.cuts.empty() && ensure_light_pool(state.light_pool, *arena, ctx.backend->device_memory_bytes(), ctx.backend->compute_fill_threads())) {
        LightPool& pool = state.light_pool;
        // Its budget in rays, by what the view's trace took: its passes' GPU time, the one values'
        // and the points'.
        const char* trace_passes[2] = {state.chosen_by_other ? "other:deferred.light_trace" : "deferred.light_trace",
                                       state.chosen_by_other ? "other:deferred.light_points" : "deferred.light_points"};
        float trace_ms = 0.f;
        for (const auto& t : ctx.backend->last_gpu_timings()) {
            if (std::strcmp(t.label, trace_passes[0]) == 0 || std::strcmp(t.label, trace_passes[1]) == 0) {
                trace_ms += t.ms;
            }
        }
        adjust_light_rays(pool, *arena, trace_ms, state.light_trace_ms);
        if (pool.segments.empty()) {
            grow_light_pool(pool, *arena, 1u);
            VELK_LOG(I, "light pool: the trace fills the device at %u threads", pool.fill);
        } else if (ctx.present_counter >= pool.check_frame + 16u) {
            pool.check_frame = ctx.present_counter;
            // The most any frame since the last look asked for and did not get.
            const uint64_t wanted_at = pool.header.offset() + uint64_t(kLightPoolMostPagesWanted) * sizeof(uint32_t);
            uint32_t wanted = 0;
            arena->read_at(wanted_at, &wanted, sizeof(wanted));
            if (wanted != 0) {
                const uint32_t none = 0;
                arena->update_at(wanted_at, &none, sizeof(none));
            }
            if (wanted != 0 && pool.segments.size() < pool.max_segments) {
                uint32_t free_pages = 0;
                arena->read_at(pool.header.offset() + uint64_t(kLightPoolFreePages) * sizeof(uint32_t), &free_pages,
                               sizeof(free_pages));
                const uint32_t added = grow_light_pool(pool, *arena, (wanted + kLightSegmentPages - 1u) / kLightSegmentPages);
                VELK_LOG(I, "light pool +%u to %u pg: %u pages asked for were not there, %u free at the last sweep", added,
                         pool.pages(), wanted, free_pages);
            }
        }
        // What it holds, now and then, from the last frame's complete counts.
        if (pool.header.valid() && ctx.present_counter >= pool.log_frame + 30u) {
            pool.log_frame = ctx.present_counter;
            // The pool's header as it is now, by its words' names; the summed ones cleared after.
            uint32_t h[kLightPoolHeaderWords] = {};
            arena->read_at(pool.header.offset(), h, sizeof(h));
            const auto clear = [&](uint32_t word, uint32_t count) {
                const uint32_t zeros[16] = {};
                arena->update_at(pool.header.offset() + uint64_t(word) * sizeof(uint32_t), zeros, count * sizeof(uint32_t));
            };
            uint32_t written = 0;
            uint32_t took = 0;
            for (const auto& c : state.cuts) {
                uint32_t words[2] = {};
                arena->read_at(c.region.offset() + uint64_t(kCutLastRewritten) * sizeof(uint32_t), words, sizeof(words));
                written += words[0];
                took += words[1];
            }
            clear(kLightPoolSumChanged, 1u);
            VELK_LOG(I, "light: %u pages, %u of them free; records %u paged (%u linked), %u one value; last frame %u searched, %u one values "
                        "traced, points listed: %u untraced, %u stale coarse, %u stale fine, %u warming up; "
                        "%u records changed since the last log, %u records "
                        "chosen again of which %u kept their light, %u lit pixels untraced; budget %u rays",
                     h[kLightPoolPages], h[kLightPoolFreePages], h[kLightPoolLastRecordsPaged], h[kLightPoolLastRecordsLinked],
                     h[kLightPoolLastRecordsOneValue], h[kLightPoolLastSearched], h[kLightPoolLastTraces],
                     h[kLightPoolLastUntracedPoints], h[kLightPoolLastCoarsePoints], h[kLightPoolLastFinePoints],
                     h[kLightPoolLastWarmupPoints],
                     h[kLightPoolSumChanged], written, took, h[kLightPoolLastUntracedPixels], h[kLightPoolRayBudget]);
            uint32_t longest = 0;
            uint32_t most = 0;
            for (const auto& c : state.cuts) {
                uint32_t words[2] = {};
                arena->read_at(c.region.offset() + uint64_t(kCutLastLongestWalk) * sizeof(uint32_t), words, sizeof(words));
                longest = std::max(longest, words[0]);
                most = std::max(most, words[1]);
            }
            VELK_LOG(I, "choosing: last frame the longest walk of one thread visited %u hierarchy nodes, the most "
                        "records one walk chose %u",
                     longest, most);
            // The light trace's worst frame since the last log: its rays, and their walks' steps.
            clear(kLightPoolWorstRays, kLightPoolWorstFirstRays + 1u - kLightPoolWorstRays);
            const uint32_t worst_rays = h[kLightPoolWorstRays];
            const uint32_t worst_steps = h[kLightPoolWorstSteps];
            clear(kLightPoolLongestWalk, 1u);
            VELK_LOG(I, "light trace: worst frame %u rays (%u for first traces), %u walk steps, %.0f steps a ray; longest walk "
                        "of one thread %u steps; GPU %.2f ms the last frame, %.2f ms at most",
                     worst_rays, h[kLightPoolWorstFirstRays], worst_steps,
                     worst_rays != 0 ? double(worst_steps) / double(worst_rays) : 0.0, h[kLightPoolLongestWalk],
                     double(pool.trace_ms), double(pool.most_trace_ms));
            pool.most_trace_ms = 0.f;
            // Since the last log: what a ray's walk tests, and which own points were traced.
            clear(kLightPoolSumTriangles, kLightPoolSumLaterTraces + 1u - kLightPoolSumTriangles);
            const double rays = h[kLightPoolSumRays] != 0 ? double(h[kLightPoolSumRays]) : 1.0;
            const double steps = double(h[kLightPoolSumSteps]) * 16.0;
            const double tris = double(h[kLightPoolSumTriangles]) * 16.0;
            const double tested = double(h[kLightPoolSumRecords]);
            clear(kLightPoolSumRecordsHit, 7u);
            VELK_LOG(I, "light trace: a ray tests %.0f triangles, %.0f records (%.1f through their box: %.1f patches, %.1f quads, "
                        "%.1f without an instance, %.1f clusters, %.1f whole meshes, %.1f meshes without a tree), %.0f nodes; "
                        "points traced %u first, %u later",
                     tris / rays, tested / rays, double(h[kLightPoolSumRecordsHit]) / rays, double(h[kLightPoolSumHitBy]) / rays,
                     double(h[kLightPoolSumHitBy + 1u]) / rays, double(h[kLightPoolSumHitBy + 2u]) / rays,
                     double(h[kLightPoolSumHitBy + 3u]) / rays, double(h[kLightPoolSumHitBy + 4u]) / rays,
                     double(h[kLightPoolSumHitBy + 5u]) / rays, std::max(steps - tris - tested, 0.0) / rays,
                     h[kLightPoolSumFirstTraces], h[kLightPoolSumLaterTraces]);
            clear(kLightPoolSumMeshWide, 8u);
            VELK_LOG(I, "light trace: whole meshes a ray walks by box size %.1f under 1 unit, %.1f under 4, %.1f under 16, %.1f "
                        "more; by triangles %.1f under 64, %.1f under 512, %.1f under 4096, %.1f more",
                     double(h[kLightPoolSumMeshWide]) / rays, double(h[kLightPoolSumMeshWide + 1u]) / rays,
                     double(h[kLightPoolSumMeshWide + 2u]) / rays, double(h[kLightPoolSumMeshWide + 3u]) / rays,
                     double(h[kLightPoolSumMeshTris]) / rays, double(h[kLightPoolSumMeshTris + 1u]) / rays,
                     double(h[kLightPoolSumMeshTris + 2u]) / rays, double(h[kLightPoolSumMeshTris + 3u]) / rays);
            // Since the last log: where traces come from, a change or the view.
            clear(kLightPoolSumRetraces, kLightPoolSumSplitsRefused + 1u - kLightPoolSumRetraces);
            VELK_LOG(I, "light causes: %u points traced again from none after a change; points laid out for records without light %u, "
                        "for records grown from one value %u, for records for light only %u",
                     h[kLightPoolSumRetraces], h[kLightPoolSumLaidNew], h[kLightPoolSumLaidFromOne], h[kLightPoolSumLaidLightOnly]);
            // Since the last log: records whose lights were chosen, and of them those that left out
            // a light at least a tenth, or a quarter, as strong as the strongest they took.
            // Since the last log, in every 16th frame: points listed by what listed them.
            clear(kLightPoolSumListedBy, 4u);
            VELK_LOG(I, "light listed (every 16th frame): %u points by records without pieces, by triangles in pieces %u under 8 "
                        "px on screen, %u under 64, %u larger",
                     h[kLightPoolSumListedBy], h[kLightPoolSumListedBy + 1u], h[kLightPoolSumListedBy + 2u],
                     h[kLightPoolSumListedBy + 3u]);
            clear(kLightPoolSumSlotChoices, 3u);
            VELK_LOG(I, "light slots: %u records chose their lights, %u left out one a tenth as strong as their strongest, %u "
                        "one a quarter as strong",
                     h[kLightPoolSumSlotChoices], h[kLightPoolSumLeftOutTenth], h[kLightPoolSumLeftOutQuarter]);
            // Pieces: those kept at the last frame's mark and the deepest of them, and since the
            // last log those made, freed and asked for but not made.
            VELK_LOG(I, "light pieces: %u kept, the deepest at %u; last frame %u asks; since the last log %u made, %u freed, "
                        "%u asks not made",
                     h[kLightPoolLastMarkPieces], h[kLightPoolLastMarkDeepest], h[kLightPoolLastSplits], h[kLightPoolSumPiecesMade],
                     h[kLightPoolSumPiecesFreed], h[kLightPoolSumSplitsRefused]);
            // What the culling arena holds: in use of its buffer, and this view's part of it.
            uint64_t cut_bytes = 0;
            for (const auto& c : state.cuts) cut_bytes += c.region.size();
            uint64_t pool_bytes = pool.header.size();
            for (const auto& s : pool.segments) pool_bytes += s.size();
            const uint64_t view_other = state.together.size() + state.item_descriptor_region.size() + state.retest.size()
                                      + state.quads.size() + state.ray_top.size() + state.rename_words.size();
            VELK_LOG(I, "culling arena: %.1f of %.1f MB in use; this view's light pool %.1f MB, cut stores %.1f MB, other "
                        "%.1f MB",
                     double(arena->capacity() - arena->free_bytes()) / 1e6, double(arena->capacity()) / 1e6,
                     double(pool_bytes) / 1e6, double(cut_bytes) / 1e6, double(view_other) / 1e6);
            // The ray lists: records rays trace against all records, from the partitions' states.
            uint32_t traced_records = 0;
            uint32_t all_records = 0;
            uint32_t most_traced = 0;
            uint32_t unsorted = 0;
            uint32_t balanced = 0;
            uint32_t tried_most = 0;
            uint32_t limit_least = 0xFFFFFFFFu;
            for (const auto& c : state.cuts) {
                uint32_t header[kCutHeaderWords] = {};
                arena->read_at(c.region.offset(), header, sizeof(header));
                vector<uint32_t> states(size_t(c.partitions) * kCutPartitionWords);
                arena->read_at(uint64_t(header[kCutStatesWord]) * sizeof(uint32_t), states.data(), states.size() * sizeof(uint32_t));
                for (uint32_t p = 0; p < c.partitions; ++p) {
                    const uint32_t ray_count = states[size_t(p) * kCutPartitionWords + kPartRayCount];
                    traced_records += ray_count;
                    all_records += states[size_t(p) * kCutPartitionWords + kPartCount];
                    most_traced = std::max(most_traced, ray_count);
                    unsorted += ray_count > kRaySortMax ? 1u : 0u;
                    const uint32_t tried = states[size_t(p) * kCutPartitionWords + kPartBalanced];
                    if (ray_count > 4u && tried != 0u) {
                        ++balanced;
                        tried_most = std::max(tried_most, tried);
                        // As velk_lbvh_build_bounded: a balanced tree's height and 16 more.
                        const uint32_t leaves = (ray_count + 3u) / 4u;
                        uint32_t bits = 0;
                        while (leaves > 1u && (1u << bits) < leaves) ++bits;
                        limit_least = std::min(limit_least, bits + 16u);
                    }
                }
            }
            VELK_LOG(I, "ray lists: %u of %u records; at most %u in a partition, %u partitions past %u sorted in their pages, "
                        "%u trees balanced for being too deep sorted (sorted heights up to %u, limits from %u)",
                     traced_records, all_records, most_traced, unsorted, kRaySortMax, balanced, tried_most,
                     balanced != 0u ? limit_least : 0u);
            // The deepest the walk's stack got since the last log, in the trace and the probes.
            clear(kLightPoolDeepestTrace, 2u);
            VELK_LOG(I, "walk stack: deepest %u in the light trace, %u in the probes", h[kLightPoolDeepestTrace],
                     h[kLightPoolDeepestProbes]);
            // Where a ray's walk spends its steps, since the last log (the same rays as above).
            clear(kLightPoolSumTopNodes, kLightPoolSumPartitions + 1u - kLightPoolSumTopNodes);
            VELK_LOG(I, "light trace: a ray walks %.0f top tree nodes, enters %.1f partitions, walks %.0f of their tree nodes "
                        "and %.0f mesh tree nodes",
                     double(h[kLightPoolSumTopNodes]) * 16.0 / rays, double(h[kLightPoolSumPartitions]) / rays,
                     double(h[kLightPoolSumPartitionNodes]) * 16.0 / rays, double(h[kLightPoolSumMeshNodes]) * 16.0 / rays);
            // The top tree's partitions: how many have records for rays, and how much their boxes
            // overlap, as the sum of their volumes over the volume of the box around them all.
            if (state.ray_top.valid()) {
                vector<uint32_t> top(size_t(state.ray_top.size() / sizeof(uint32_t)));
                arena->read_at(state.ray_top.offset(), top.data(), top.size() * sizeof(uint32_t));
                const uint32_t cut_count = top[0];
                const uint32_t leaves = top[1];
                const uint32_t node_count = top[2];
                const size_t nodes_at = kRayTopHeaderWords + size_t(cut_count) * kRayTopCutWords + size_t(leaves) * kRayTopLeafWords;
                double volume = 0.0;
                double lo[3] = {1e30, 1e30, 1e30};
                double hi[3] = {-1e30, -1e30, -1e30};
                uint32_t boxes = 0;
                for (uint32_t n = 0; n < node_count && nodes_at + (size_t(n) + 1u) * kCutNodeWords <= top.size(); ++n) {
                    const uint32_t* node = top.data() + nodes_at + size_t(n) * kCutNodeWords;
                    for (uint32_t k = 0; k < 2; ++k) {
                        if ((node[12u + k] & kCutLeaf) == 0 || node[12u + k] == 0xFFFFFFFFu) continue;
                        float b[6];
                        std::memcpy(b, node + k * 6u, sizeof(b));
                        if (b[0] > b[3] || b[1] > b[4] || b[2] > b[5]) continue;
                        volume += double(b[3] - b[0]) * double(b[4] - b[1]) * double(b[5] - b[2]);
                        for (int a = 0; a < 3; ++a) {
                            lo[a] = std::min(lo[a], double(b[a]));
                            hi[a] = std::max(hi[a], double(b[3 + a]));
                        }
                        ++boxes;
                    }
                }
                const double around = boxes != 0 ? (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]) : 0.0;
                VELK_LOG(I, "ray top tree: %u partitions with records for rays of %u, their boxes' volumes %.1f times the "
                            "box around them all (%.0f by %.0f by %.0f)",
                         boxes, leaves, around > 0.0 ? volume / around : 0.0, hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
            }
        }
    }
    record = record || state.light_pool.pages() != state.light_pool.recorded_pages;
    state.light_pool.recorded_pages = state.light_pool.pages();

    // The top tree over the kept cuts rays walk: when what it holds changes, a new region with
    // room for its header, its cuts, its leaves and its nodes, which emit_instance_cull_pass
    // writes. A new one, since earlier frames may still read the old.
    const uint64_t ray_top_layout = instance_ray_top_layout(state, plain_cut, plain_count);
    if (!plain_cut) {
        state.ray_top = {};
        state.ray_top_layout = 0;
        state.ray_top_fresh = false;
    } else if (!state.ray_top.valid() || ray_top_layout != state.ray_top_layout) {
        const uint32_t cut_count = plain_cut ? 1u : 0u;
        const uint32_t leaves = plain_cut ? plain_count : 0u;
        const uint64_t words = uint64_t(kRayTopHeaderWords) + uint64_t(cut_count) * kRayTopCutWords + uint64_t(leaves) * kRayTopLeafWords
                             + uint64_t(top_tree_nodes(leaves)) * kCutNodeWords;
        state.ray_top = arena->alloc(words * sizeof(uint32_t));
        state.ray_top_layout = ray_top_layout;
        state.ray_top_fresh = state.ray_top.valid();
        // The passes name its word.
        record = true;
    }

    state.reserved = true;
    state.reserved_record = record;
    state.reserved_plain_cut = plain_cut;
    state.reserved_light_pool_word = state.light_pool.segments.empty() ? kCullNoSlot : state.light_pool.word();
}

/**
 * Records a view's culling as reserve_instance_cull worked it out this frame; nothing when it
 * did not. Allocates nothing, so the culling arena's buffer it names stays the frame's.
 */
inline void emit_instance_cull_pass(InstanceCullPass& state, const vector<IBatch::Ptr>* batches,
                                    uint32_t view_globals_base, FrameContext& ctx, IRenderGraph& graph,
                                    uint32_t pyramid_word = kCullNoSlot, bool whole_cut = false,
                                    uint32_t plain_table_word = kCullNoSlot, uint32_t plain_count = 0)
{
    VELK_PERF_SCOPE("instance_cull.emit");
    if (!state.reserved || !batches || !ctx.resources || !ctx.backend || !ctx.render_ctx) {
        return;
    }
    state.reserved = false;
    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) {
        return;
    }
    bool record = state.reserved_record;
    const bool plain_cut = state.reserved_plain_cut;
    const uint32_t light_pool_word = state.reserved_light_pool_word;
    const auto cut_word = [&](const void* key) {
        for (const auto& c : state.cuts) {
            if (c.key == key) {
                return static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t));
            }
        }
        return kCullNoSlot;
    };

    IGpuBuffer* arena_buffer = arena->buffer();
    if (!arena_buffer) {
        return;
    }
    // The copies below name the arena's buffer, which growing it replaces.
    // A grown arena's new buffer can have the old one's address, so its size counts too.
    if (arena_buffer != state.arena_buffer || arena_buffer->size_bytes() != state.arena_size) {
        record = true;
        ++state.arena_generation;
    }
    state.arena_buffer = arena_buffer;
    state.arena_size = arena_buffer->size_bytes();
    if (state.retest_capacity == 0) {
        pyramid_word = kCullNoSlot;
    }
    record = record || pyramid_word != state.pyramid_word;
    state.pyramid_word = pyramid_word;
    if (state.tiny_word == kCullNoSlot || state.tiny_capacity == 0) {
        state.tiny_word = kCullNoSlot;
        state.tiny_pixels = 0.f;
    }
    record = record || state.tiny_word != state.recorded_tiny_word || state.tiny_pixels != state.recorded_tiny_pixels
          || whole_cut != state.recorded_whole_cut;
    state.recorded_whole_cut = whole_cut;
    state.recorded_tiny_word = state.tiny_word;
    state.recorded_tiny_pixels = state.tiny_pixels;
    state.retest_dirty = state.retest_dirty || record;

    // A mesh's count of visible slots, and at 7 words on, those the second phase added.
    const uint32_t zero = 0;
    for (uint32_t word : state.count_words) {
        arena->update_at(uint64_t(word) * sizeof(uint32_t), &zero, sizeof(zero));
        arena->update_at(uint64_t(word + 7u) * sizeof(uint32_t), &zero, sizeof(zero));
    }

    // All the stages are recorded together, each into its own pass.
    for (const auto& pass : state.passes) record = record || !pass;
    CachedPassRecording stage_recs[kCullStages];
    const auto record_all = [&]() {
        CachedPassRecording& rec = stage_recs[kCullStages - 1u];
        const auto pipeline_for = [&](string_view src, uint64_t key) {
            auto found = ctx.render_ctx->pipelines().find(PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0});
            return found ? found : ctx.render_ctx->pipelines().compile_compute(src, key);
        };
        auto pipeline = pipeline_for(instance_cull_compute_src, 0x4375'6c6c'496e'7374ULL);
        auto partition_pipeline = pipeline_for(instance_partition_cull_src, 0x4375'6c6c'5061'7274ULL);
        auto runs_pipeline = pipeline_for(instance_cluster_runs_src, 0x4375'6c6c'5275'6e73ULL);
        auto fill_pipeline = pipeline_for(instance_cluster_fill_src, 0x4375'6c6c'4669'6c6cULL);
        auto begin_pipeline = pipeline_for(instance_cut_begin_src, 0x4375'7442'6567'696eULL);
        auto pages_pipeline = pipeline_for(instance_cut_pages_src, 0x4375'7450'6167'6573ULL);
        auto choose_pipeline = pipeline_for(instance_cut_choose_src, 0x4375'7443'686f'6f73ULL);
        auto place_pipeline = pipeline_for(instance_cut_place_src, 0x4375'7450'6c61'6365ULL);
        auto draw_pipeline = pipeline_for(instance_cut_draw_src, 0x4375'7444'7261'7720ULL);
        auto visible_pipeline = pipeline_for(instance_cut_visible_src, 0x4375'7456'6973'6962ULL);
        auto tree_pipeline = pipeline_for(instance_cut_tree_src, 0x4375'7454'7265'6520ULL);
        auto top_pipeline = pipeline_for(instance_cut_top_src, 0x4375'7454'6f70'2020ULL);
        auto mark_pipeline = pipeline_for(light_mark_src, 0x4c69'6768'744d'726bULL);
        auto sweep_pipeline = pipeline_for(light_sweep_src, 0x4c69'6768'7453'7770ULL);
        auto pieces_pipeline = pipeline_for(light_pieces_src, 0x4c69'6768'7450'6363ULL);
        auto rename_pipeline = pipeline_for(instance_cut_rename_src, 0x4375'7452'656e'616dULL);
        auto items_pipeline = pipeline_for(instance_items_src, 0x4375'6c6c'4974'656dULL);
        auto stale_pipeline = pipeline_for(instance_cut_stale_src, 0x4375'7453'7461'6c65ULL);
        const bool have_pipelines = pipeline && partition_pipeline && runs_pipeline && fill_pipeline && begin_pipeline
                                 && pages_pipeline && choose_pipeline && place_pipeline && draw_pipeline
                                 && visible_pipeline && tree_pipeline && top_pipeline && mark_pipeline && sweep_pipeline && pieces_pipeline
                                 && rename_pipeline
                                 && items_pipeline && stale_pipeline;
        IGpuCommandBuffer::Ptr stage_cmds[kCullStages];
        for (auto& stage_cmd : stage_cmds) {
            stage_cmd = have_pipelines ? ctx.backend->create_command_buffer() : nullptr;
            if (!stage_cmd) {
                return;
            }
        }
        // The stage being recorded; next_stage ends it and begins the next.
        uint32_t stage = 0;
        IGpuCommandBuffer* cmd = stage_cmds[0].get();
        const auto begin_stage = [&]() {
            cmd = stage_cmds[stage].get();
            cmd->begin_recording();
            cmd->push_label(kCullStageNames[stage]);
            // Earlier frames may still be drawing from these lists, and the stage before
            // wrote what this one reads.
            cmd->record_global_barrier();
        };
        const auto next_stage = [&]() {
            cmd->pop_label();
            cmd->end_recording();
            ++stage;
            begin_stage();
        };
        begin_stage();

        const bool together = state.together_items != 0;
        const auto together_word = together ? static_cast<uint32_t>(state.together.offset() / sizeof(uint32_t)) : 0u;
        const uint32_t together_entries_word =
            together_word + kCullChosenHeaderWords + state.together_chosen * kCullChosenRecordWords;
        const uint32_t together_items_word = together_entries_word + state.together_entries;
        if (together) {
            cmd->record_clear_buffer(arena_buffer, uint64_t(together_word) * sizeof(uint32_t), sizeof(uint32_t));
        }
        // The records of items drawn by their own pass: none unless the draw pass finds one.
        const uint32_t item_records_word = instance_item_records_word(state);
        if (item_records_word != kCullNoSlot) {
            cmd->record_clear_buffer(arena_buffer, uint64_t(item_records_word) * sizeof(uint32_t),
                                     uint64_t(state.together_items) * sizeof(uint32_t));
        }
        if (state.retest.valid()) {
            cmd->record_clear_buffer(arena_buffer, state.retest.offset(), sizeof(uint32_t));
        }
        // Only the count: the rest of the header holds what the last frame drew until
        // instance_tiny_args_src writes it again.
        if (state.tiny_word != kCullNoSlot) {
            cmd->record_clear_buffer(arena_buffer, uint64_t(state.tiny_word) * sizeof(uint32_t), sizeof(uint32_t));
        }
        // The quad list's instance count.
        const uint32_t quads_word = instance_quad_word(state);
        if (quads_word != kCullNoSlot) {
            cmd->record_clear_buffer(arena_buffer, uint64_t(quads_word + kQuadListCount) * sizeof(uint32_t), sizeof(uint32_t));
            cmd->record_clear_buffer(arena_buffer, uint64_t(quads_word + kQuadListInCuts) * sizeof(uint32_t), sizeof(uint32_t));
        }
        cmd->record_global_barrier();

        const auto dispatch =[&](IGpuPipeline* used, const void* pc, uint32_t pc_size, uint32_t groups,
                                  uint32_t args_word = kCullNoSlot) {
            DispatchCall dc{};
            dc.pipeline = used;
            dc.groups_x = groups;
            dc.groups_y = 1;
            dc.groups_z = 1;
            dc.root_constants_size = pc_size;
            std::memcpy(dc.root_constants, pc, pc_size);
            if (args_word != kCullNoSlot) {
                cmd->record_dispatch_indirect(dc, arena_buffer, uint64_t(args_word) * sizeof(uint32_t));
            } else {
                cmd->record_dispatch(dc);
            }
        };

        // The instanced visuals' items, from their slot tables, before anything reads them.
        if (together && state.instanced_items != 0 && state.item_descriptor_region.valid()) {
            const uint32_t objects_word = together_items_word + state.together_items * kPlainItemWords;
            const uint32_t items_pc[7] = {static_cast<uint32_t>(state.item_descriptor_region.offset() / sizeof(uint32_t)),
                                          state.item_descriptor_count, together_items_word, state.instanced_first_item,
                                          state.instanced_items, objects_word, objects_word + state.together_items};
            dispatch(items_pipeline.get(), items_pc, sizeof(items_pc), (state.instanced_items + 63u) / 64u);
            cmd->record_global_barrier();
        }

        // The kept cut: the scene's objects', by item.
        struct CutJob
        {
            uint32_t cut;
            uint32_t partition_table;
            uint32_t partition_count;
            float max_distance;
            uint32_t instances_base;
            uint32_t slot_count;
            uint32_t table_word;
            uint32_t chosen_word;
            uint32_t chosen_capacity;
            uint32_t items_word;  ///< The items.
            float pad;            ///< What its tree's boxes grow by besides each item's own: see instance_cut_tree_src.
        };
        const uint32_t together_objects_word = together_items_word + state.together_items * kPlainItemWords;
        const uint32_t together_next_word = together_objects_word + state.together_items;
        const uint32_t together_head_word = together_next_word + state.together_items;
        vector<CutJob> jobs;
        const uint32_t plain_cut_word = plain_cut ? cut_word(&kPlainCutKey) : kCullNoSlot;
        if (plain_cut_word != kCullNoSlot) {
            jobs.push_back({plain_cut_word, plain_table_word, plain_count, 0.f, 0u, 0u, 0u, together_word, state.together_chosen,
                            together_items_word, 0.f});
        }
        for (const CutJob& job : jobs) {
            dispatch(begin_pipeline.get(), &job.cut, sizeof(job.cut), 1u);
        }
        cmd->record_global_barrier();

        // Records named in batches made again this frame, before anything reads them.
        if (!state.renames.empty()) {
            for (const auto& rename : state.renames) {
                for (const auto& c : state.cuts) {
                    if (c.key != rename.key) continue;
                    const uint32_t pc[7] = {static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t)), rename.pairs_word,
                                            static_cast<uint32_t>(rename.pairs.size() / 2u), rename.items_word,
                                            static_cast<uint32_t>(rename.items.size()), rename.blocks_word,
                                            static_cast<uint32_t>(rename.blocks.size() / 3u)};
                    dispatch(rename_pipeline.get(), pc, sizeof(pc), (c.capacity + 63u) / 64u);
                }
            }
            cmd->record_global_barrier();
            // Partitions with records that did not carry over choose again.
            for (const auto& rename : state.renames) {
                for (const auto& c : state.cuts) {
                    if (c.key != rename.key || c.partitions == 0) continue;
                    const uint32_t cut_pc = static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t));
                    dispatch(stale_pipeline.get(), &cut_pc, sizeof(cut_pc), c.partitions);
                }
            }
            cmd->record_global_barrier();
        }

        // Partitions next: whether they keep their records.
        struct PartitionPushC
        {
            uint32_t table_word;
            uint32_t entry_count;
            uint32_t globals_base;
            uint32_t cut_word;
            float refresh;
        };
        for (const CutJob& job : jobs) {
            const PartitionPushC pc{job.partition_table, job.partition_count, view_globals_base, job.cut, kCutRefresh};
            dispatch(partition_pipeline.get(), &pc, sizeof(pc), (job.partition_count + 63u) / 64u);
        }
        next_stage();

        // The kept cuts: records counted for the partitions that choose again, placed, written,
        // then drawn from; those that keep theirs keep their pages.
        if (!jobs.empty()) {
            struct ChoosePushC
            {
                uint32_t cut_word;
                uint32_t instances_base;
                uint32_t slot_count;
                uint32_t table_word;
                uint32_t globals_base;
                float lod_max_pixels;
                uint32_t write;
                uint32_t items_word;
                uint32_t objects_word;
                uint32_t next_word;
                uint32_t head_word;
                uint32_t object_slots;
                uint32_t partition_table;
                float quad_pixels;
                float light_pixels;
                uint32_t descriptors_word;
                uint32_t descriptor_count;
            };
            const auto choose = [&](const CutJob& job, uint32_t write) {
                const uint32_t partition_table = job.partition_table;
                const ChoosePushC pc{job.cut, job.instances_base, job.slot_count, job.table_word, view_globals_base,
                                     kLodMaxErrorPixels, write, job.items_word, together_objects_word, together_next_word,
                                     together_head_word, state.together_object_slots, partition_table, kQuadPixels,
                                     kLodMaxErrorPixels * kLightPixels / kLodElementPixels,
                                     state.item_descriptor_region.valid()
                                         ? static_cast<uint32_t>(state.item_descriptor_region.offset() / sizeof(uint32_t))
                                         : 0u,
                                     state.item_descriptor_region.valid() ? state.item_descriptor_count : 0u};
                dispatch(choose_pipeline.get(), &pc, sizeof(pc), 1u, job.cut + kCutChooseGroups);
            };
            for (const CutJob& job : jobs) {
                choose(job, 0u);
            }
            next_stage();
            for (const CutJob& job : jobs) {
                dispatch(place_pipeline.get(), &job.cut, sizeof(job.cut), (job.partition_count + 63u) / 64u);
            }
            next_stage();
            for (const CutJob& job : jobs) {
                choose(job, 1u);
            }
            next_stage();
            // Then the trees for rays over the partitions that chose again.
            struct TreePushC
            {
                uint32_t cut_word;
                uint32_t instances_base;
                uint32_t items_word;
                float pad;
                uint32_t globals_base;
                float light_pixels;
            };
            for (const CutJob& job : jobs) {
                const TreePushC pc{job.cut, job.instances_base, job.items_word, job.pad, view_globals_base,
                                   kLodMaxErrorPixels * kLightPixels / kLodElementPixels};
                dispatch(tree_pipeline.get(), &pc, sizeof(pc), job.partition_count);
            }
            cmd->record_global_barrier();

            // And the top tree over them all, its cuts and leaves written into the region
            // reserve_instance_cull made when they changed (instance_ray_top_layout).
            uint32_t leaves = 0;
            for (const CutJob& job : jobs) {
                leaves += job.partition_count;
            }
            if (state.ray_top.valid() && state.ray_top_fresh) {
                state.ray_top_fresh = false;
                const auto cut_count = static_cast<uint32_t>(jobs.size());
                const uint32_t node_count = top_tree_nodes(leaves);
                vector<uint32_t> words;
                words.resize(size_t(kRayTopHeaderWords) + size_t(cut_count) * kRayTopCutWords + size_t(leaves) * kRayTopLeafWords, 0u);
                words[kRayTopCuts] = cut_count;
                words[kRayTopLeaves] = leaves;
                words[kRayTopNodes] = node_count;
                words[kRayTopRoot] = node_count - 1u;
                uint32_t leaf = 0;
                for (uint32_t c = 0; c < cut_count; ++c) {
                    const CutJob& job = jobs[c];
                    uint32_t* cut = words.data() + kRayTopHeaderWords + size_t(c) * kRayTopCutWords;
                    cut[kRayTopCutCut] = job.cut;
                    cut[kRayTopCutInstancesBase] = job.instances_base;
                    cut[kRayTopCutItems] = job.items_word;
                    cut[kRayTopCutPartitions] = job.partition_count;
                    cut[kRayTopCutFirstLeaf] = leaf;
                    cut[kRayTopCutMoves] = state.items_move ? 1u : 0u;
                    cut[kRayTopCutQuads] = quads_word;
                    for (uint32_t p = 0; p < job.partition_count; ++p, ++leaf) {
                        uint32_t* at = words.data() + kRayTopHeaderWords + size_t(cut_count) * kRayTopCutWords + size_t(leaf) * kRayTopLeafWords;
                        at[kRayTopLeafCut] = c;
                        at[kRayTopLeafPartition] = p;
                    }
                }
                arena->write_at(state.ray_top.offset(), words.data(), words.size() * sizeof(uint32_t));
                state.ray_top_cuts.clear();
                for (const CutJob& job : jobs) state.ray_top_cuts.push_back(job.cut);
            }
            if (state.ray_top.valid()) {
                const uint32_t top_word = instance_ray_top_word(state);
                dispatch(top_pipeline.get(), &top_word, sizeof(top_word), 1u);
            }
            next_stage();

            // The pages no page table names are free again, for the next frame's choosing, and
            // the passes over the pools' records go up to the last page in use.
            for (const CutJob& job : jobs) {
                uint32_t pool_pages = 0;
                for (const auto& c : state.cuts) {
                    if (static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t)) == job.cut) pool_pages = c.capacity / kCutPageRecords;
                }
                const uint32_t mark_pc[2] = {job.cut, 0u};
                dispatch(pages_pipeline.get(), mark_pc, sizeof(mark_pc), (pool_pages + 63u) / 64u + 1u);
            }
            cmd->record_global_barrier();
            for (const CutJob& job : jobs) {
                uint32_t pool_pages = 0;
                for (const auto& c : state.cuts) {
                    if (static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t)) == job.cut) pool_pages = c.capacity / kCutPageRecords;
                }
                const uint32_t sweep_pc[2] = {job.cut, 1u};
                dispatch(pages_pipeline.get(), sweep_pc, sizeof(sweep_pc), (pool_pages / 32u + 1u + 63u) / 64u);
            }
            cmd->record_global_barrier();

            // The records are what they are this frame: the light pages none of them names are
            // free again.
            if (light_pool_word != kCullNoSlot) {
                const uint32_t begin_pc[2] = {light_pool_word, 1u};
                dispatch(sweep_pipeline.get(), begin_pc, sizeof(begin_pc), 1u);
                cmd->record_global_barrier();
                for (const CutJob& job : jobs) {
                    const uint32_t mark_pc[5] = {job.cut, light_pool_word, job.instances_base, job.items_word, view_globals_base};
                    dispatch(mark_pipeline.get(), mark_pc, sizeof(mark_pc), 1u, job.cut + kCutDrawGroups);
                }
                cmd->record_global_barrier();
                // The pieces: what no pixel reads unlinked, then those still linked marked.
                const uint32_t piece_groups = (state.light_pool.pages() + 63u) / 64u;
                const uint32_t unlink_pc[2] = {light_pool_word, 0u};
                dispatch(pieces_pipeline.get(), unlink_pc, sizeof(unlink_pc), piece_groups);
                cmd->record_global_barrier();
                const uint32_t keep_pc[2] = {light_pool_word, 1u};
                dispatch(pieces_pipeline.get(), keep_pc, sizeof(keep_pc), piece_groups);
                cmd->record_global_barrier();
                const uint32_t sweep_pc[2] = {light_pool_word, 0u};
                dispatch(sweep_pipeline.get(), sweep_pc, sizeof(sweep_pc), (state.light_pool.pages() / 32u + 63u) / 64u);
            }
            next_stage();
        } else {
            state.ray_top = {};
            state.ray_top_layout = 0;
            state.ray_top_cuts.clear();
            // No kept cuts: their stages are empty.
            while (stage + 1u < kCullStages) next_stage();
        }

        struct PushC
        {
            uint32_t instances_base;
            uint32_t slot_count;
            uint32_t table_word;
            uint32_t globals_base;
            float max_distance;
            float lod_max_pixels;
            uint32_t items_word;
            uint32_t chosen_word;
            uint32_t chosen_capacity;
            uint32_t pyramid_word;
            uint32_t retest_word;
            uint32_t retest_capacity;
            uint32_t tiny_word;
            uint32_t tiny_capacity;
            float tiny_pixels;
        };
        const auto retest_word = static_cast<uint32_t>(state.retest.offset() / sizeof(uint32_t));
        struct DrawPushC
        {
            uint32_t cut_word;
            uint32_t instances_base;
            uint32_t table_word;
            uint32_t globals_base;
            float max_distance;
            float lod_max_pixels;
            uint32_t chosen_word;
            uint32_t chosen_capacity;
            uint32_t pyramid_word;
            uint32_t retest_word;
            uint32_t retest_capacity;
            uint32_t tiny_word;
            uint32_t tiny_capacity;
            float tiny_pixels;
            uint32_t whole;
            uint32_t items_word;
            uint32_t quads_word;
            uint32_t quad_capacity;
            float quad_pad;
            uint32_t item_records_word;
        };
        // The records of partitions in view, in runs, then those drawn.
        for (const CutJob& job : jobs) {
            if (job.partition_count == 0) continue;
            const uint32_t visible_pc[4] = {job.cut, view_globals_base, whole_cut ? 1u : 0u, quads_word};
            dispatch(visible_pipeline.get(), visible_pc, sizeof(visible_pc), (job.partition_count + 63u) / 64u);
        }
        if (!jobs.empty()) cmd->record_global_barrier();
        for (const CutJob& job : jobs) {
            const DrawPushC pc{job.cut, job.instances_base, job.table_word, view_globals_base, job.max_distance,
                               kLodMaxErrorPixels, job.chosen_word, job.chosen_capacity,
                               pyramid_word, retest_word, state.retest_capacity,
                               state.tiny_word, state.tiny_capacity, state.tiny_pixels, whole_cut ? 1u : 0u, job.items_word,
                               quads_word, state.quad_capacity, job.pad, item_records_word};
            dispatch(draw_pipeline.get(), &pc, sizeof(pc), 1u, job.cut + kCutVisibleGroups);
        }
        // Without the scene's plain partitions, plain objects are chosen every frame.
        if (together && plain_cut_word == kCullNoSlot) {
            const PushC pc{0u, state.together_items, 0u, view_globals_base, 0.f, kLodMaxErrorPixels,
                           together_items_word, together_word, state.together_chosen,
                           pyramid_word, retest_word, state.retest_capacity,
                           state.tiny_word, state.tiny_capacity, state.tiny_pixels};
            dispatch(pipeline.get(), &pc, sizeof(pc), (state.together_items + 63u) / 64u);
        }
        cmd->record_global_barrier();

        // Draws by cluster: lay out each one's runs, then fill them.
        record_cluster_layout(*cmd, state, runs_pipeline.get(), fill_pipeline.get(), arena_buffer, together_word,
                              together_entries_word, false);

        // Each other batch's indirect args take the count of its list.
        constexpr uint64_t kInstanceCountOffset = BatchBufferLayout::kArgsOffset + sizeof(uint32_t);
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (!batch || !batch->get_instance_cull(info) || info.cluster_count != 0) {
                continue;
            }
            auto* entry = batch->storage_buffer() ? ctx.resources->find_buffer(batch->storage_buffer()) : nullptr;
            auto args = entry ? entry->buffer.lock() : nullptr;
            if (args) {
                cmd->record_copy_buffer(arena_buffer, uint64_t(info.count_word) * sizeof(uint32_t), args.get(),
                                        kInstanceCountOffset, sizeof(uint32_t));
            }
        }
        cmd->record_global_barrier();

        cmd->pop_label();
        cmd->end_recording();
        for (uint32_t k = 0; k < kCullStages; ++k) stage_recs[k].cmd = std::move(stage_cmds[k]);
        // The last stage holds the pipelines for all of them.
        rec.held.push_back(std::move(pipeline));
        rec.held.push_back(std::move(partition_pipeline));
        rec.held.push_back(std::move(runs_pipeline));
        rec.held.push_back(std::move(fill_pipeline));
        rec.held.push_back(std::move(begin_pipeline));
        rec.held.push_back(std::move(pages_pipeline));
        rec.held.push_back(std::move(choose_pipeline));
        rec.held.push_back(std::move(place_pipeline));
        rec.held.push_back(std::move(tree_pipeline));
        rec.held.push_back(std::move(top_pipeline));
        rec.held.push_back(std::move(draw_pipeline));
        rec.held.push_back(std::move(visible_pipeline));
        rec.held.push_back(std::move(mark_pipeline));
        rec.held.push_back(std::move(sweep_pipeline));
        rec.held.push_back(std::move(pieces_pipeline));
        rec.held.push_back(std::move(rename_pipeline));
        rec.held.push_back(std::move(items_pipeline));
        rec.held.push_back(std::move(stale_pipeline));
    };
    if (record) record_all();
    const char* const* names = state.chosen_by_other ? kCullOtherStageNames : kCullStageNames;
    for (uint32_t k = 0; k < kCullStages; ++k) {
        bool stage_dirty = record;
        emit_cached_view_pass(state.passes[k], stage_dirty, names[k], graph,
                              [&](CachedPassRecording& rec) { rec = std::move(stage_recs[k]); });
    }
}

/**
 * @brief Adds the second phase of culling to @p graph, after what passed the first has been
 *        drawn and @p state's pyramid built from it. Only meaningful after
 *        emit_instance_cull_pass was given a pyramid.
 */
inline void emit_instance_retest_pass(InstanceCullPass& state, const vector<IBatch::Ptr>* batches,
                                      uint32_t view_globals_base, FrameContext& ctx, IRenderGraph& graph)
{
    if (!batches || state.pyramid_word == kCullNoSlot || !state.retest.valid() || !ctx.backend || !ctx.render_ctx) {
        return;
    }
    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    IGpuBuffer* arena_buffer = arena ? arena->buffer() : nullptr;
    if (!arena_buffer) {
        return;
    }

    const char* name = state.chosen_by_other ? "other:instance.retest" : "instance.retest";
    emit_cached_view_pass(state.retest_pass, state.retest_dirty, name, graph, [&](CachedPassRecording& rec) {
        const auto pipeline_for = [&](string_view src, uint64_t key) {
            auto found = ctx.render_ctx->pipelines().find(PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0});
            return found ? found : ctx.render_ctx->pipelines().compile_compute(src, key);
        };
        auto pipeline = pipeline_for(instance_retest_compute_src, 0x4375'6c6c'5265'7473ULL);
        auto runs_pipeline = pipeline_for(instance_cluster_runs_src, 0x4375'6c6c'5275'6e73ULL);
        auto fill_pipeline = pipeline_for(instance_cluster_fill_src, 0x4375'6c6c'4669'6c6cULL);
        auto cmd = pipeline && runs_pipeline && fill_pipeline ? ctx.backend->create_command_buffer() : nullptr;
        if (!cmd) {
            return;
        }

        const bool together = state.together_items != 0;
        const auto together_word = together ? static_cast<uint32_t>(state.together.offset() / sizeof(uint32_t)) : 0u;
        const uint32_t together_entries_word =
            together_word + kCullChosenHeaderWords + state.together_chosen * kCullChosenRecordWords;

        cmd->begin_recording();
        cmd->push_label("Instance retest");
        cmd->record_global_barrier();
        if (together) {
            cmd->record_clear_buffer(arena_buffer, uint64_t(together_word) * sizeof(uint32_t), sizeof(uint32_t));
        }
        cmd->record_global_barrier();

        struct PushC
        {
            uint32_t retest_word;
            uint32_t retest_capacity;
            uint32_t globals_base;
            uint32_t pyramid_word;
            float lod_max_pixels;
            uint32_t tiny_word;
            uint32_t tiny_capacity;
            float tiny_pixels;
        };
        const PushC pc{static_cast<uint32_t>(state.retest.offset() / sizeof(uint32_t)), state.retest_capacity, view_globals_base,
                       state.pyramid_word, kLodMaxErrorPixels, state.tiny_word, state.tiny_capacity, state.tiny_pixels};
        DispatchCall dc{};
        dc.pipeline = pipeline.get();
        dc.groups_x = (state.retest_capacity + 63u) / 64u;
        dc.groups_y = 1;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(pc);
        std::memcpy(dc.root_constants, &pc, sizeof(pc));
        cmd->record_dispatch(dc);
        cmd->record_global_barrier();

        record_cluster_layout(*cmd, state, runs_pipeline.get(), fill_pipeline.get(), arena_buffer, together_word,
                              together_entries_word, true);

        // A plain instanced draw continues its list where the first phase stopped.
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (!batch || !batch->get_instance_cull(info) || info.cluster_count != 0 || info.second_args_word == kCullNoSlot) {
                continue;
            }
            const uint64_t word = sizeof(uint32_t);
            cmd->record_copy_buffer(arena_buffer, uint64_t(info.count_word + 7u) * word, arena_buffer,
                                    uint64_t(info.second_args_word + 1u) * word, sizeof(uint32_t));
            cmd->record_copy_buffer(arena_buffer, uint64_t(info.count_word) * word, arena_buffer,
                                    uint64_t(info.second_args_first_word) * word, sizeof(uint32_t));
        }
        cmd->record_global_barrier();

        cmd->pop_label();
        cmd->end_recording();
        rec.cmd = std::move(cmd);
        rec.held.push_back(std::move(pipeline));
        rec.held.push_back(std::move(runs_pipeline));
        rec.held.push_back(std::move(fill_pipeline));
    });
}

} // namespace velk

#endif // VELK_RENDER_FRAME_INSTANCE_CULL_H
