#ifndef VELK_RENDER_FRAME_INSTANCE_CULL_H
#define VELK_RENDER_FRAME_INSTANCE_CULL_H

#include <velk/api/perf.h>
#include <velk/api/velk.h>
#include <velk/string_view.h>
#include <velk/vector.h>

#include <velk-render/api/cached_view_pass.h>
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
#include <cstring>
#include <utility>

namespace velk {

/// Slots in a chunk of a partition's list. A partition's last chunk is filled out with
/// kCullNoSlot.
inline constexpr uint32_t kCullChunkSlots = 64;
inline constexpr uint32_t kCullNoSlot = 0xFFFFFFFFu;
/// Words per partition in a partition table: its world bounds (min, max), the word of its first
/// chunk, its chunk count, the word of its quad tree (kQuadNodeWords, kCullNoSlot for none) and
/// the tree's leaves. See instance_partition_cull_src. Mirrors VELK_CULL_ENTRY_WORDS.
inline constexpr uint32_t kCullEntryWords = 10;
/// Set in a partition table entry's leaves word when its tree's leaves are the finest it has (a
/// partition without its instances, whose list names chunk quads): where no node is small
/// enough, the leaf itself is taken. Mirrors VELK_QUAD_LEAVES_ONLY.
inline constexpr uint32_t kQuadLeavesOnly = 0x80000000u;
/// Words per chunk in a kept cut's choose list: the word of its first slot, and the partition
/// it is from.
inline constexpr uint32_t kCullChunkListWords = 2;
/// The slot an object id names, in tables by object (IScene::get_object_ids).
inline constexpr uint32_t kObjectSlotMask = 0x3FFFFFFu;
/// Words per item (InstanceCullPass::together), what a kept cut record names: its instance's
/// index (kCullNoSlot for an instance without a place in its raster region), its slot in its
/// batch's instances, its table, its draw's index in the draw data, what rays test it as (an
/// RtShape kind: 255 its mesh, else its visual's analytic shape, see IAnalyticShape), its mesh's
/// ray data (MeshStaticData's index, kInvalidMeshStaticBase for none), and how far a motion
/// function can move it (a float), which its bounds grow by. A plain object's instances are an
/// item each, first; then each instanced visual's items, an item per slot, which
/// instance_items_src writes whole each frame, with their objects and next words, so that the
/// CPU never goes through them. Mirrors VELK_PLAIN_ITEM_WORDS.
inline constexpr uint32_t kPlainItemWords = 7;
/// Words per instanced visual in the item descriptors instance_items_src and the choose read:
/// its first item, its slots, its instances' base, its slot table's word, its mesh count M, its
/// table, how far it can move (a float) and its first object id (slot i's object is that plus
/// i), then per mesh its draw, its ray shape kind and its mesh's ray data (kItemMeshWords each).
/// See InstanceCullPass::item_descriptors. Mirrors VELK_ITEM_DESCRIPTOR_HEADER_WORDS.
inline constexpr uint32_t kItemDescriptorHeaderWords = 8;
inline constexpr uint32_t kItemMeshWords = 3;
/// A plain item's shape kind for a mesh. Mirrors RtShape's 255.
inline constexpr uint32_t kRayShapeMesh = 255;
/// A plain item's shape kind for a rect over its instance's extent. Mirrors RtShape's 0.
inline constexpr uint32_t kRayShapeRect = 0;
/// How far the camera may get from where a partition's levels of detail were chosen before
/// they are chosen again, as a share of its distance from the partition.
inline constexpr float kCutRefresh = 1.f / 16.f;

/// A view's kept cut of a partitioned visual, in the culling arena: the clusters chosen for each
/// partition, all round the camera, kept while the camera stays near where they were chosen
/// from. A header of kCutHeaderWords:
///   0..3   group counts of the copy dispatch: how many partitions keep their records
///   4..7   group counts of the choosing dispatches: how many chunks choose again
///   8..11  group counts of the drawing dispatch: a group per 64 records
///   12     which store is current, 0 or 1
///   13     records the current store was asked for, past its room when some did not fit
///   14     how many times partitions chose again, for the log
///   15     the most records a frame wanted, when more than fit
///   16     records a store holds
///   17, 18 the stores' words
///   19     partitions
///   20     the word of the keep list: a partition index each
///   21     the word of the choose list: kCullChunkListWords a chunk
///   22     the word of the partitions' states, kCutPartitionWords each:
///            0..2 where in the world it chose from, 3 and 4 its table entry's first chunk word
///            and chunk count when it chose, 5 its first record and 6 its record count in the
///            current store, 7 its first record in the last store, 8 1 while choosing this
///            frame, 9 while choosing where its chunks start in the choose list, 10 its first
///            tree node in the current node store, 11 in the last, 12 its table entry's quad
///            tree word when it chose (a new tree makes it choose again), 13 and 14 unused, 15
///            while choosing again how many records it had, whose light its new records take
///            over (7 then says where they are), 0 for none, 16 how many records rays trace (its
///            ray list), 17 to 19
///            which way the camera looked when it chose, 20 1 when it chose some of its clusters
///            at the light's level only because they were out of view, so that it chooses
///            again when the camera turns
///   23     the word of the choose list's chunk counts, one a chunk: how many records the
///          chunk's instances choose, then its first record
///   24     tree nodes in the current node store
///   25     tree nodes a node store holds
///   26, 27 the node stores' words, flipping with the record stores
///   28     the end of the records placed in the current store, which is all that is read: a
///          partition that does not fit leaves its range unwritten, and every one after it fails
///          too, so all placed records are before it
///   29     the frame of the takeover table, counted by the begin pass: a slot of another frame
///          is empty
///   30, 31 the takeover table's word and its slots, a power of two, kCutTakeoverWords each:
///          the old records of the partitions choosing again, by what they are, for each new
///          record to find the one that was the same and take over its light
///   32     records for light only (kCutLightOnly) in the current store, as the drawing counts them
///   33     the same for the last frame, complete, for the log
///   34, 35 records written by partitions choosing again this frame, and of those how many took
///          over the light of an old record that was the same
///   36, 37 the same for the last frame, complete, for the log
///   38, 39 the ray list stores' words, flipping with the record stores: for each partition, from
///          its first record's place, the places of the records rays trace (those at the light's
///          level: records for light only, clusters as coarse as the light, quads, whole objects),
///          which its tree is over
///   40, 41 the most hierarchy nodes one thread walked choosing this frame, and the most records
///          one walk chose
///   42, 43 the same for the last frame, complete, for the log
///   44     the word of the ray marks: a bit for each record of the current store, set by the
///          choosing where the record's member is in a patch for rays (kCutRayQuad), so that the
///          tree pass leaves it out of the ray list; only the partitions that chose this frame
///          read them
/// A partition's records are in the order of its chunks, each chunk's in the order of its
/// members, and each instance's in the order its hierarchy's tree is expanded and walked, so
/// that records near in the store are near in the world.
/// A record is kCutRecordWords: entry, cluster and slot, as the chosen lists have them, and its
/// light (kLightNone until it is read, see light_pool.h); for a mesh without clusters the whole instance: entry kCullNoSlot,
/// cluster its mesh's table; for an item rays see that its own pass draws, entry and cluster
/// kCullNoSlot. What is drawn from a record, and what a ray hits of it, carries the word of the
/// record (velk_ids_record, RayHit::record), which holds for that frame.
///
/// Each partition has a tree over its records for rays, kCutNodeWords a node: the boxes of
/// its two children (left min, left max, right min, right max, in world space), then a
/// reference to each, its level (1 over leaves) and 1 for the root. A reference with kCutLeaf
/// is up to four of its records, the first one's offset from its first record times four plus
/// how many less one; without, a node's offset from its first node; kCullNoSlot for none. Its
/// nodes are its levels from the bottom, the root last. See instance_cut_tree_src.
inline constexpr uint32_t kCutHeaderWords = 45;
inline constexpr uint32_t kCutPartitionWords = 24;  ///< Mirrors VELK_CUT_PARTITION_WORDS.
/// Words per slot of a kept cut's takeover table: the frame it was filled in and an old record's word.
inline constexpr uint32_t kCutTakeoverWords = 2;
inline constexpr uint32_t kCutRecordWords = 4;
inline constexpr uint32_t kCutNodeWords = 16;
inline constexpr uint32_t kCutLeaf = 0x80000000u;

/// A partition's quad tree (VELK_QUAD_NODE_WORDS): its members as quads, and every node above them a quad standing for
/// the members under it, so that where they are small on screen a few quads take their place.
/// A complete binary tree over the partition's members in their order, by index with no links:
/// node 0 is the root, node i's children are 2i + 1 and 2i + 2, and the leaves are the last
/// `leaves` nodes, leaf k the member at position k of the partition's list (a power of two of
/// them, the rest empty). A node is kQuadNodeWords: its centre (3 floats), its two half axes
/// (3 each; its normal is their cross product), its colour (RGBA8, red in the low byte), and
/// the radius of a sphere at its centre around it and every quad under it, negative for an
/// empty node, in world space, and the light of the members under it where it is as wide on
/// screen as the light spacing (kLightPixels), one value or kLightNone (see light_pool.h).
inline constexpr uint32_t kQuadNodeWords = 12;

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

/// Nodes in the tree over a partition of @p records records. Mirrors velk_cut_tree_nodes.
inline uint32_t cut_tree_nodes(uint32_t records)
{
    if (records == 0) {
        return 0;
    }
    uint32_t below = (records + 3u) / 4u;
    uint32_t total = 0;
    do {
        below = (below + 1u) / 2u;
        total += below;
    } while (below > 1u);
    return total;
}

// Where a cut's parts are. The shader declares cull_words.
#define VELK_CUT_GLSL R"(
uint velk_cut_state(uint cut, uint index)
{
    return cull_words.data[cut + 22u] + index * VELK_CUT_PARTITION_WORDS;
}

// The current store, or the last frame's.
uint velk_cut_store(uint cut, bool current)
{
    uint parity = cull_words.data[cut + 12u];
    return cull_words.data[cut + 17u + (current ? parity : 1u - parity)];
}

// The current node store, or the last frame's.
uint velk_cut_node_store(uint cut, bool current)
{
    uint parity = cull_words.data[cut + 12u];
    return cull_words.data[cut + 26u + (current ? parity : 1u - parity)];
}

// The current ray list store, or the last frame's.
uint velk_cut_ray_store(uint cut, bool current)
{
    uint parity = cull_words.data[cut + 12u];
    return cull_words.data[cut + 38u + (current ? parity : 1u - parity)];
}

// Nodes in the tree over a partition's `records` records: its leaves of four records paired
// level by level up to one. Mirrors cut_tree_nodes.
uint velk_cut_tree_nodes(uint records)
{
    if (records == 0u) return 0u;
    uint below = (records + 3u) / 4u;
    uint total = 0u;
    do {
        below = (below + 1u) / 2u;
        total += below;
    } while (below > 1u);
    return total;
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

/// The list of what failed the first occlusion test starts with its count. A record is an
/// instance's index in the instance arena, its slot, its table, then an entry and cluster, the
/// entry kCullNoSlot for the whole instance, the kept cut record it is from, where the visual's chosen
/// clusters go and how many fit, then where its levels of detail are chosen from (three
/// floats, see instance_partition_cull_src) and a spare word. What does not fit is drawn.
inline constexpr uint32_t kCullRetestHeaderWords = 1;
inline constexpr uint32_t kCullRetestRecordWords = 12;
inline constexpr uint32_t kCullRetestMaxRecords = 1u << 20;

// Whether a sphere around a point relative to the camera, as seen through vp, lies behind all
// that a depth pyramid holds there. A larger depth is nearer. Declares nothing the shader must
// provide but cull_words.
#define VELK_OCCLUSION_GLSL R"(
bool velk_behind_pyramid(uint pyramid, mat4 vp, vec3 center, float radius)
{
    uint levels = cull_words.data[pyramid];
    if (levels == 0u) return false;

    vec2 lo = vec2(1e30);
    vec2 hi = vec2(-1e30);
    float nearest = 0.0;
    for (int i = 0; i < 8; ++i) {
        vec3 corner = center + vec3((i & 1) != 0 ? radius : -radius, (i & 2) != 0 ? radius : -radius,
                                    (i & 4) != 0 ? radius : -radius);
        vec4 clip = vp * vec4(corner, 1.0);
        if (clip.w <= 1e-5) return false;
        vec3 ndc = clip.xyz / clip.w;
        lo = min(lo, ndc.xy);
        hi = max(hi, ndc.xy);
        nearest = max(nearest, ndc.z);
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

/// Words before the records of the list of clusters drawn in compute (instance_tiny_draw_src):
/// how many were added, the group counts of the dispatch that draws them, how many of them
/// are drawn, and three spare. A record is kTinyRecordWords: its draw data record, the word
/// of its cluster in the level of detail table, its instance's slot under that draw, and the
/// word of the kept cut record it is from (kCutRecordWords), 0xFFFFFFFF for none.
inline constexpr uint32_t kTinyHeaderWords = 8;
inline constexpr uint32_t kTinyRecordWords = 4;

// Adds a cluster to the list of those drawn in compute, false when it is full. The shader has
// pc.tiny_word and pc.tiny_capacity.
#define VELK_TINY_ADD_GLSL R"(
bool velk_tiny_add(uint draw, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[pc.tiny_word], 1u);
    if (at >= pc.tiny_capacity) return false;
    uint record = pc.tiny_word + 8u + at * 4u;
    cull_words.data[record] = draw;
    cull_words.data[record + 1u] = cluster;
    cull_words.data[record + 2u] = slot;
    cull_words.data[record + 3u] = cut_record;
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
    if (uintBitsToFloat(lod_nodes.data[child + 4u]) * per_error / (dist + reach) > coarsest) return VELK_LOD_SKIP;
    if (uintBitsToFloat(lod_nodes.data[child + 5u]) * per_error / max(dist - reach, 1e-3) <= finest) return VELK_LOD_SKIP;
    if ((ref & VELK_LOD_LEAF) == 0u) {
        c = ref;
        return VELK_LOD_DESCEND;
    }

    c = ref & ~VELK_LOD_LEAF;
    uint cluster = clusters + c * VELK_LOD_CLUSTER_WORDS;
    float own = velk_lod_pixels(cluster + 3u, world, scale, lod_cam, pixels_per_unit);
    float parent = velk_lod_pixels(cluster + 8u, world, scale, lod_cam, pixels_per_unit);
    float limit = max_pixels;
#ifdef VELK_LOD_LIGHT
    // Out of view, the light's level is all there is of it: rays and light use it, and it
    // is drawn if it comes into view before its partition chooses again.
    vec3 at = mat3(world) * vec3(uintBitsToFloat(cull_words.data[cluster + 3u]), uintBitsToFloat(cull_words.data[cluster + 4u]),
                                 uintBitsToFloat(cull_words.data[cluster + 5u])) + world[3].xyz;
    bool out_of_view = velk_lod_out_of_view(at, uintBitsToFloat(cull_words.data[cluster + 6u]) * scale, vp);
    if (out_of_view) {
        limit = pc.light_pixels;
        g_out_of_view = true;
    }
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
    uint clusters = cull_words.data[entry + 2u];
    uint nodes = cull_words.data[entry + 8u];
    // A deforming mesh's spheres are where its clusters are at rest, which says nothing
    // about whether they are hidden.
    bool deforming = (cull_words.data[entry + 9u] & VELK_LOD_DEFORMING) != 0u;
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
    uint taken = cull_words.data[entry + 2u] + c * VELK_LOD_CLUSTER_WORDS;
    uint own_mark = cull_words.data[taken + 23u];
    if (own_mark != 0xFFFFFFFFu) {
        cull_words.data[own_mark] = 1u;
    }
    uint mark = cull_words.data[taken + 22u];
    if (mark != 0xFFFFFFFFu) {
        float from = length(velk_lod_center(taken + 3u, world, lod_cam)) - uintBitsToFloat(cull_words.data[taken + 6u]) * scale;
        if (uintBitsToFloat(cull_words.data[taken + 21u]) * scale * pixels_per_unit / max(from, 1e-3) > pc.lod_max_pixels) {
            cull_words.data[mark] = 1u;
        }
    }
    uint draw = cull_words.data[entry + 13u];
    if (pc.tiny_pixels > 0.0 && draw != 0xFFFFFFFFu && (cull_words.data[entry + 9u] & VELK_LOD_MOVED) == 0u) {
        uint cluster = cull_words.data[entry + 2u] + c * VELK_LOD_CLUSTER_WORDS;
        float radius = uintBitsToFloat(cull_words.data[cluster + 16u]) * scale;
        float away = length(velk_lod_center(cluster + 13u, world, cam));
        float across = 2.0 * radius * pixels_per_unit / max(away, 1e-3);
        float triangles = float(max(cull_words.data[cluster + 1u], 1u));
        // The largest triangle as wide as it can be on screen, at the cluster's nearest.
        float largest = uintBitsToFloat(cull_words.data[cluster + 17u]) * scale * pixels_per_unit / max(away - radius, 1e-3);
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
 * entry is as it was. A partition that keeps its records gets a range for them in the current
 * store and goes to the keep list; any other chooses again from the camera, with its chunks in
 * the choose list, kCullChunkListWords each: its first slot's word and its partition's index.
 * So a partition's choice is never off by more than that share.
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
    cull_words.data[state + 8u] = 0u;
    if (chunks == 0u) {
        cull_words.data[state + 6u] = 0u;
        cull_words.data[state + 16u] = 0u;
        return;
    }
    vec3 viewpoint = vec3(uintBitsToFloat(cull_words.data[state]), uintBitsToFloat(cull_words.data[state + 1u]),
                          uintBitsToFloat(cull_words.data[state + 2u]));
    uint count = cull_words.data[state + 6u];
    bool same = cull_words.data[state + 3u] == cull_words.data[entry + 6u] && cull_words.data[state + 4u] == chunks
             && cull_words.data[state + 12u] == cull_words.data[entry + 8u];
    vec3 looked = vec3(uintBitsToFloat(cull_words.data[state + 17u]), uintBitsToFloat(cull_words.data[state + 18u]),
                       uintBitsToFloat(cull_words.data[state + 19u]));
    bool turned = cull_words.data[state + 20u] != 0u && dot(looked, look) < 0.94;
    if (same && count != 0u && !turned && length(cam - viewpoint) <= pc.refresh * length(away)) {
        uint first = atomicAdd(cull_words.data[cut + 13u], count);
        if (first + count <= cull_words.data[cut + 16u]) {
            atomicMax(cull_words.data[cut + 28u], first + count);
            cull_words.data[state + 7u] = cull_words.data[state + 5u];
            cull_words.data[state + 5u] = first;
            // Its tree goes with its records. The node stores have room for every tree of
            // records that fit.
            cull_words.data[state + 11u] = cull_words.data[state + 10u];
            cull_words.data[state + 10u] = atomicAdd(cull_words.data[cut + 24u], velk_cut_tree_nodes(count));
            cull_words.data[state + 15u] = 0u;
            cull_words.data[cull_words.data[cut + 20u] + atomicAdd(cull_words.data[cut], 1u)] = index;
            return;
        }
        // No room: nothing for it this frame, and it chooses again on the next.
        atomicMax(cull_words.data[cut + 15u], first + count);
        cull_words.data[state + 6u] = 0u;
        cull_words.data[state + 16u] = 0u;
        return;
    }
    // Choosing again, its new records take over the light of its old ones that are the same
    // (entry, cluster and slot), which are in the last store: also when its members changed, as
    // when one of them moved, so that what stayed keeps its light.
    if (count != 0u) {
        cull_words.data[state + 7u] = cull_words.data[state + 5u];
        cull_words.data[state + 15u] = count;
    } else {
        cull_words.data[state + 15u] = 0u;
    }
    cull_words.data[state] = floatBitsToUint(cam.x);
    cull_words.data[state + 1u] = floatBitsToUint(cam.y);
    cull_words.data[state + 2u] = floatBitsToUint(cam.z);
    cull_words.data[state + 17u] = floatBitsToUint(look.x);
    cull_words.data[state + 18u] = floatBitsToUint(look.y);
    cull_words.data[state + 19u] = floatBitsToUint(look.z);
    cull_words.data[state + 20u] = 0u;
    cull_words.data[state + 3u] = cull_words.data[entry + 6u];
    cull_words.data[state + 4u] = chunks;
    cull_words.data[state + 12u] = cull_words.data[entry + 8u];
    cull_words.data[state + 6u] = 0u;
    // Nothing for rays until its tree pass lists its new records, which it may never get to
    // when the store has no room for them.
    cull_words.data[state + 16u] = 0u;
    cull_words.data[state + 8u] = 1u;
    atomicAdd(cull_words.data[cut + 14u], 1u);
    uint at = atomicAdd(cull_words.data[cut + 4u], chunks);
    cull_words.data[state + 9u] = at;
    uint list = cull_words.data[cut + 21u];
    uint first_chunk = cull_words.data[entry + 6u];
    for (uint c = 0u; c < chunks; ++c) {
        cull_words.data[list + (at + c) * 2u] = first_chunk + c * 64u;
        cull_words.data[list + (at + c) * 2u + 1u] = index;
    }
}

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= pc.entry_count) return;

    uint entry = pc.table_word + index * VELK_CULL_ENTRY_WORDS;
    uint chunks = cull_words.data[entry + 7u];

    vec3 lo = vec3(uintBitsToFloat(cull_words.data[entry + 0u]), uintBitsToFloat(cull_words.data[entry + 1u]),
                   uintBitsToFloat(cull_words.data[entry + 2u]));
    vec3 hi = vec3(uintBitsToFloat(cull_words.data[entry + 3u]), uintBitsToFloat(cull_words.data[entry + 4u]),
                   uintBitsToFloat(cull_words.data[entry + 5u]));

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
 * Starts a frame of a kept cut (kCutHeaderWords): the other store becomes current, and the
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
    cull_words.data[cut + 12u] = 1u - cull_words.data[cut + 12u];
    cull_words.data[cut + 13u] = 0u;
    cull_words.data[cut + 28u] = 0u;
    cull_words.data[cut + 24u] = 0u;
    // A new frame of the takeover table: what it held is gone.
    cull_words.data[cut + 29u] += 1u;
    cull_words.data[cut + 33u] = cull_words.data[cut + 32u];
    cull_words.data[cut + 32u] = 0u;
    cull_words.data[cut + 36u] = cull_words.data[cut + 34u];
    cull_words.data[cut + 37u] = cull_words.data[cut + 35u];
    cull_words.data[cut + 34u] = 0u;
    cull_words.data[cut + 35u] = 0u;
    cull_words.data[cut + 42u] = cull_words.data[cut + 40u];
    cull_words.data[cut + 43u] = cull_words.data[cut + 41u];
    cull_words.data[cut + 40u] = 0u;
    cull_words.data[cut + 41u] = 0u;
    cull_words.data[cut] = 0u;
    cull_words.data[cut + 4u] = 0u;
}
)";

/**
 * Copies the records of each partition that keeps them from the last store to its range in
 * the current one, and its tree from the last node store likewise, whose references are from
 * its own first record and node, so they hold, and its members' ranges from the last member
 * store. A work group per partition in the keep list.
 */
inline constexpr string_view instance_cut_copy_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint cut_word;
} pc;
)" VELK_CUT_GLSL R"(
void main()
{
    uint cut = pc.cut_word;
    uint state = velk_cut_state(cut, cull_words.data[cull_words.data[cut + 20u] + gl_WorkGroupID.x]);
    uint words = cull_words.data[state + 6u] * 4u;
    uint from = velk_cut_store(cut, false) + cull_words.data[state + 7u] * 4u;
    uint to = velk_cut_store(cut, true) + cull_words.data[state + 5u] * 4u;
    for (uint i = gl_LocalInvocationIndex; i < words; i += 64u) {
        cull_words.data[to + i] = cull_words.data[from + i];
    }
    // Its ray list, by its records' places, so it holds as they do.
    uint list_from = velk_cut_ray_store(cut, false) + cull_words.data[state + 7u];
    uint list_to = velk_cut_ray_store(cut, true) + cull_words.data[state + 5u];
    for (uint i = gl_LocalInvocationIndex; i < cull_words.data[state + 16u]; i += 64u) {
        cull_words.data[list_to + i] = cull_words.data[list_from + i];
    }
    uint node_words = velk_cut_tree_nodes(cull_words.data[state + 16u]) * 16u;
    uint node_from = velk_cut_node_store(cut, false) + cull_words.data[state + 11u] * 16u;
    uint node_to = velk_cut_node_store(cut, true) + cull_words.data[state + 10u] * 16u;
    for (uint i = gl_LocalInvocationIndex; i < node_words; i += 64u) {
        cull_words.data[node_to + i] = cull_words.data[node_from + i];
    }
}
)";

/**
 * Chooses the clusters of each instance in the chunks of the choose list, all round the camera,
 * from its partition's viewpoint. With `write` 0, counts them into the chunk's count and the
 * partition's, and lists the chunk's share of the partition's old records in the takeover table;
 * with 1, writes them from the chunk's first record, which instance_cut_place_src made of its
 * count, and each takes over the light of the old record that was the same (kCutPartitionWords
 * 15). A mesh without clusters gives one record for the whole instance. A work group per chunk,
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
)" VELK_CUT_GLSL R"(
uint g_count;
uint g_state;
uint g_store;
bool g_writing;
uint g_at;        // where this thread's next record goes, while writing
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
    uint table = cull_words.data[cut + 30u];
    uint mask = cull_words.data[cut + 31u] - 1u;
    uint stamp = cull_words.data[cut + 29u];
    uint h = velk_takeover_hash(cull_words.data[was], cull_words.data[was + 1u], cull_words.data[was + 2u]);
    for (uint p = 0u; p < VELK_TAKEOVER_PROBES; ++p) {
        uint s = table + ((h + p) & mask) * 2u;
        uint seen = cull_words.data[s];
        if (seen == stamp) continue;
        if (atomicCompSwap(cull_words.data[s], seen, stamp) == seen) {
            cull_words.data[s + 1u] = was;
            return;
        }
    }
}

// The word of the old record that is (entry, cluster, slot), or 0xFFFFFFFF.
uint velk_takeover_find(uint cut, uint entry, uint cluster, uint slot)
{
    uint table = cull_words.data[cut + 30u];
    uint mask = cull_words.data[cut + 31u] - 1u;
    uint stamp = cull_words.data[cut + 29u];
    uint h = velk_takeover_hash(entry, cluster, slot);
    for (uint p = 0u; p < VELK_TAKEOVER_PROBES; ++p) {
        uint s = table + ((h + p) & mask) * 2u;
        if (cull_words.data[s] != stamp) return 0xFFFFFFFFu;
        uint was = cull_words.data[s + 1u];
        if (cull_words.data[was] == entry && cull_words.data[was + 1u] == cluster && cull_words.data[was + 2u] == slot) return was;
    }
    return 0xFFFFFFFFu;
}

// Counts a record, or writes it at this thread's next place, with the light of the old record
// that was the same.
void velk_cut_record(uint entry, uint cluster, uint slot)
{
    if (g_writing) {
        uint at = g_at++;
        // Never past the range the count gave the partition.
        if (at >= cull_words.data[g_state + 5u] + cull_words.data[g_state + 6u]) return;
        uint record = g_store + at * 4u;
        cull_words.data[record] = entry;
        cull_words.data[record + 1u] = cluster;
        cull_words.data[record + 2u] = slot;
        // Whether rays leave it out, for the tree pass.
        uint mark = cull_words.data[pc.cut_word + 44u] + (at >> 5u);
        uint bit = 1u << (at & 31u);
        if (g_no_rays) atomicOr(cull_words.data[mark], bit);
        else atomicAnd(cull_words.data[mark], ~bit);
        uint light = 0xFFFFFFFFu;
        if (g_takeover) {
            uint was = velk_takeover_find(pc.cut_word, entry, cluster, slot);
            if (was != 0xFFFFFFFFu) {
                light = cull_words.data[was + 3u];
                ++g_took;
            }
        }
        cull_words.data[record + 3u] = light;
    }
    ++g_count;
}

void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     bool deforming)
{
    velk_cut_record(entry, c, slot);
}

// A cluster at the light's level over drawn ones finer than it: a record for light only.
void velk_lod_light_chosen(uint entry, uint c, uint slot)
{
    velk_cut_record(entry | VELK_CUT_LIGHT_ONLY, c, slot);
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
    mat4 world = cull_instances.data[velk_cut_named_instance(named)].world_matrix;
    GlobalData globals = velk_globals.data[pc.globals_base];
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
    mat4 world = cull_instances.data[velk_cut_named_instance(named)].world_matrix;
    float scale = velk_cut_scale(world);
    GlobalData globals = velk_globals.data[pc.globals_base];
    mat4 vp = globals.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * globals.viewport.y * 0.5;
    uint clusters = cull_words.data[entry + 2u];
    uint at = cull_words.data[entry + 8u] + node * VELK_LOD_NODE_STRIDE;
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
    uint first_entry = cull_words.data[table + 8u];
    uint entries = cull_words.data[table + 9u];
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
    velk_cut_choose(named, viewpoint, table_word + cull_instances.data[instance].params.y * 12u);
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
        float radius = uintBitsToFloat(cull_words.data[w + 10u]);
        if (radius < 0.0) break;
        vec3 center = vec3(uintBitsToFloat(cull_words.data[w]), uintBitsToFloat(cull_words.data[w + 1u]),
                           uintBitsToFloat(cull_words.data[w + 2u]));
        vec3 u = vec3(uintBitsToFloat(cull_words.data[w + 3u]), uintBitsToFloat(cull_words.data[w + 4u]),
                      uintBitsToFloat(cull_words.data[w + 5u]));
        vec3 v = vec3(uintBitsToFloat(cull_words.data[w + 6u]), uintBitsToFloat(cull_words.data[w + 7u]),
                      uintBitsToFloat(cull_words.data[w + 8u]));
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
        float radius = uintBitsToFloat(cull_words.data[w + 10u]);
        if (radius < 0.0) break;
        vec3 center = vec3(uintBitsToFloat(cull_words.data[w]), uintBitsToFloat(cull_words.data[w + 1u]),
                           uintBitsToFloat(cull_words.data[w + 2u]));
        vec3 u = vec3(uintBitsToFloat(cull_words.data[w + 3u]), uintBitsToFloat(cull_words.data[w + 4u]),
                      uintBitsToFloat(cull_words.data[w + 5u]));
        vec3 v = vec3(uintBitsToFloat(cull_words.data[w + 6u]), uintBitsToFloat(cull_words.data[w + 7u]),
                      uintBitsToFloat(cull_words.data[w + 8u]));
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
                && uintBitsToFloat(cull_words.data[g_tree + quad * VELK_QUAD_NODE_WORDS + 10u]) >= 0.0) {
                velk_cut_root(VELK_CHOOSE_RECORD, g_tree, VELK_CUT_QUAD, g_tree + quad * VELK_QUAD_NODE_WORDS, viewpoint);
            }
            return;
        }
    }
    if (g_leaves_only) return;
    // An instanced visual's object: its one item, by its id, through its visual's descriptor.
    uint at = pc.descriptors_word;
    for (uint k = 0u; k < pc.descriptor_count && k < 4096u; ++k) {
        uint base = cull_words.data[at + 7u];
        if (base != 0xFFFFFFFFu && member >= base && member - base < cull_words.data[at + 1u]) {
            uint item = cull_words.data[at] + member - base;
            uint w = pc.items_word + item * VELK_PLAIN_ITEM_WORDS;
            velk_cut_instance(item, cull_words.data[w], cull_words.data[w + 2u], viewpoint);
            return;
        }
        at += VELK_ITEM_DESCRIPTOR_HEADER_WORDS + min(cull_words.data[at + 4u], 4096u) * 3u;
    }
    // A plain object: each of its items. An object has a few at most; the bound only keeps a
    // broken chain from running on.
    uint object_slot = member & 0x3FFFFFFu;
    uint item = object_slot < pc.object_slots ? cull_words.data[pc.head_word + object_slot] : 0xFFFFFFFFu;
    for (uint n = 0u; n < 64u && item != 0xFFFFFFFFu; ++n) {
        if (cull_words.data[pc.objects_word + item] == member) {
            uint at = pc.items_word + item * VELK_PLAIN_ITEM_WORDS;
            velk_cut_instance(item, cull_words.data[at], cull_words.data[at + 2u], viewpoint);
        }
        item = cull_words.data[pc.next_word + item];
    }
}

void main()
{
    uint cut = pc.cut_word;
    uint chunk_index = gl_WorkGroupID.x;
    uint chunk = cull_words.data[cut + 21u] + chunk_index * 2u;
    uint lane = gl_LocalInvocationIndex;
    uint member = cull_words.data[cull_words.data[chunk] + lane];
    uint state = velk_cut_state(cut, cull_words.data[chunk + 1u]);
    uint chunk_word = cull_words.data[cut + 23u] + chunk_index;
    // A work group is one chunk of one partition, so this is the same for all its threads. A
    // partition that found no room chooses on the next frame.
    bool choosing = pc.write == 0u || cull_words.data[state + 8u] != 0u;
    vec3 viewpoint = vec3(uintBitsToFloat(cull_words.data[state]), uintBitsToFloat(cull_words.data[state + 1u]),
                          uintBitsToFloat(cull_words.data[state + 2u]));

    g_count = 0u;
    g_state = state;
    g_store = velk_cut_store(cut, true);
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
        uint entry = pc.partition_table + cull_words.data[chunk + 1u] * VELK_CULL_ENTRY_WORDS;
        uint leaves = cull_words.data[entry + 9u] & ~VELK_QUAD_LEAVES_ONLY;
        uint position = cull_words.data[chunk] - cull_words.data[entry + 6u] + lane;
        if (cull_words.data[entry + 8u] != 0xFFFFFFFFu && position < leaves) {
            g_tree = cull_words.data[entry + 8u];
            g_leaves = leaves;
            g_position = position;
            g_leaves_only = (cull_words.data[entry + 9u] & VELK_QUAD_LEAVES_ONLY) != 0u;
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
    if (g_out_of_view) cull_words.data[state + 20u] = 1u;

    if (pc.write == 0u) {
        // The longest walk and the most records of one walk, for the log.
        if (g_visited != 0u) atomicMax(cull_words.data[cut + 40u], g_visited);
        if (most != 0u) atomicMax(cull_words.data[cut + 41u], most);
        if (lane == 0u) {
            cull_words.data[chunk_word] = total;
            if (total != 0u) atomicAdd(cull_words.data[state + 6u], total);
        }
        // This chunk's share of the partition's old records goes in the takeover table, for its
        // records written next to find (kCutPartitionWords 7 and 15).
        uint old = cull_words.data[state + 15u];
        if (old != 0u) {
            uint chunks = cull_words.data[state + 4u];
            uint per = (old + chunks - 1u) / chunks;
            uint from = (chunk_index - cull_words.data[state + 9u]) * per;
            uint to = min(from + per, old);
            uint old_first = velk_cut_store(cut, false) + cull_words.data[state + 7u] * 4u;
            for (uint i = from + lane; i < to; i += 64u) velk_takeover_put(cut, old_first + i * 4u);
        }
        return;
    }

    // Written: each item from its place in the chunk's range, then the members alone. Each record
    // takes over the light of the old record that was the same.
    uint first = cull_words.data[chunk_word];
    g_writing = true;
    g_takeover = cull_words.data[state + 15u] != 0u;
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
    if (g_count != 0u) atomicAdd(cull_words.data[cut + 34u], g_count);
    if (g_took != 0u) atomicAdd(cull_words.data[cut + 35u], g_took);
}
)";

/**
 * Gives each partition that chose again this frame a range in the current store, as large as
 * its count, and each of its chunks its part of that, in order. One that does not fit gets no
 * records and chooses again on the next frame. A thread per partition.
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
void main()
{
    uint cut = pc.cut_word;
    uint index = gl_GlobalInvocationID.x;
    if (index >= cull_words.data[cut + 19u]) return;
    uint state = velk_cut_state(cut, index);
    if (cull_words.data[state + 8u] == 0u) return;
    uint count = cull_words.data[state + 6u];
    uint first = atomicAdd(cull_words.data[cut + 13u], count);
    if (first + count > cull_words.data[cut + 16u]) {
        atomicMax(cull_words.data[cut + 15u], first + count);
        cull_words.data[state + 6u] = 0u;
        cull_words.data[state + 8u] = 0u;
        return;
    }
    atomicMax(cull_words.data[cut + 28u], first + count);
    cull_words.data[state + 5u] = first;
    cull_words.data[state + 10u] = atomicAdd(cull_words.data[cut + 24u], velk_cut_tree_nodes(count));
    // Each of its chunks' first record, in the order of its chunks.
    uint counts = cull_words.data[cut + 23u] + cull_words.data[state + 9u];
    uint next = first;
    for (uint c = 0u; c < cull_words.data[state + 4u]; ++c) {
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
    uint entry = cull_words.data[record] & ~(velk_cut_light_only(cull_words.data[record]) ? VELK_CUT_LIGHT_ONLY : 0u);
    uint cluster = cull_words.data[record + 1u];
    uint slot = cull_words.data[record + 2u];
    if (entry == VELK_CUT_RAY_QUAD) {
        // A patch: its node's sphere, which holds all its members, in the world already.
        vec3 c = vec3(uintBitsToFloat(cull_words.data[cluster]), uintBitsToFloat(cull_words.data[cluster + 1u]),
                      uintBitsToFloat(cull_words.data[cluster + 2u]));
        float r = max(uintBitsToFloat(cull_words.data[cluster + 10u]), 0.0) + pad;
        lo = c - vec3(r);
        hi = c + vec3(r);
        return;
    }
    if (entry == VELK_CUT_QUAD) {
        // A quad: around its corners, in the world already.
        vec3 c = vec3(uintBitsToFloat(cull_words.data[cluster]), uintBitsToFloat(cull_words.data[cluster + 1u]),
                      uintBitsToFloat(cull_words.data[cluster + 2u]));
        vec3 u = vec3(uintBitsToFloat(cull_words.data[cluster + 3u]), uintBitsToFloat(cull_words.data[cluster + 4u]),
                      uintBitsToFloat(cull_words.data[cluster + 5u]));
        vec3 v = vec3(uintBitsToFloat(cull_words.data[cluster + 6u]), uintBitsToFloat(cull_words.data[cluster + 7u]),
                      uintBitsToFloat(cull_words.data[cluster + 8u]));
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
        pad += 2.0 * uintBitsToFloat(cull_words.data[item + 6u]);
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
    // A cluster's own sphere, or a whole mesh's: its table's.
    uint sphere = entry != 0xFFFFFFFFu ? cull_words.data[entry + 2u] + cluster * VELK_LOD_CLUSTER_WORDS + 13u : cluster;
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
 * leaves of four records in their order, which is near to near in the world, paired level by
 * level from the bottom, so that no sort is needed. `pad` grows every box, for instances that a
 * motion function moves, by twice how far it can move one, so that the tree holds wherever
 * they are while it is kept. A work group per partition.
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
)" VELK_CUT_GLSL VELK_CUT_BOX_GLSL R"(
shared uint s_counts[64];

// How wide on screen, from `from`, the error of the sphere at `word` (centre, radius, error) is.
float velk_ray_pixels(uint word, mat4 world, float scale, vec3 from, float pixels_per_unit)
{
    vec3 center = mat3(world) * vec3(uintBitsToFloat(cull_words.data[word]), uintBitsToFloat(cull_words.data[word + 1u]),
                                     uintBitsToFloat(cull_words.data[word + 2u])) + world[3].xyz - from;
    float away = length(center) - uintBitsToFloat(cull_words.data[word + 3u]) * scale;
    return uintBitsToFloat(cull_words.data[word + 4u]) * scale * pixels_per_unit / max(away, 1e-3);
}

// Whether rays trace `record`, the record at `index` of the current store: at the light's
// level, seen from `viewpoint` where its partition chose. A record for light only is, and a
// patch for rays; a record of a member in a patch is not, the patch standing for it; a cluster
// drawn finer than the light, whose parent is not coarser than the light, is not, since a record
// for light only covers it.
bool velk_ray_record(uint record, uint index, vec3 viewpoint, float pixels_per_unit)
{
    uint marks = cull_words.data[pc.cut_word + 44u];
    if ((cull_words.data[marks + (index >> 5u)] & (1u << (index & 31u))) != 0u) return false;
    uint entry = cull_words.data[record];
    if (entry >= VELK_CUT_RAY_QUAD || velk_cut_light_only(entry)) return true;
    uint slot = cull_words.data[record + 2u];
    uint instance = pc.instances_base + slot;
    if (pc.items_word != 0xFFFFFFFFu) instance = cull_words.data[pc.items_word + slot * VELK_PLAIN_ITEM_WORDS];
    if (instance == 0xFFFFFFFFu) return false;
    mat4 world = cull_instances.data[instance].world_matrix;
    float scale = sqrt(max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)), dot(world[2].xyz, world[2].xyz)));
    uint cluster = cull_words.data[entry + 2u] + cull_words.data[record + 1u] * VELK_LOD_CLUSTER_WORDS;
    return velk_ray_pixels(cluster + 8u, world, scale, viewpoint, pixels_per_unit) > pc.light_pixels;
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

void main()
{
    uint cut = pc.cut_word;
    if (gl_WorkGroupID.x >= cull_words.data[cut + 19u]) return;
    uint state = velk_cut_state(cut, gl_WorkGroupID.x);
    // The same for the whole group.
    if (cull_words.data[state + 8u] == 0u) return;
    uint records_count = cull_words.data[state + 6u];
    uint first_index = cull_words.data[state + 5u];
    uint records = velk_cut_store(cut, true) + first_index * 4u;
    uint nodes = velk_cut_node_store(cut, true) + cull_words.data[state + 10u] * 16u;
    uint list = velk_cut_ray_store(cut, true) + cull_words.data[state + 5u];
    uint lane = gl_LocalInvocationIndex;

    // The records rays trace, in their order: each thread a run of them, counted, then placed
    // after the runs before.
    GlobalData g = velk_globals.data[pc.globals_base];
    mat4 vp = g.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
    vec3 viewpoint = vec3(uintBitsToFloat(cull_words.data[state]), uintBitsToFloat(cull_words.data[state + 1u]),
                          uintBitsToFloat(cull_words.data[state + 2u]));
    uint per = (records_count + 63u) / 64u;
    uint from = min(lane * per, records_count);
    uint to = min(from + per, records_count);
    uint mine = 0u;
    for (uint r = from; r < to; ++r) {
        if (velk_ray_record(records + r * 4u, first_index + r, viewpoint, pixels_per_unit)) ++mine;
    }
    s_counts[lane] = mine;
    barrier();
    uint at = 0u;
    for (uint i = 0u; i < lane; ++i) at += s_counts[i];
    for (uint r = from; r < to; ++r) {
        if (velk_ray_record(records + r * 4u, first_index + r, viewpoint, pixels_per_unit)) cull_words.data[list + at++] = r;
    }
    uint count = 0u;
    for (uint i = 0u; i < 64u; ++i) count += s_counts[i];
    if (lane == 0u) cull_words.data[state + 16u] = count;
    memoryBarrierBuffer();
    barrier();
    if (count == 0u) return;

    uint below = (count + 3u) / 4u;
    uint below_first = 0u;
    uint level_first = 0u;
    bool from_leaves = true;
    uint level = 1u;
    while (true) {
        uint here = (below + 1u) / 2u;
        for (uint i = gl_LocalInvocationIndex; i < here; i += 64u) {
            uint node = nodes + (level_first + i) * 16u;
            for (uint k = 0u; k < 2u; ++k) {
                uint child = i * 2u + k;
                vec3 lo = vec3(1e30);
                vec3 hi = vec3(-1e30);
                uint ref = 0xFFFFFFFFu;
                if (child < below && from_leaves) {
                    uint first = child * 4u;
                    uint n = min(4u, count - first);
                    for (uint r = 0u; r < n; ++r) {
                        vec3 rlo, rhi;
                        velk_cut_record_box(records + cull_words.data[list + first + r] * 4u, pc.instances_base, pc.items_word,
                                            pc.pad, rlo, rhi);
                        lo = min(lo, rlo);
                        hi = max(hi, rhi);
                    }
                    ref = 0x80000000u | (first * 4u + n - 1u);
                } else if (child < below) {
                    uint at = nodes + (below_first + child) * 16u;
                    lo = min(velk_load_vec3(at), velk_load_vec3(at + 6u));
                    hi = max(velk_load_vec3(at + 3u), velk_load_vec3(at + 9u));
                    ref = below_first + child;
                }
                velk_store_box(node + k * 6u, lo, hi);
                cull_words.data[node + 12u + k] = ref;
            }
            cull_words.data[node + 14u] = level;
            cull_words.data[node + 15u] = here == 1u ? 1u : 0u;
        }
        memoryBarrierBuffer();
        barrier();
        if (here == 1u) break;
        below_first = level_first;
        level_first += here;
        below = here;
        from_leaves = false;
        ++level;
    }
}
)";

/// A view's top tree for rays, over the partitions of all its kept cuts, in the culling arena:
/// kRayTopHeaderWords (how many cuts, leaves and nodes, and the root node's index), then a
/// cut each, kRayTopCutWords (its word, its instances_base, its plain items' word or
/// kCullNoSlot, its partition count, its first leaf, 1 if a motion function moves its
/// instances, the view's quad list or kCullNoSlot), then a leaf each, two words (the cut's
/// index, the partition's), then the nodes, kCutNodeWords each as a partition's are, but a
/// reference with kCutLeaf names a leaf, and without a node of this tree by its index.
inline constexpr uint32_t kRayTopHeaderWords = 4;
inline constexpr uint32_t kRayTopCutWords = 8;

/// Nodes in a tree over @p leaves leaves paired level by level. Mirrors velk_top_tree_nodes.
inline uint32_t top_tree_nodes(uint32_t leaves)
{
    if (leaves == 0) {
        return 0;
    }
    uint32_t below = leaves;
    uint32_t total = 0;
    do {
        below = (below + 1u) / 2u;
        total += below;
    } while (below > 1u);
    return total;
}

/**
 * Builds a view's top tree for rays (kRayTopHeaderWords) over the trees of its kept cuts'
 * partitions, as they are this frame: each leaf's box is its partition's root's, empty for one
 * without records. Leaves are in the order of the cuts and of each cut's partition table, which
 * is near to near. One work group.
 */
inline constexpr string_view instance_cut_top_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 256) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint top_word;
} pc;
)" VELK_CUT_GLSL R"(
vec3 velk_load_vec3(uint at)
{
    return vec3(uintBitsToFloat(cull_words.data[at]), uintBitsToFloat(cull_words.data[at + 1u]),
                uintBitsToFloat(cull_words.data[at + 2u]));
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

void main()
{
    uint top = pc.top_word;
    uint cuts = cull_words.data[top];
    uint leaves = cull_words.data[top + 1u];
    if (leaves == 0u) return;
    uint leaf_word = top + 4u + cuts * 8u;
    uint nodes = leaf_word + leaves * 2u;

    uint below = leaves;
    uint below_first = 0u;
    uint level_first = 0u;
    bool from_leaves = true;
    uint level = 1u;
    while (true) {
        uint here = (below + 1u) / 2u;
        for (uint i = gl_LocalInvocationIndex; i < here; i += 256u) {
            uint node = nodes + (level_first + i) * 16u;
            for (uint k = 0u; k < 2u; ++k) {
                uint child = i * 2u + k;
                vec3 lo = vec3(1e30);
                vec3 hi = vec3(-1e30);
                uint ref = 0xFFFFFFFFu;
                if (child < below && from_leaves) {
                    // A partition: its root's box, if it has records rays trace.
                    uint cut = cull_words.data[top + 4u + cull_words.data[leaf_word + child * 2u] * 8u];
                    uint state = velk_cut_state(cut, cull_words.data[leaf_word + child * 2u + 1u]);
                    uint count = cull_words.data[state + 16u];
                    // One without records is not there for rays: no ref, which the walk skips.
                    if (count != 0u) {
                        uint root = velk_cut_node_store(cut, true)
                                  + (cull_words.data[state + 10u] + velk_cut_tree_nodes(count) - 1u) * 16u;
                        lo = min(velk_load_vec3(root), velk_load_vec3(root + 6u));
                        hi = max(velk_load_vec3(root + 3u), velk_load_vec3(root + 9u));
                        ref = 0x80000000u | child;
                    }
                } else if (child < below) {
                    // A node over nothing for rays is not there either.
                    uint at = nodes + (below_first + child) * 16u;
                    lo = min(velk_load_vec3(at), velk_load_vec3(at + 6u));
                    hi = max(velk_load_vec3(at + 3u), velk_load_vec3(at + 9u));
                    if (cull_words.data[at + 12u] != 0xFFFFFFFFu || cull_words.data[at + 13u] != 0xFFFFFFFFu) {
                        ref = below_first + child;
                    }
                }
                velk_store_box(node + k * 6u, lo, hi);
                cull_words.data[node + 12u + k] = ref;
            }
            cull_words.data[node + 14u] = level;
            cull_words.data[node + 15u] = here == 1u ? 1u : 0u;
        }
        memoryBarrierBuffer();
        barrier();
        if (here == 1u) break;
        below_first = level_first;
        level_first += here;
        below = here;
        from_leaves = false;
        ++level;
    }
}
)";

/**
 * Writes the group counts of the dispatch that draws from the current store: a group per 64
 * records it holds, no more than fit. One thread.
 */
inline constexpr string_view instance_cut_args_src = R"(
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
    uint records = cull_words.data[cut + 28u];
    cull_words.data[cut + 8u] = (records + 63u) / 64u;
}
)";

/**
 * Draws from a kept cut: each record of the current store whose instance is near enough, and
 * whose cluster is in view and not hidden behind what was drawn last frame, goes where
 * instance_cull_compute_src puts what it chooses: compute raster, the visual's chosen list, or
 * the second occlusion phase. A whole instance record adds its slot to its mesh's visible list,
 * and a quad in view goes to the view's quad list. A thread per record. With `whole` set,
 * every record is drawn wherever it is, for a view that shows another camera's choice from
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
)" VELK_CUT_GLSL VELK_OCCLUSION_GLSL VELK_TINY_ADD_GLSL R"(
uint g_instance;
uint g_table;

void velk_lod_use(uint entry, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[pc.chosen_word], 1u);
    if (at >= pc.chosen_capacity) return;
    uint record = pc.chosen_word + 4u + at * 4u;
    cull_words.data[record] = entry;
    cull_words.data[record + 1u] = cluster;
    cull_words.data[record + 2u] = slot;
    cull_words.data[record + 3u] = cut_record;
    atomicAdd(cull_words.data[cull_words.data[entry + 4u] + cluster], 1u);
}

// Lists a cluster for the second test, false when there is no room for it. Levels of detail
// were chosen already, so where from no longer matters: the camera stands in.
bool velk_retest(uint entry, uint cluster, uint slot, uint cut_record, vec3 cam)
{
    uint at = atomicAdd(cull_words.data[pc.retest_word], 1u);
    if (at >= pc.retest_capacity) return false;
    uint record = pc.retest_word + 1u + at * 12u;
    cull_words.data[record] = g_instance;
    cull_words.data[record + 1u] = slot;
    cull_words.data[record + 2u] = g_table;
    cull_words.data[record + 3u] = entry;
    cull_words.data[record + 4u] = cluster;
    cull_words.data[record + 5u] = cut_record;
    cull_words.data[record + 6u] = pc.chosen_word;
    cull_words.data[record + 7u] = pc.chosen_capacity;
    cull_words.data[record + 8u] = floatBitsToUint(cam.x);
    cull_words.data[record + 9u] = floatBitsToUint(cam.y);
    cull_words.data[record + 10u] = floatBitsToUint(cam.z);
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
    uint index = gl_GlobalInvocationID.x;
    if (index >= cull_words.data[cut + 28u]) return;
    uint record = velk_cut_store(cut, true) + index * 4u;
    uint entry = cull_words.data[record];
    uint cluster_or_table = cull_words.data[record + 1u];
    uint slot = cull_words.data[record + 2u];

    // For light only: counted, not drawn. A patch for rays only: not drawn.
    if (velk_cut_light_only(entry)) {
        atomicAdd(cull_words.data[cut + 32u], 1u);
        return;
    }
    if (entry == VELK_CUT_RAY_QUAD) return;

    // A quad: in view, it goes to the view's quad list, never past its room, its record in the
    // column after.
    if (entry == VELK_CUT_QUAD) {
        if (pc.quads_word == 0xFFFFFFFFu) return;
        atomicAdd(cull_words.data[pc.quads_word + 7u], 1u);
        uint w = cluster_or_table;
        GlobalData globals = velk_globals.data[pc.globals_base];
        vec3 center = vec3(uintBitsToFloat(cull_words.data[w]), uintBitsToFloat(cull_words.data[w + 1u]),
                           uintBitsToFloat(cull_words.data[w + 2u]))
                    - globals.cam_pos.xyz;
        float radius = uintBitsToFloat(cull_words.data[w + 10u]) + pc.quad_pad;
        if (pc.whole == 0u && velk_cut_outside(globals.view_projection, center, radius)) return;
        uint at = atomicAdd(cull_words.data[pc.quads_word + 1u], 1u);
        if (at < pc.quad_capacity) {
            cull_words.data[pc.quads_word + 8u + at] = w;
            cull_words.data[pc.quads_word + 8u + pc.quad_capacity + at] = record;
        } else {
            atomicAdd(cull_words.data[pc.quads_word + 1u], 0xFFFFFFFFu);
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
        slot = cull_words.data[at + 1u];
        table_word = cull_words.data[at + 2u];
        // Rays only: drawn by its own pass, whose pixels find their record by item.
        if (table_word == 0xFFFFFFFFu) {
            cull_words.data[pc.item_records_word + item] = record;
            return;
        }
        if (instance == 0xFFFFFFFFu) return;
    }
    mat4 world = cull_instances.data[instance].world_matrix;
    if (world[3][3] == 0.0) return;
    uint table = table_word + cull_instances.data[instance].params.y * 12u;
    GlobalData globals = velk_globals.data[pc.globals_base];
    vec3 cam = globals.cam_pos.xyz;
    mat4 vp = globals.view_projection;
    float scale2 = max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)), dot(world[2].xyz, world[2].xyz));
    float scale = sqrt(scale2);

    // The whole instance: near enough.
    vec3 center = velk_rel_position(world, vec3(uintBitsToFloat(cull_words.data[table]), uintBitsToFloat(cull_words.data[table + 1u]),
                                                uintBitsToFloat(cull_words.data[table + 2u])), globals);
    float radius = uintBitsToFloat(cull_words.data[table + 3u]) * scale;
    if (pc.max_distance > 0.0 && length(center) - radius > pc.max_distance) return;
    g_instance = instance;
    g_table = table_word;

    if (entry == 0xFFFFFFFFu) {
        if (pc.whole == 0u && velk_cut_outside(vp, center, radius)) return;
        uint at = atomicAdd(cull_words.data[cluster_or_table + 4u], 1u);
        uint capacity = cull_words.data[cluster_or_table + 6u];
        if (at < capacity) {
            uint list = cull_words.data[cluster_or_table + 5u];
            cull_words.data[list + at] = slot;
            cull_words.data[list + capacity + at] = record;
        }
        return;
    }

    uint c = cluster_or_table;
    uint cluster = cull_words.data[entry + 2u] + c * VELK_LOD_CLUSTER_WORDS;
    // A deforming mesh's spheres are where it is at rest, which says nothing about where it is.
    bool deforming = (cull_words.data[entry + 9u] & VELK_LOD_DEFORMING) != 0u;
    if (!deforming && pc.whole == 0u) {
        float own_radius = uintBitsToFloat(cull_words.data[cluster + 16u]) * scale;
        if (velk_cut_outside(vp, velk_lod_center(cluster + 13u, world, cam), own_radius)) return;
        // Tested where it was last frame, against what was drawn then.
        float test_radius = uintBitsToFloat(cull_words.data[cluster + 6u]) * scale;
        if (pc.pyramid_word != 0xFFFFFFFFu
            && velk_behind_pyramid(pc.pyramid_word, globals.prev_view_projection,
                                   velk_lod_center(cluster + 3u, world, cam) + globals.cam_moved, test_radius)
            && velk_retest(entry, c, slot, record, cam)) {
            return;
        }
    }
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * globals.viewport.y * 0.5;
    velk_lod_take(entry, c, slot, record, world, scale, cam, cam, pixels_per_unit);
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
)" VELK_OCCLUSION_GLSL VELK_TINY_ADD_GLSL R"(
uint g_instance;
uint g_table;
vec3 g_lod_cam;

void velk_lod_use(uint entry, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[pc.chosen_word], 1u);
    if (at >= pc.chosen_capacity) return;
    uint record = pc.chosen_word + 4u + at * 4u;
    cull_words.data[record] = entry;
    cull_words.data[record + 1u] = cluster;
    cull_words.data[record + 2u] = slot;
    cull_words.data[record + 3u] = cut_record;
    atomicAdd(cull_words.data[cull_words.data[entry + 4u] + cluster], 1u);
}

// Lists something for the second test, false when there is no room for it.
bool velk_retest(uint entry, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[pc.retest_word], 1u);
    if (at >= pc.retest_capacity) return false;
    uint record = pc.retest_word + 1u + at * 12u;
    cull_words.data[record] = g_instance;
    cull_words.data[record + 1u] = slot;
    cull_words.data[record + 2u] = g_table;
    cull_words.data[record + 3u] = entry;
    cull_words.data[record + 4u] = cluster;
    cull_words.data[record + 5u] = cut_record;
    cull_words.data[record + 6u] = pc.chosen_word;
    cull_words.data[record + 7u] = pc.chosen_capacity;
    cull_words.data[record + 8u] = floatBitsToUint(g_lod_cam.x);
    cull_words.data[record + 9u] = floatBitsToUint(g_lod_cam.y);
    cull_words.data[record + 10u] = floatBitsToUint(g_lod_cam.z);
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
    uint cluster = cull_words.data[entry + 2u] + c * VELK_LOD_CLUSTER_WORDS;
    if (deforming || !velk_lod_occluded(cluster + 3u, world, scale, cam, entry, c, slot)) {
        velk_lod_take(entry, c, slot, 0xFFFFFFFFu, world, scale, cam, lod_cam, pixels_per_unit);
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
        slot = cull_words.data[item + 1u];
        table_word = cull_words.data[item + 2u];
        // Rays only: drawn by its own pass. Or an instance without a place in its raster region.
        if (table_word == 0xFFFFFFFFu || instance == 0xFFFFFFFFu) return;
    }

    mat4 world = cull_instances.data[instance].world_matrix;
    if (world[3][3] == 0.0) return;

    uint table = table_word + cull_instances.data[instance].params.y * 12u;
    vec4 sphere = vec4(uintBitsToFloat(cull_words.data[table + 0u]),
                       uintBitsToFloat(cull_words.data[table + 1u]),
                       uintBitsToFloat(cull_words.data[table + 2u]),
                       uintBitsToFloat(cull_words.data[table + 3u]));
    vec3 center = velk_rel_position(world, sphere.xyz, globals);
    float scale2 = max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)),
                       dot(world[2].xyz, world[2].xyz));
    float radius = sphere.w * sqrt(scale2);
    uint first_entry = cull_words.data[table + 8u];
    uint entries = cull_words.data[table + 9u];
    // A deforming mesh's sphere is where it was at rest, which says nothing about whether it
    // is in view or hidden.
    bool deforming = entries != 0u && (cull_words.data[first_entry + 9u] & VELK_LOD_DEFORMING) != 0u;

    if (!deforming && pc.max_distance > 0.0 && length(center) - radius > pc.max_distance) return;

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
        if (abs(clip.x) > clip.w + w_reach + radius * length(r0)) return;
        if (abs(clip.y) > clip.w + w_reach + radius * length(r1)) return;
        if (abs(clip.z) > clip.w + w_reach + radius * length(r2)) return;
    }

    g_instance = instance;
    g_table = table_word;
    if (!deforming && pc.pyramid_word != 0xFFFFFFFFu
        && velk_behind_pyramid(pc.pyramid_word, globals.prev_view_projection, center + globals.cam_moved, radius)
        && velk_retest(0xFFFFFFFFu, 0u, slot, 0xFFFFFFFFu)) {
        return;
    }

    if (pc.items_word == 0xFFFFFFFFu) {
        uint index = atomicAdd(cull_words.data[table + 4u], 1u);
        uint capacity = cull_words.data[table + 6u];
        if (index < capacity) {
            uint list = cull_words.data[table + 5u];
            cull_words.data[list + index] = slot;
            cull_words.data[list + capacity + index] = 0xFFFFFFFFu;
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
    uint at = atomicAdd(cull_words.data[g_chosen_word], 1u);
    if (at >= g_chosen_capacity) return;
    uint record = g_chosen_word + 4u + at * 4u;
    cull_words.data[record] = entry;
    cull_words.data[record + 1u] = cluster;
    cull_words.data[record + 2u] = slot;
    cull_words.data[record + 3u] = cut_record;
    atomicAdd(cull_words.data[cull_words.data[entry + 4u] + cluster], 1u);
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
    uint cluster = cull_words.data[entry + 2u] + c * VELK_LOD_CLUSTER_WORDS;
    if (deforming || !velk_lod_occluded(cluster + 3u, world, scale, cam)) {
        velk_lod_take(entry, c, slot, 0xFFFFFFFFu, world, scale, cam, lod_cam, pixels_per_unit);
    }
}

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= min(cull_words.data[pc.retest_word], pc.retest_capacity)) return;

    uint record = pc.retest_word + 1u + index * 12u;
    uint instance = cull_words.data[record];
    uint slot = cull_words.data[record + 1u];
    uint table_word = cull_words.data[record + 2u];
    uint entry = cull_words.data[record + 3u];
    g_chosen_word = cull_words.data[record + 6u];
    g_chosen_capacity = cull_words.data[record + 7u];
    vec3 lod_cam = vec3(uintBitsToFloat(cull_words.data[record + 8u]), uintBitsToFloat(cull_words.data[record + 9u]),
                        uintBitsToFloat(cull_words.data[record + 10u]));

    mat4 world = cull_instances.data[instance].world_matrix;
    float scale = sqrt(max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)),
                           dot(world[2].xyz, world[2].xyz)));
    GlobalData globals = velk_globals.data[pc.globals_base];
    vec3 cam = globals.cam_pos.xyz;

    if (entry != 0xFFFFFFFFu) {
        uint cluster = cull_words.data[record + 4u];
        uint sphere = cull_words.data[entry + 2u] + cluster * VELK_LOD_CLUSTER_WORDS + 3u;
        if (!velk_lod_occluded(sphere, world, scale, cam)) {
            mat4 vp = globals.view_projection;
            float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * globals.viewport.y * 0.5;
            velk_lod_take(entry, cluster, slot, cull_words.data[record + 5u], world, scale, cam, lod_cam, pixels_per_unit);
        }
        return;
    }

    uint table = table_word + cull_instances.data[instance].params.y * 12u;
    vec3 local_center = vec3(uintBitsToFloat(cull_words.data[table + 0u]), uintBitsToFloat(cull_words.data[table + 1u]),
                             uintBitsToFloat(cull_words.data[table + 2u]));
    float radius = uintBitsToFloat(cull_words.data[table + 3u]) * scale;
    if (velk_behind_pyramid(pc.pyramid_word, globals.view_projection, velk_rel_position(world, local_center, globals), radius)) {
        return;
    }

    // After the positions the first test filled. A table without a list has no capacity.
    uint position = cull_words.data[table + 4u] + atomicAdd(cull_words.data[table + 11u], 1u);
    uint capacity = cull_words.data[table + 6u];
    if (position < capacity) {
        uint list = cull_words.data[table + 5u];
        cull_words.data[list + position] = slot;
        cull_words.data[list + capacity + position] = 0xFFFFFFFFu;
    }

    uint first_entry = cull_words.data[table + 8u];
    uint entries = cull_words.data[table + 9u];
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
        cull_words.data[pc.chosen_word + 1u] = (cull_words.data[pc.chosen_word] + 63u) / 64u;
        cull_words.data[pc.chosen_word + 2u] = 1u;
        cull_words.data[pc.chosen_word + 3u] = 1u;
    }

    uint capacity = cull_words.data[entry + 1u];
    uint table = cull_words.data[entry + 2u];
    uint clusters = cull_words.data[entry + 3u];
    uint counts = cull_words.data[entry + 4u];
    uint cursors = cull_words.data[entry + 5u];
    uint commands = cull_words.data[entry + (pc.second_phase != 0u ? 11u : 6u)];

    uint first = pc.second_phase != 0u ? cull_words.data[entry + 10u] : 0u;
    uint draws = 0u;
    for (uint c = 0u; c < clusters; ++c) {
        uint count = min(cull_words.data[counts + c], capacity - first);
        cull_words.data[counts + c] = 0u;
        cull_words.data[cursors + c] = first;
        if (count != 0u) {
            uint cluster = table + c * VELK_LOD_CLUSTER_WORDS;
            uint command = commands + draws * 5u;
            cull_words.data[command + 0u] = cull_words.data[cluster + 1u] * 3u;
            cull_words.data[command + 1u] = count;
            cull_words.data[command + 2u] = cull_words.data[cluster];
            cull_words.data[command + 3u] = 0u;
            cull_words.data[command + 4u] = first;
            ++draws;
        }
        first += count;
    }
    if (pc.second_phase != 0u) {
        cull_words.data[entry + 12u] = draws;
    } else {
        cull_words.data[entry + 7u] = draws;
        cull_words.data[entry + 10u] = first;
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
    if (index >= min(cull_words.data[pc.chosen_word], pc.chosen_capacity)) return;
    uint record = pc.chosen_word + 4u + index * 4u;
    uint entry = cull_words.data[record];
    uint at = atomicAdd(cull_words.data[cull_words.data[entry + 5u] + cull_words.data[record + 1u]], 1u);
    uint capacity = cull_words.data[entry + 1u];
    if (at < capacity) {
        uint list = cull_words.data[entry];
        cull_words.data[list + at] = cull_words.data[record + 2u];
        cull_words.data[list + capacity + at] = cull_words.data[record + 3u];
        cull_words.data[list + capacity * 2u + at] = cull_words.data[entry + 2u] + cull_words.data[record + 1u] * VELK_LOD_CLUSTER_WORDS;
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
    uint count = min(tiny_words.data[pc.list_word], pc.capacity);
    uint row = clamp(count, 1u, 65535u);
    tiny_words.data[pc.list_word + 1u] = count == 0u ? 0u : row;
    tiny_words.data[pc.list_word + 2u] = (count + row - 1u) / row;
    tiny_words.data[pc.list_word + 3u] = 1u;
    tiny_words.data[pc.list_word + 4u] = count;
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
 * one triangle. With 1, the triangle whose depth that is writes its ids (velk_pixel_ids) to
 * the two words a pixel after that plane. The resolve takes both from there.
 */
inline constexpr string_view instance_tiny_draw_src = R"(
#version 450
#include "velk.glsl"

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
} pc;

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
    if (index >= tiny_words.data[pc.list_word + 4u]) return;
    uint record = pc.list_word + 8u + index * 4u;
    uint draw = tiny_words.data[record];
    VelkDrawData d = velk_draw_data.data[draw];
    uint cluster = tiny_words.data[record + 1u];
    TinyInstance inst = tiny_instances.data[d.instances_base + tiny_words.data[record + 2u]];
    GlobalData g = velk_globals.data[d.globals_base];
    // The cluster's record says where its indices are, and the vertices they count from.
    uint first = tiny_words.data[cluster];
    uint vertices = tiny_words.data[cluster + 18u];
    uint count = tiny_words.data[cluster + 1u];
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
        uint tie = tiny_tie(index, t);
        vec3 depths = vec3(s[0].z, s[1].z, s[2].z);
        for (int y = from.y; y <= to.y; ++y) {
            for (int x = from.x; x <= to.x; ++x) {
                vec2 p = vec2(float(x), float(y)) + 0.5;
                vec3 b = vec3(tiny_edge(s[1].xy, s[2].xy, p), tiny_edge(s[2].xy, s[0].xy, p), tiny_edge(s[0].xy, s[1].xy, p)) / area;
                if (b.x < 0.0 || b.y < 0.0 || b.z < 0.0) continue;
                float depth = dot(b, depths);
                if (depth <= 0.0 || depth > 1.0) continue;
                if (texelFetch(velk_textures[nonuniformEXT(pc.depth_id)], ivec2(x, y), 0).r >= depth) continue;
                uint bits = (floatBitsToUint(depth) & ~0xFFu) | tie;
                uint pixel = uint(y) * pc.width + uint(x);
                if (pc.mode == 0u) {
                    atomicMax(tiny_words.data[pc.planes_word + pixel], bits);
                } else if (tiny_words.data[pc.planes_word + pixel] == bits) {
                    uint ids = pc.planes_word + plane + pixel * 2u;
                    tiny_words.data[ids] = (draw + 1u) | VELK_IDS_TINY | (t << VELK_IDS_TRIANGLE_SHIFT);
                    tiny_words.data[ids + 1u] = record;
                }
            }
        }
    }
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
        uint32_t capacity = 0;      ///< Records a store holds.
        uint32_t node_capacity = 0; ///< Tree nodes a node store holds.
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
    /// Visuals whose stores were too small, with the records they wanted: theirs are made
    /// again with room for those.
    vector<std::pair<const void*, uint32_t>> cut_growth;
    uint64_t cut_check_frame = 0;  ///< When the stores' needs were last read back.
    uint64_t cut_log_frame = 0;    ///< When the count of partitions choosing again was last logged.
    uint32_t cut_moves = 0;        ///< What the count was then.
    bool cut_moves_known = false;  ///< Whether that count is comparable with the next.
    /// The light of the kept cuts' records, which the culling frees each frame what no record
    /// names of, and the path gives records the first time it draws them (light_claim_src).
    LightPool light_pool;

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

/// Words per mesh in a batch's culling table, and where its first cluster entry and their count
/// are in it. Mirror DefaultBatch's table.
inline constexpr uint32_t kBatchTableWords = 12;
inline constexpr uint32_t kBatchFirstEntryWord = 8;
inline constexpr uint32_t kBatchEntryCountWord = 9;

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
        uint first = cull_words.data[at];
        uint slots = cull_words.data[at + 1u];
        if (item >= first && item - first < slots) {
            found = at;
            break;
        }
        at += VELK_ITEM_DESCRIPTOR_HEADER_WORDS + min(cull_words.data[at + 4u], 4096u) * 3u;
    }
    // What does not change with its place: its table, how far it can move, its object, and no
    // next item, since an instanced object has one item.
    uint slot = found != 0xFFFFFFFFu ? item - cull_words.data[found] : 0u;
    cull_words.data[w + 2u] = found != 0xFFFFFFFFu ? cull_words.data[found + 5u] : 0xFFFFFFFFu;
    cull_words.data[w + 6u] = found != 0xFFFFFFFFu ? cull_words.data[found + 6u] : 0u;
    uint object_base = found != 0xFFFFFFFFu ? cull_words.data[found + 7u] : 0xFFFFFFFFu;
    cull_words.data[pc.objects_word + item] = object_base != 0xFFFFFFFFu ? object_base + slot : 0xFFFFFFFFu;
    cull_words.data[pc.next_word + item] = 0xFFFFFFFFu;
    uint place = 0xFFFFFFFFu;
    if (found != 0xFFFFFFFFu && cull_words.data[found + 3u] != 0u) {
        place = cull_words.data[cull_words.data[found + 3u] + slot];
    }
    if (place == 0xFFFFFFFFu) {
        cull_words.data[w] = 0xFFFFFFFFu;
        cull_words.data[w + 1u] = 0xFFFFFFFFu;
        cull_words.data[w + 3u] = 0xFFFFFFFFu;
        cull_words.data[w + 4u] = 255u;
        cull_words.data[w + 5u] = 0xFFFFFFFFu;
        return;
    }
    uint instance = cull_words.data[found + 2u] + place;
    cull_words.data[w] = instance;
    cull_words.data[w + 1u] = place;
    uint mesh = cull_instances.data[instance].params.y;
    if (mesh < cull_words.data[found + 4u]) {
        uint m = found + VELK_ITEM_DESCRIPTOR_HEADER_WORDS + mesh * 3u;
        cull_words.data[w + 3u] = cull_words.data[m];
        cull_words.data[w + 4u] = cull_words.data[m + 1u];
        cull_words.data[w + 5u] = cull_words.data[m + 2u];
    }
}
)";

/**
 * Names a kept cut's records again after their batches were made again: each record of the last
 * store (the one the copy and the choose read from) gets its cluster entry, or a whole mesh's
 * table, from `pairs` (old word, new word, sorted by the old), its plain item from `items` (by
 * old item), or its instanced visual's item from `blocks`. A record whose words do not carry
 * over becomes kCutStale. A thread per record the store holds.
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
    uint index = gl_GlobalInvocationID.x;
    if (index >= cull_words.data[cut + 16u]) return;
    uint record = velk_cut_store(cut, false) + index * 4u;
    uint entry = cull_words.data[record];
    if (entry == VELK_CUT_QUAD || entry == VELK_CUT_RAY_QUAD || entry == VELK_CUT_STALE) return;
    bool stale = false;
    if (entry == 0xFFFFFFFFu) {
        uint table = cull_words.data[record + 1u];
        if (table != 0xFFFFFFFFu) {
            uint renamed = velk_rename(table);
            stale = renamed == VELK_CUT_STALE;
            cull_words.data[record + 1u] = renamed;
        }
    } else {
        uint renamed = velk_rename(entry & ~VELK_CUT_LIGHT_ONLY);
        stale = renamed == VELK_CUT_STALE;
        cull_words.data[record] = stale ? VELK_CUT_STALE : renamed | (entry & VELK_CUT_LIGHT_ONLY);
    }
    uint item = cull_words.data[record + 2u];
    if (item < pc.item_count) {
        uint renamed = cull_words.data[pc.items_word + item];
        stale = stale || renamed == 0xFFFFFFFFu;
        cull_words.data[record + 2u] = renamed;
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
        cull_words.data[record + 2u] = renamed;
    }
    // What does not carry over: its partition chooses again (instance_cut_stale_src).
    if (stale) cull_words.data[record] = VELK_CUT_STALE;
}
)";

/**
 * After a rename, makes each partition holding a record that did not carry over (kCutStale)
 * choose again this frame: its table entry's chunk word no longer matches, so the decide pass
 * chooses it from nothing, without taking over its old records' light. A work group per
 * partition, over its records in the last store, which the decide pass then reads.
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
    if (index >= cull_words.data[cut + 19u]) return;
    uint state = velk_cut_state(cut, index);
    uint first = cull_words.data[state + 5u];
    uint count = cull_words.data[state + 6u];
    if (count == 0u) return;
    if (gl_LocalInvocationIndex == 0u) s_stale = 0u;
    barrier();
    uint records = velk_cut_store(cut, false);
    uint end = min(first + count, cull_words.data[cut + 16u]);
    for (uint r = first + gl_LocalInvocationIndex; r < end; r += 64u) {
        if (cull_words.data[records + r * 4u] == VELK_CUT_STALE) s_stale = 1u;
    }
    barrier();
    if (gl_LocalInvocationIndex == 0u && s_stale != 0u) cull_words.data[state + 3u] = 0xFFFFFFFFu;
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
        cmd.record_dispatch_indirect(dc, arena_buffer, uint64_t(pc.chosen_word + 1u) * sizeof(uint32_t));
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

/// How many tree nodes a node store of @p state's kept cut of @p key holds, 0 without one.
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
                arena->read_at((uint64_t(info.table_word) + uint64_t(m) * kBatchTableWords + kBatchFirstEntryWord) * sizeof(uint32_t),
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
                items.push_back(instances_base + slot);
                items.push_back(slot);
                items.push_back(table_word);
                items.push_back(draw);
                items.push_back(kind);
                items.push_back(mesh);
                items.push_back(0u);
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
                // Drawn by its own pass, but rays see it: an item without a table, for the
                // plain objects' kept cut.
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
                arena->update_at(items_byte + (uint64_t(pending.first_item + k) * kPlainItemWords + 5u) * sizeof(uint32_t),
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
            descriptors[header] = first;
            descriptors[header + 1u] = slots;
            uint32_t meshes = 0;
            for (const auto& batch : *batches) {
                InstanceCullInfo info{};
                if (!batch || !batch->get_instance_cull(info) || !info.instanced || info.key != key) {
                    continue;
                }
                descriptors[header + 2u] = info.instances_base;
                descriptors[header + 3u] = info.slot_table_count >= slots ? info.slot_table_word : 0u;
                descriptors[header + 5u] = info.table_word;
                std::memcpy(&descriptors[header + 6u], &info.max_displacement, sizeof(uint32_t));
                descriptors[header + 7u] = info.object_base != 0u ? info.object_base : kCullNoSlot;
                if (info.mesh_index >= meshes) {
                    descriptors.resize(header + kItemDescriptorHeaderWords + size_t(info.mesh_index + 1u) * kItemMeshWords, 0u);
                    for (uint32_t m = meshes; m <= info.mesh_index; ++m) {
                        uint32_t* words = descriptors.data() + header + kItemDescriptorHeaderWords + size_t(m) * kItemMeshWords;
                        words[0] = kCullNoSlot;
                        words[1] = kRayShapeMesh;
                        words[2] = kInvalidMeshStaticBase;
                    }
                    meshes = info.mesh_index + 1u;
                }
                uint32_t* words = descriptors.data() + header + kItemDescriptorHeaderWords + size_t(info.mesh_index) * kItemMeshWords;
                batch_ray_words(*batch, ctx, words[0], words[1], words[2]);
            }
            descriptors[header + 4u] = meshes;
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

    // Whether a cut's stores were too small lately, now and then: reading the arena is slow,
    // and until they grow the partitions that do not fit choose again every frame.
    constexpr uint64_t kCutCheckFrames = 16;
    if (!state.cuts.empty() && ctx.present_counter >= state.cut_check_frame + kCutCheckFrames) {
        state.cut_check_frame = ctx.present_counter;
        for (const auto& cut : state.cuts) {
            uint32_t wanted = 0;
            arena->read_at(cut.region.offset() + 15u * sizeof(uint32_t), &wanted, sizeof(wanted));
            if (wanted > cut.capacity) {
                state.cut_growth.push_back({cut.key, wanted});
            }
        }
    }

    // The scene's partitions get a kept cut for this view, every partition choosing on its first
    // frame. It is made again when the partitions may have been laid out again, so that none
    // keeps records meant for another, or when its stores need room; else it carries over.
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
            cut.capacity = std::max({info.members * 2u + 4096u, wanted + wanted / 2u,
                                     old != state.cuts.end() ? old->capacity : 0u});
            // Every partition's chunks at most: each one's members, and a part-filled chunk each.
            const uint32_t chunks = (info.members + kCullChunkSlots - 1u) / kCullChunkSlots + info.partitions;
            const uint32_t keep_word = kCutHeaderWords;
            const uint32_t choose_word = keep_word + info.partitions;
            const uint32_t counts_word = choose_word + chunks * kCullChunkListWords;
            const uint32_t state_word = counts_word + chunks;
            const uint32_t store_word = state_word + info.partitions * kCutPartitionWords;
            // A tree has as many nodes as leaves of four records, one each, and one more per level
            // at most, from odd counts: fewer than 32 levels.
            const uint32_t node_capacity = cut.capacity / 4u + info.partitions * 33u;
            cut.node_capacity = node_capacity;
            const uint32_t node_store_word = store_word + cut.capacity * kCutRecordWords * 2u;
            // The takeover table: at least two slots a record, so that lookups stay short.
            uint32_t takeover_slots = 1;
            while (takeover_slots < cut.capacity * 2u) takeover_slots <<= 1;
            const uint32_t takeover_word = node_store_word + node_capacity * kCutNodeWords * 2u;
            // A ray list store, a word a record.
            const uint32_t ray_store_word = takeover_word + takeover_slots * kCutTakeoverWords;
            // The ray marks, a bit a record.
            const uint32_t marks_word = ray_store_word + cut.capacity * 2u;
            const size_t words = size_t(marks_word) + size_t(cut.capacity) / 32u + 1u;
            cut.region = arena->alloc(words * sizeof(uint32_t));
            if (!cut.region.valid()) {
                VELK_LOG(E, "instance culling: no room for a kept cut of %u records", cut.capacity);
                continue;
            }
            const auto base = static_cast<uint32_t>(cut.region.offset() / sizeof(uint32_t));
            vector<uint32_t> initial;
            initial.resize(store_word, 0u);
            for (uint32_t args : {0u, 4u, 8u}) {
                initial[args + 1u] = 1u;
                initial[args + 2u] = 1u;
            }
            initial[16] = cut.capacity;
            initial[17] = base + store_word;
            initial[18] = base + store_word + cut.capacity * kCutRecordWords;
            initial[19] = info.partitions;
            initial[20] = base + keep_word;
            initial[21] = base + choose_word;
            initial[22] = base + state_word;
            initial[23] = base + counts_word;
            initial[25] = node_capacity;
            initial[26] = base + node_store_word;
            initial[27] = base + node_store_word + node_capacity * kCutNodeWords;
            initial[30] = base + takeover_word;
            initial[31] = takeover_slots;
            initial[38] = base + ray_store_word;
            initial[39] = base + ray_store_word + cut.capacity;
            initial[44] = base + marks_word;
            // Chosen from nowhere, and from a table entry no partition has: all choose.
            constexpr float kFar = 1e30f;
            uint32_t far_bits;
            std::memcpy(&far_bits, &kFar, sizeof(far_bits));
            for (uint32_t p = 0; p < info.partitions; ++p) {
                uint32_t* partition = initial.data() + state_word + p * kCutPartitionWords;
                partition[0] = partition[1] = partition[2] = far_bits;
                partition[3] = kCullNoSlot;
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
                const uint32_t header[kQuadListHeaderWords] = {4u, 0u, 0u, 0u, 1u, kCullNoSlot, 0u, 0u};
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
        for (const auto& c : state.cuts) {
            uint32_t header[2] = {};
            arena->read_at(c.region.offset() + 13u * sizeof(uint32_t), header, sizeof(header));
            records += header[0];
            moves += header[1];
            partitions += c.partitions;
            uint32_t light = 0;
            arena->read_at(c.region.offset() + 33u * sizeof(uint32_t), &light, sizeof(light));
            light_only += light;
        }
        const auto frames = static_cast<double>(ctx.present_counter - state.cut_log_frame);
        if (state.cut_moves_known) {
            VELK_LOG(I, "kept cut: %u records, %u for light only; partitions choosing again: %.1f a frame, of %u table entries",
                     records, light_only, static_cast<double>(moves - state.cut_moves) / frames, partitions);
            if (state.quads.valid()) {
                uint32_t quads = 0;
                uint32_t in_cut = 0;
                arena->read_at(state.quads.offset() + sizeof(uint32_t), &quads, sizeof(quads));
                arena->read_at(state.quads.offset() + 7u * sizeof(uint32_t), &in_cut, sizeof(in_cut));
                VELK_LOG(I, "quads %u in cut, %u in view", in_cut, quads);
            }
        }
        state.cut_moves = moves;
        state.cut_moves_known = true;
        state.cut_log_frame = ctx.present_counter;
    }

    // The light pool, with a segment to start, and more when records asked for pages that were
    // not there. Its pages are swept every frame, so a new segment's are free on the next.
    if (!state.cuts.empty() && ensure_light_pool(state.light_pool, *arena, ctx.backend->device_memory_bytes())) {
        LightPool& pool = state.light_pool;
        if (pool.segments.empty()) {
            grow_light_pool(pool, *arena, 1u);
        } else if (ctx.present_counter >= pool.check_frame + 16u) {
            pool.check_frame = ctx.present_counter;
            uint32_t wanted = 0;
            arena->read_at(pool.header.offset() + 2u * sizeof(uint32_t), &wanted, sizeof(wanted));
            if (wanted != 0 && pool.segments.size() < pool.max_segments) {
                const uint32_t added = grow_light_pool(pool, *arena, (wanted + kLightSegmentPages - 1u) / kLightSegmentPages);
                VELK_LOG(I, "light pool +%u to %u pg", added, pool.pages());
            }
        }
        // What it holds, now and then, from the last frame's complete counts.
        if (pool.header.valid() && ctx.present_counter >= pool.log_frame + 30u) {
            pool.log_frame = ctx.present_counter;
            uint32_t counts[16] = {};
            arena->read_at(pool.header.offset(), counts, sizeof(counts));
            uint32_t written = 0;
            uint32_t took = 0;
            for (const auto& c : state.cuts) {
                uint32_t words[2] = {};
                arena->read_at(c.region.offset() + 36u * sizeof(uint32_t), words, sizeof(words));
                written += words[0];
                took += words[1];
            }
            uint32_t extra[5] = {};
            arena->read_at(pool.header.offset() + uint64_t(kLightPoolTraceWord) * sizeof(uint32_t), extra, sizeof(extra));
            uint32_t budget[15] = {};
            arena->read_at(pool.header.offset() + uint64_t(kLightPoolTraceWord + 26u) * sizeof(uint32_t), budget, sizeof(budget));
            const uint32_t no_changes = 0;
            arena->update_at(pool.header.offset() + uint64_t(kLightPoolTraceWord + 36u) * sizeof(uint32_t), &no_changes,
                             sizeof(no_changes));
            VELK_LOG(I, "light: %u pages; records %u paged (%u linked), %u one value; last frame %u searched, %u one values "
                        "traced, %u coarse points listed for a first trace, %u other points for a first trace and %u for a "
                        "later one, %u records changed since the last log, %u records "
                        "chosen again of which %u kept their light, %u lit pixels untraced; budget %u rays",
                     counts[1], counts[12], counts[14], counts[13], counts[15], extra[2], budget[14], budget[8], budget[9],
                     budget[10], written, took, extra[4], budget[0]);
            uint32_t longest = 0;
            uint32_t most = 0;
            for (const auto& c : state.cuts) {
                uint32_t words[2] = {};
                arena->read_at(c.region.offset() + 42u * sizeof(uint32_t), words, sizeof(words));
                longest = std::max(longest, words[0]);
                most = std::max(most, words[1]);
            }
            VELK_LOG(I, "choosing: last frame the longest walk of one thread visited %u hierarchy nodes, the most "
                        "records one walk chose %u",
                     longest, most);
            // The light trace's worst frame since the last log: its rays, and their walks' steps.
            uint32_t worst[4] = {};
            arena->read_at(pool.header.offset() + 88u * sizeof(uint32_t), worst, sizeof(worst));
            const uint32_t cleared[4] = {};
            arena->update_at(pool.header.offset() + 88u * sizeof(uint32_t), cleared, sizeof(cleared));
            VELK_LOG(I, "light trace: worst frame %u rays (%u for first traces), %u walk steps, %.0f steps a ray", worst[0],
                     worst[3], worst[1], worst[0] != 0 ? double(worst[1]) / double(worst[0]) : 0.0);
            // Since the last log: what a ray's walk tests, and which own points were traced.
            uint32_t sums[8] = {};
            arena->read_at(pool.header.offset() + 92u * sizeof(uint32_t), sums, sizeof(sums));
            const uint32_t zeros[8] = {};
            arena->update_at(pool.header.offset() + 92u * sizeof(uint32_t), zeros, sizeof(zeros));
            const double rays = sums[2] != 0 ? double(sums[2]) : 1.0;
            const double steps = double(sums[3]) * 16.0;
            const double tris = double(sums[0]) * 16.0;
            VELK_LOG(I, "light trace: a ray tests %.0f triangles, %.0f records, %.0f nodes; points traced %u first, %u later",
                     tris / rays, double(sums[1]) / rays, std::max(steps - tris - double(sums[1]), 0.0) / rays, sums[6], sums[7]);
            // Since the last log: where traces come from, a change or the view.
            uint32_t why[5] = {};
            arena->read_at(pool.header.offset() + 121u * sizeof(uint32_t), why, sizeof(why));
            arena->update_at(pool.header.offset() + 121u * sizeof(uint32_t), zeros, sizeof(why));
            VELK_LOG(I, "light causes: %u later traces after a change; points laid out for records without light %u, "
                        "again for records grown %u, for records grown from one value %u, for records for light only %u",
                     why[0], why[1], why[2], why[3], why[4]);
            // The ray lists: records rays trace against all records, from the partitions' states.
            uint32_t traced_records = 0;
            uint32_t all_records = 0;
            for (const auto& c : state.cuts) {
                uint32_t header[kCutHeaderWords] = {};
                arena->read_at(c.region.offset(), header, sizeof(header));
                all_records += header[28];
                vector<uint32_t> states(size_t(c.partitions) * kCutPartitionWords);
                arena->read_at(uint64_t(header[22]) * sizeof(uint32_t), states.data(), states.size() * sizeof(uint32_t));
                for (uint32_t p = 0; p < c.partitions; ++p) traced_records += states[size_t(p) * kCutPartitionWords + 16u];
            }
            VELK_LOG(I, "ray lists: %u of %u records", traced_records, all_records);
            // The deepest the walk's stack got since the last log, in the trace and the probes.
            uint32_t deepest[2] = {};
            arena->read_at(pool.header.offset() + 100u * sizeof(uint32_t), deepest, sizeof(deepest));
            arena->update_at(pool.header.offset() + 100u * sizeof(uint32_t), zeros, sizeof(deepest));
            VELK_LOG(I, "walk stack: deepest %u in the light trace, %u in the probes", deepest[0], deepest[1]);
            // Where a ray's walk spends its steps, since the last log (the same rays as above).
            uint32_t where[4] = {};
            arena->read_at(pool.header.offset() + 102u * sizeof(uint32_t), where, sizeof(where));
            arena->update_at(pool.header.offset() + 102u * sizeof(uint32_t), zeros, sizeof(where));
            VELK_LOG(I, "light trace: a ray walks %.0f top tree nodes, enters %.1f partitions, walks %.0f of their tree nodes "
                        "and %.0f mesh tree nodes",
                     double(where[0]) * 16.0 / rays, double(where[3]) / rays, double(where[1]) * 16.0 / rays,
                     double(where[2]) * 16.0 / rays);
            // The top tree's partitions: how many have records for rays, and how much their boxes
            // overlap, as the sum of their volumes over the volume of the box around them all.
            if (state.ray_top.valid()) {
                vector<uint32_t> top(size_t(state.ray_top.size() / sizeof(uint32_t)));
                arena->read_at(state.ray_top.offset(), top.data(), top.size() * sizeof(uint32_t));
                const uint32_t cut_count = top[0];
                const uint32_t leaves = top[1];
                const uint32_t node_count = top[2];
                const size_t nodes_at = kRayTopHeaderWords + size_t(cut_count) * kRayTopCutWords + size_t(leaves) * 2u;
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
        const uint64_t words = uint64_t(kRayTopHeaderWords) + uint64_t(cut_count) * kRayTopCutWords + uint64_t(leaves) * 2u
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
        auto copy_pipeline = pipeline_for(instance_cut_copy_src, 0x4375'7443'6f70'7920ULL);
        auto choose_pipeline = pipeline_for(instance_cut_choose_src, 0x4375'7443'686f'6f73ULL);
        auto place_pipeline = pipeline_for(instance_cut_place_src, 0x4375'7450'6c61'6365ULL);
        auto args_pipeline = pipeline_for(instance_cut_args_src, 0x4375'7441'7267'7320ULL);
        auto draw_pipeline = pipeline_for(instance_cut_draw_src, 0x4375'7444'7261'7720ULL);
        auto tree_pipeline = pipeline_for(instance_cut_tree_src, 0x4375'7454'7265'6520ULL);
        auto top_pipeline = pipeline_for(instance_cut_top_src, 0x4375'7454'6f70'2020ULL);
        auto mark_pipeline = pipeline_for(light_mark_src, 0x4c69'6768'744d'726bULL);
        auto sweep_pipeline = pipeline_for(light_sweep_src, 0x4c69'6768'7453'7770ULL);
        auto rename_pipeline = pipeline_for(instance_cut_rename_src, 0x4375'7452'656e'616dULL);
        auto items_pipeline = pipeline_for(instance_items_src, 0x4375'6c6c'4974'656dULL);
        auto stale_pipeline = pipeline_for(instance_cut_stale_src, 0x4375'7453'7461'6c65ULL);
        const bool have_pipelines = pipeline && partition_pipeline && runs_pipeline && fill_pipeline && begin_pipeline
                                 && copy_pipeline && choose_pipeline && place_pipeline && args_pipeline && draw_pipeline
                                 && tree_pipeline && top_pipeline && mark_pipeline && sweep_pipeline && rename_pipeline
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
            cmd->record_clear_buffer(arena_buffer, uint64_t(quads_word + 1u) * sizeof(uint32_t), sizeof(uint32_t));
            cmd->record_clear_buffer(arena_buffer, uint64_t(quads_word + 7u) * sizeof(uint32_t), sizeof(uint32_t));
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

        // The kept cuts: records copied for partitions that keep them, counted for those that
        // choose again, placed, written, then drawn from.
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
                dispatch(choose_pipeline.get(), &pc, sizeof(pc), 1u, job.cut + 4u);
            };
            for (const CutJob& job : jobs) {
                dispatch(copy_pipeline.get(), &job.cut, sizeof(job.cut), 1u, job.cut);
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
                dispatch(args_pipeline.get(), &job.cut, sizeof(job.cut), 1u);
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
                words.resize(size_t(kRayTopHeaderWords) + size_t(cut_count) * kRayTopCutWords + size_t(leaves) * 2u, 0u);
                words[0] = cut_count;
                words[1] = leaves;
                words[2] = node_count;
                words[3] = node_count - 1u;
                uint32_t leaf = 0;
                for (uint32_t c = 0; c < cut_count; ++c) {
                    const CutJob& job = jobs[c];
                    uint32_t* cut = words.data() + kRayTopHeaderWords + size_t(c) * kRayTopCutWords;
                    cut[0] = job.cut;
                    cut[1] = job.instances_base;
                    cut[2] = job.items_word;
                    cut[3] = job.partition_count;
                    cut[4] = leaf;
                    cut[5] = state.items_move ? 1u : 0u;
                    cut[6] = quads_word;
                    for (uint32_t p = 0; p < job.partition_count; ++p, ++leaf) {
                        uint32_t* at = words.data() + kRayTopHeaderWords + size_t(cut_count) * kRayTopCutWords + size_t(leaf) * 2u;
                        at[0] = c;
                        at[1] = p;
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

            // The records are what they are this frame: the light pages none of them names are
            // free again.
            if (light_pool_word != kCullNoSlot) {
                const uint32_t begin_pc[2] = {light_pool_word, 1u};
                dispatch(sweep_pipeline.get(), begin_pc, sizeof(begin_pc), 1u);
                cmd->record_global_barrier();
                for (const CutJob& job : jobs) {
                    const uint32_t mark_pc[2] = {job.cut, light_pool_word};
                    dispatch(mark_pipeline.get(), mark_pc, sizeof(mark_pc), 1u, job.cut + 8u);
                }
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
        for (const CutJob& job : jobs) {
            const DrawPushC pc{job.cut, job.instances_base, job.table_word, view_globals_base, job.max_distance,
                               kLodMaxErrorPixels, job.chosen_word, job.chosen_capacity,
                               pyramid_word, retest_word, state.retest_capacity,
                               state.tiny_word, state.tiny_capacity, state.tiny_pixels, whole_cut ? 1u : 0u, job.items_word,
                               quads_word, state.quad_capacity, job.pad, item_records_word};
            dispatch(draw_pipeline.get(), &pc, sizeof(pc), 1u, job.cut + 8u);
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
        rec.held.push_back(std::move(copy_pipeline));
        rec.held.push_back(std::move(choose_pipeline));
        rec.held.push_back(std::move(place_pipeline));
        rec.held.push_back(std::move(tree_pipeline));
        rec.held.push_back(std::move(top_pipeline));
        rec.held.push_back(std::move(args_pipeline));
        rec.held.push_back(std::move(draw_pipeline));
        rec.held.push_back(std::move(mark_pipeline));
        rec.held.push_back(std::move(sweep_pipeline));
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
