#ifndef VELK_RENDER_FRAME_INSTANCE_CULL_H
#define VELK_RENDER_FRAME_INSTANCE_CULL_H

#include <velk/api/velk.h>
#include <velk/string_view.h>
#include <velk/vector.h>

#include <velk-render/api/cached_view_pass.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_batch.h>
#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/render_path/frame_context.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace velk {

/// Slots in a chunk of a partition's list. A partition's last chunk is filled out with
/// kCullNoSlot.
inline constexpr uint32_t kCullChunkSlots = 64;
inline constexpr uint32_t kCullNoSlot = 0xFFFFFFFFu;
/// Words per partition in a partition table. See instance_partition_cull_src.
inline constexpr uint32_t kCullEntryWords = 8;
/// Words before the chunks in a chunk list. See instance_partition_cull_src.
inline constexpr uint32_t kCullChunkListHeaderWords = 4;
/// Words per chunk in a chunk list: the word of its first slot, and the partition it is from.
inline constexpr uint32_t kCullChunkListWords = 2;
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
///   13     records in the current store
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
///            current store, 7 its first record in the last store, or while choosing a cursor,
///            8 1 while choosing this frame
/// A record is kCutRecordWords: entry, cluster, slot and error, as the chosen lists have them;
/// for a mesh without clusters the whole instance: entry kCullNoSlot, cluster its mesh's table.
inline constexpr uint32_t kCutHeaderWords = 24;
inline constexpr uint32_t kCutPartitionWords = 12;
inline constexpr uint32_t kCutRecordWords = 4;

// Where a cut's parts are. The shader declares cull_words.
#define VELK_CUT_GLSL R"(
uint velk_cut_state(uint cut, uint index)
{
    return cull_words.data[cut + 22u] + index * 12u;
}

// The current store, or the last frame's.
uint velk_cut_store(uint cut, bool current)
{
    uint parity = cull_words.data[cut + 12u];
    return cull_words.data[cut + 17u + (current ? parity : 1u - parity)];
}
)"

// Whether an instance is too small on screen to be worth drawing, in which case it goes
// through instance_splat_src instead. Each one changes over at its own size, so that no
// line shows where they do, by its `id`, which stays the same wherever the instance is in
// memory. `area` is its mean projected area, 0 if it never changes over.
#define VELK_INSTANCE_FAR_GLSL R"(
const float kFarInstancePixels = 2.0;

bool velk_instance_is_far(uint id, float area, float clip_w, mat4 vp, float height)
{
    if (area <= 0.0 || clip_w <= 0.0) return false;
    float f = length(vec3(vp[0][1], vp[1][1], vp[2][1]));
    float pixels = sqrt(area) * f * height * 0.5 / clip_w;
    float h = float((id * 2654435761u) >> 16u) / 65536.0;
    return pixels * (0.7 + 0.6 * h) < kFarInstancePixels;
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
/// entry kCullNoSlot for the whole instance, the cluster's error, where the visual's chosen
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
/// of its cluster in the level of detail table, its instance's slot under that draw, and its
/// surface error in world units as a float.
inline constexpr uint32_t kTinyHeaderWords = 8;
inline constexpr uint32_t kTinyRecordWords = 4;

// Adds a cluster to the list of those drawn in compute, false when it is full. The shader has
// pc.tiny_word and pc.tiny_capacity.
#define VELK_TINY_ADD_GLSL R"(
bool velk_tiny_add(uint draw, uint cluster, uint slot, float error)
{
    uint at = atomicAdd(cull_words.data[pc.tiny_word], 1u);
    if (at >= pc.tiny_capacity) return false;
    uint record = pc.tiny_word + 8u + at * 4u;
    cull_words.data[record] = draw;
    cull_words.data[record + 1u] = cluster;
    cull_words.data[record + 2u] = slot;
    cull_words.data[record + 3u] = floatBitsToUint(error);
    return true;
}
)"

// velk_lod_visit calls velk_lod_chosen, with its error in world units, for each cluster of a
// primitive's hierarchy that this instance uses: those whose own error on screen is small
// enough and whose parent's is not. A parent's sphere holds its children's and its error is
// not less than theirs, so going up from any triangle the answer changes once, which is what
// picks one complete surface. The shader declares cull_words, lod_nodes and
//   void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam,
//                        vec3 lod_cam, float pixels_per_unit, float error, bool deforming);
// first. `entry` is as at instance_cluster_runs_src. Clusters are found through the tree over
// them, which skips those under a node when none could qualify.
#define VELK_LOD_SELECT_GLSL R"(
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

// `cam` is the camera, which occlusion and compute raster go by; `lod_cam` where levels of
// detail are chosen from: the camera, or its partition's viewpoint.
void velk_lod_visit(uint entry, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, mat4 vp, float height,
                    float max_pixels)
{
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * height * 0.5;
    float per_error = scale * pixels_per_unit;
    uint clusters = cull_words.data[entry + 2u];
    uint nodes = cull_words.data[entry + 8u];
    // A deforming mesh's spheres are where its clusters are at rest, which says nothing
    // about whether they are hidden.
    bool deforming = (cull_words.data[entry + 9u] & VELK_LOD_DEFORMING) != 0u;
    // The tree's nodes are among the mesh's nodes that rays walk, in lod_nodes. A node's
    // child is 8 words: a sphere holding the spheres the clusters under it are tested with,
    // the smallest of their errors, the largest of their parents', and what it is. The tree
    // is no deeper than kLodMaxDepth.
    uint stack[32];
    int sp = 0;
    stack[sp++] = 0u;
    while (sp > 0) {
        uint node = nodes + stack[--sp] * VELK_LOD_NODE_STRIDE;
        for (uint k = 0u; k < 2u; ++k) {
            uint child = node + k * 8u;
            uint ref = lod_nodes.data[child + 6u];
            if (ref == VELK_LOD_NO_CHILD) continue;
            // Skipped when all under it are too coarse, or all have a parent that will do.
            vec3 center = vec3(uintBitsToFloat(lod_nodes.data[child]), uintBitsToFloat(lod_nodes.data[child + 1u]),
                               uintBitsToFloat(lod_nodes.data[child + 2u]));
            float dist = length(mat3(world) * center + (world[3].xyz - lod_cam));
            float reach = uintBitsToFloat(lod_nodes.data[child + 3u]) * scale;
            if (uintBitsToFloat(lod_nodes.data[child + 4u]) * per_error / (dist + reach) > max_pixels) continue;
            if (uintBitsToFloat(lod_nodes.data[child + 5u]) * per_error / max(dist - reach, 1e-3) <= max_pixels) continue;
            if ((ref & VELK_LOD_LEAF) == 0u) {
                if (sp < 32) stack[sp++] = ref;
                continue;
            }

            uint c = ref & ~VELK_LOD_LEAF;
            uint cluster = clusters + c * VELK_LOD_CLUSTER_WORDS;
            if (velk_lod_pixels(cluster + 3u, world, scale, lod_cam, pixels_per_unit) <= max_pixels
                && velk_lod_pixels(cluster + 8u, world, scale, lod_cam, pixels_per_unit) > max_pixels) {
                float error = uintBitsToFloat(cull_words.data[cluster + 7u]) * scale;
                velk_lod_chosen(entry, c, slot, world, scale, cam, lod_cam, pixels_per_unit, error, deforming);
            }
        }
    }
}
)"

// Draws chosen cluster `c` of `entry`: in compute when its mean triangle on screen, the
// width of the sphere around its own triangles over the square root of their count, is
// less than pc.tiny_pixels, its largest triangle fits the most compute raster covers (4
// times that, see instance_tiny_draw_src), and the list has room. Else by the hardware: also
// while its draw is not known yet, and when its material moves vertices. `cam` is the camera,
// which compute raster decides by; `lod_cam` where levels of detail were chosen from. Comes
// after VELK_LOD_SELECT_GLSL; the shader declares pc.tiny_pixels, pc.lod_max_pixels,
// velk_lod_use and velk_tiny_add.
#define VELK_LOD_TAKE_GLSL R"(
void velk_lod_take(uint entry, uint c, uint slot, float error, mat4 world, float scale, vec3 cam, vec3 lod_cam,
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
            && velk_tiny_add(draw, cluster, slot, error)) {
            return;
        }
    }
    velk_lod_use(entry, c, slot, error);
}
)"

/**
 * Tests a visual's partitions against the view and appends the chunks of those that
 * pass to a list, for instance_cull_compute_src to go through. A table entry holds the
 * partition's world bounds (min xyz and max xyz, as floats), the word of its first
 * chunk, and its chunk count, 0 for an unused entry. The list at list_word starts with
 * the group counts of the dispatch that follows, and its first word is the chunk count;
 * each chunk is kCullChunkListWords: its first slot's word and its partition's index.
 *
 * With cut_word set, it also decides for every partition, in view or not, whether its kept cut
 * (kCutHeaderWords) holds: it does while the camera is no further from where the partition
 * chose than `refresh` times the camera's distance from the partition, and the partition's
 * table entry is as it was. A partition that keeps its records gets a range for them in the
 * current store and goes to the keep list; any other chooses again from the camera, with its
 * chunks in the choose list. So a partition's choice is never off by more than that share.
 */
inline constexpr string_view instance_partition_cull_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint table_word;
    uint entry_count;
    uint list_word;
    uint globals_base;
    float max_distance;
    uint cut_word;
    float refresh;
} pc;
)" VELK_CUT_GLSL R"(
// Keeps partition `index`'s records, or lists it to choose again. `away` is from the camera
// to its bounds.
void velk_cut_decide(uint index, uint entry, uint chunks, vec3 cam, vec3 away)
{
    uint cut = pc.cut_word;
    uint state = velk_cut_state(cut, index);
    cull_words.data[state + 8u] = 0u;
    if (chunks == 0u) {
        cull_words.data[state + 6u] = 0u;
        return;
    }
    vec3 viewpoint = vec3(uintBitsToFloat(cull_words.data[state]), uintBitsToFloat(cull_words.data[state + 1u]),
                          uintBitsToFloat(cull_words.data[state + 2u]));
    uint count = cull_words.data[state + 6u];
    bool same = cull_words.data[state + 3u] == cull_words.data[entry + 6u] && cull_words.data[state + 4u] == chunks;
    if (same && count != 0u && length(cam - viewpoint) <= pc.refresh * length(away)) {
        uint first = atomicAdd(cull_words.data[cut + 13u], count);
        if (first + count <= cull_words.data[cut + 16u]) {
            cull_words.data[state + 7u] = cull_words.data[state + 5u];
            cull_words.data[state + 5u] = first;
            cull_words.data[cull_words.data[cut + 20u] + atomicAdd(cull_words.data[cut], 1u)] = index;
            return;
        }
        // No room: nothing for it this frame, and it chooses again on the next.
        atomicMax(cull_words.data[cut + 15u], first + count);
        cull_words.data[state + 6u] = 0u;
        return;
    }
    cull_words.data[state] = floatBitsToUint(cam.x);
    cull_words.data[state + 1u] = floatBitsToUint(cam.y);
    cull_words.data[state + 2u] = floatBitsToUint(cam.z);
    cull_words.data[state + 3u] = cull_words.data[entry + 6u];
    cull_words.data[state + 4u] = chunks;
    cull_words.data[state + 6u] = 0u;
    cull_words.data[state + 8u] = 1u;
    atomicAdd(cull_words.data[cut + 14u], 1u);
    uint at = atomicAdd(cull_words.data[cut + 4u], chunks);
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

    uint entry = pc.table_word + index * 8u;
    uint chunks = cull_words.data[entry + 7u];

    vec3 lo = vec3(uintBitsToFloat(cull_words.data[entry + 0u]), uintBitsToFloat(cull_words.data[entry + 1u]),
                   uintBitsToFloat(cull_words.data[entry + 2u]));
    vec3 hi = vec3(uintBitsToFloat(cull_words.data[entry + 3u]), uintBitsToFloat(cull_words.data[entry + 4u]),
                   uintBitsToFloat(cull_words.data[entry + 5u]));

    GlobalData globals = velk_globals.data[pc.globals_base];
    lo -= globals.cam_pos.xyz;
    hi -= globals.cam_pos.xyz;
    vec3 away = max(max(lo, -hi), vec3(0.0));

    if (pc.cut_word != 0xFFFFFFFFu) {
        velk_cut_decide(index, entry, chunks, globals.cam_pos.xyz, away);
    }
    if (chunks == 0u) return;

    if (pc.max_distance > 0.0 && length(away) > pc.max_distance) return;

    // Outside if the corner furthest along a plane's normal is behind it. Near and far
    // use the wider of the two conventions for clip z.
    mat4 vp = globals.view_projection;
    vec4 r0 = vec4(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    vec4 r1 = vec4(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    vec4 r2 = vec4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    vec4 r3 = vec4(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
    vec4 planes[6] = vec4[6](r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2);
    for (int p = 0; p < 6; ++p) {
        vec3 corner = mix(lo, hi, greaterThanEqual(planes[p].xyz, vec3(0.0)));
        if (dot(planes[p].xyz, corner) + planes[p].w < 0.0) return;
    }

    uint first = cull_words.data[entry + 6u];
    uint at = atomicAdd(cull_words.data[pc.list_word], chunks);
    for (uint c = 0u; c < chunks; ++c) {
        cull_words.data[pc.list_word + 4u + (at + c) * 2u] = first + c * 64u;
        cull_words.data[pc.list_word + 4u + (at + c) * 2u + 1u] = index;
    }
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
    cull_words.data[cut] = 0u;
    cull_words.data[cut + 4u] = 0u;
}
)";

/**
 * Copies the records of each partition that keeps them from the last store to its range in
 * the current one. A work group per partition in the keep list.
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
}
)";

/**
 * Chooses the clusters of each instance in the chunks of the choose list, all round the camera,
 * from its partition's viewpoint. With `write` 0, adds how many there are to the partition's
 * count; with 1, writes them into the partition's range, which the count gave it room for. A
 * mesh without clusters gives one record for the whole instance. A work group per chunk.
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
} pc;
)" VELK_CUT_GLSL R"(
uint g_count;
uint g_state;
uint g_store;

// Counts a record, or writes it at the next place in its partition's range.
void velk_cut_record(uint entry, uint cluster, uint slot, float error)
{
    if (pc.write != 0u) {
        uint at = atomicAdd(cull_words.data[g_state + 7u], 1u);
        // Never past the range the count gave the partition.
        if (at >= cull_words.data[g_state + 5u] + cull_words.data[g_state + 6u]) return;
        uint record = g_store + at * 4u;
        cull_words.data[record] = entry;
        cull_words.data[record + 1u] = cluster;
        cull_words.data[record + 2u] = slot;
        cull_words.data[record + 3u] = floatBitsToUint(error);
    }
    ++g_count;
}

void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     float error, bool deforming)
{
    velk_cut_record(entry, c, slot, error);
}
)" VELK_LOD_SELECT_GLSL R"(
// The same choice both times: what the count found room for is what is written.
void velk_cut_choose(uint slot, uint instance, vec3 viewpoint, mat4 world, float scale, uint table)
{
    GlobalData globals = velk_globals.data[pc.globals_base];
    uint first_entry = cull_words.data[table + 8u];
    uint entries = cull_words.data[table + 9u];
    if (entries == 0u) {
        velk_cut_record(0xFFFFFFFFu, table, slot, 0.0);
        return;
    }
    for (uint e = 0u; e < entries; ++e) {
        velk_lod_visit(first_entry + e * 14u, slot, world, scale, viewpoint, viewpoint, globals.view_projection,
                       globals.viewport.y, pc.lod_max_pixels);
    }
}

void main()
{
    uint cut = pc.cut_word;
    uint chunk = cull_words.data[cut + 21u] + gl_WorkGroupID.x * 2u;
    uint slot = cull_words.data[cull_words.data[chunk] + gl_LocalInvocationIndex];
    uint state = velk_cut_state(cut, cull_words.data[chunk + 1u]);
    if (slot >= pc.slot_count) return;
    // A partition that found no room chooses on the next frame.
    if (pc.write != 0u && cull_words.data[state + 8u] == 0u) return;

    uint instance = pc.instances_base + slot;
    mat4 world = cull_instances.data[instance].world_matrix;
    if (world[3][3] == 0.0) return;
    float scale = sqrt(max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)),
                           dot(world[2].xyz, world[2].xyz)));
    uint table = pc.table_word + cull_instances.data[instance].params.y * 12u;
    vec3 viewpoint = vec3(uintBitsToFloat(cull_words.data[state]), uintBitsToFloat(cull_words.data[state + 1u]),
                          uintBitsToFloat(cull_words.data[state + 2u]));

    g_count = 0u;
    g_state = state;
    g_store = velk_cut_store(cut, true);
    velk_cut_choose(slot, instance, viewpoint, world, scale, table);
    if (pc.write == 0u && g_count != 0u) atomicAdd(cull_words.data[state + 6u], g_count);
}
)";

/**
 * Gives each partition that chose again this frame a range in the current store, as large as
 * its count, with its cursor at the start. One that does not fit gets no records and chooses
 * again on the next frame. A thread per partition.
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
    cull_words.data[state + 5u] = first;
    cull_words.data[state + 7u] = first;
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
    uint records = min(cull_words.data[cut + 13u], cull_words.data[cut + 16u]);
    cull_words.data[cut + 8u] = (records + 63u) / 64u;
}
)";

/**
 * Draws from a kept cut: each record of the current store whose instance is near enough, not
 * too small to draw (far form), and whose cluster is in view and not hidden behind what was
 * drawn last frame, goes where instance_cull_compute_src puts what it chooses: compute raster,
 * the visual's chosen list, or the second occlusion phase. A whole instance record adds its
 * slot to its mesh's visible list. A thread per record. With `whole` set, every record is drawn
 * wherever it is, for a view that shows another camera's choice from outside.
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
    uint far_form;
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
} pc;
)" VELK_CUT_GLSL VELK_INSTANCE_FAR_GLSL VELK_OCCLUSION_GLSL VELK_TINY_ADD_GLSL R"(
uint g_instance;
uint g_table;

void velk_lod_use(uint entry, uint cluster, uint slot, float error)
{
    uint at = atomicAdd(cull_words.data[pc.chosen_word], 1u);
    if (at >= pc.chosen_capacity) return;
    uint record = pc.chosen_word + 4u + at * 4u;
    cull_words.data[record] = entry;
    cull_words.data[record + 1u] = cluster;
    cull_words.data[record + 2u] = slot;
    cull_words.data[record + 3u] = floatBitsToUint(error);
    atomicAdd(cull_words.data[cull_words.data[entry + 4u] + cluster], 1u);
}

// Lists a cluster for the second test, false when there is no room for it. Levels of detail
// were chosen already, so where from no longer matters: the camera stands in.
bool velk_retest(uint entry, uint cluster, uint slot, float error, vec3 cam)
{
    uint at = atomicAdd(cull_words.data[pc.retest_word], 1u);
    if (at >= pc.retest_capacity) return false;
    uint record = pc.retest_word + 1u + at * 12u;
    cull_words.data[record] = g_instance;
    cull_words.data[record + 1u] = slot;
    cull_words.data[record + 2u] = g_table;
    cull_words.data[record + 3u] = entry;
    cull_words.data[record + 4u] = cluster;
    cull_words.data[record + 5u] = floatBitsToUint(error);
    cull_words.data[record + 6u] = pc.chosen_word;
    cull_words.data[record + 7u] = pc.chosen_capacity;
    cull_words.data[record + 8u] = floatBitsToUint(cam.x);
    cull_words.data[record + 9u] = floatBitsToUint(cam.y);
    cull_words.data[record + 10u] = floatBitsToUint(cam.z);
    return true;
}

void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     float error, bool deforming)
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
    if (index >= min(cull_words.data[cut + 13u], cull_words.data[cut + 16u])) return;
    uint record = velk_cut_store(cut, true) + index * 4u;
    uint entry = cull_words.data[record];
    uint cluster_or_table = cull_words.data[record + 1u];
    uint slot = cull_words.data[record + 2u];
    float error = uintBitsToFloat(cull_words.data[record + 3u]);

    uint instance = pc.instances_base + slot;
    mat4 world = cull_instances.data[instance].world_matrix;
    if (world[3][3] == 0.0) return;
    uint table = pc.table_word + cull_instances.data[instance].params.y * 12u;
    GlobalData globals = velk_globals.data[pc.globals_base];
    vec3 cam = globals.cam_pos.xyz;
    mat4 vp = globals.view_projection;
    float scale2 = max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)), dot(world[2].xyz, world[2].xyz));
    float scale = sqrt(scale2);

    // The whole instance: near enough, and not left to the far form.
    vec3 center = velk_rel_position(world, vec3(uintBitsToFloat(cull_words.data[table]), uintBitsToFloat(cull_words.data[table + 1u]),
                                                uintBitsToFloat(cull_words.data[table + 2u])), globals);
    float radius = uintBitsToFloat(cull_words.data[table + 3u]) * scale;
    if (pc.max_distance > 0.0 && length(center) - radius > pc.max_distance) return;
    if (pc.far_form != 0u) {
        float area = uintBitsToFloat(cull_words.data[table + 7u]) * scale2;
        vec4 clip = vp * vec4(center, 1.0);
        if (velk_instance_is_far(cull_instances.data[instance].params.z, area, clip.w, vp, globals.viewport.y)) return;
    }
    g_instance = instance;
    g_table = pc.table_word;

    if (entry == 0xFFFFFFFFu) {
        if (pc.whole == 0u && velk_cut_outside(vp, center, radius)) return;
        uint at = atomicAdd(cull_words.data[cluster_or_table + 4u], 1u);
        if (at < cull_words.data[cluster_or_table + 6u]) {
            cull_words.data[cull_words.data[cluster_or_table + 5u] + at] = slot;
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
            && velk_retest(entry, c, slot, error, cam)) {
            return;
        }
    }
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * globals.viewport.y * 0.5;
    velk_lod_take(entry, c, slot, error, world, scale, cam, cam, pixels_per_unit);
}
)";

/**
 * Tests each instance's bounding sphere against the view and appends the visible
 * slots to its mesh's list in the culling arena, which the vertex shader reads through
 * velk_instance. The table at table_word holds 12 words per mesh: its bounding sphere
 * (center xyz and radius, as floats), the count of visible slots, the word of the
 * list, the list's capacity, its mean projected area at scale 1, and where the mesh's
 * primitives drawn by cluster are: the word of their entries, and how many, as described
 * at instance_cluster_runs_src. Each cluster a visible instance uses is counted there.
 * Instances with a zero matrix have been removed.
 *
 * For visuals and batches without a kept cut (kCutHeaderWords): partitioned visuals with one
 * choose and draw through instance_cut_choose_src and instance_cut_draw_src instead.
 *
 * With items_word set, one dispatch goes through the instances of many batches. An item
 * is 3 words: the instance's index in the instance arena, its slot, and its batch's table.
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
    uint far_form;
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
)" VELK_INSTANCE_FAR_GLSL VELK_OCCLUSION_GLSL VELK_TINY_ADD_GLSL R"(
uint g_instance;
uint g_table;
vec3 g_lod_cam;

void velk_lod_use(uint entry, uint cluster, uint slot, float error)
{
    uint at = atomicAdd(cull_words.data[pc.chosen_word], 1u);
    if (at >= pc.chosen_capacity) return;
    uint record = pc.chosen_word + 4u + at * 4u;
    cull_words.data[record] = entry;
    cull_words.data[record + 1u] = cluster;
    cull_words.data[record + 2u] = slot;
    cull_words.data[record + 3u] = floatBitsToUint(error);
    atomicAdd(cull_words.data[cull_words.data[entry + 4u] + cluster], 1u);
}

// Lists something for the second test, false when there is no room for it.
bool velk_retest(uint entry, uint cluster, uint slot, float error)
{
    uint at = atomicAdd(cull_words.data[pc.retest_word], 1u);
    if (at >= pc.retest_capacity) return false;
    uint record = pc.retest_word + 1u + at * 12u;
    cull_words.data[record] = g_instance;
    cull_words.data[record + 1u] = slot;
    cull_words.data[record + 2u] = g_table;
    cull_words.data[record + 3u] = entry;
    cull_words.data[record + 4u] = cluster;
    cull_words.data[record + 5u] = floatBitsToUint(error);
    cull_words.data[record + 6u] = pc.chosen_word;
    cull_words.data[record + 7u] = pc.chosen_capacity;
    cull_words.data[record + 8u] = floatBitsToUint(g_lod_cam.x);
    cull_words.data[record + 9u] = floatBitsToUint(g_lod_cam.y);
    cull_words.data[record + 10u] = floatBitsToUint(g_lod_cam.z);
    return true;
}

bool velk_lod_occluded(uint word, mat4 world, float scale, vec3 cam, uint entry, uint cluster, uint slot, float error);
void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     float error, bool deforming);
)" VELK_LOD_SELECT_GLSL VELK_LOD_TAKE_GLSL R"(
// Tested where it was last frame, against what was drawn then.
bool velk_lod_occluded(uint word, mat4 world, float scale, vec3 cam, uint entry, uint cluster, uint slot, float error)
{
    if (pc.pyramid_word == 0xFFFFFFFFu) return false;
    GlobalData g = velk_globals.data[pc.globals_base];
    float radius = uintBitsToFloat(cull_words.data[word + 3u]) * scale;
    return velk_behind_pyramid(pc.pyramid_word, g.prev_view_projection, velk_lod_center(word, world, cam) + g.cam_moved, radius)
        && velk_retest(entry, cluster, slot, error);
}

// Drawn unless hidden; a deforming mesh's spheres are where it is at rest, which says nothing
// about whether it is hidden.
void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     float error, bool deforming)
{
    uint cluster = cull_words.data[entry + 2u] + c * VELK_LOD_CLUSTER_WORDS;
    if (deforming || !velk_lod_occluded(cluster + 3u, world, scale, cam, entry, c, slot, error)) {
        velk_lod_take(entry, c, slot, error, world, scale, cam, lod_cam, pixels_per_unit);
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
        uint item = pc.items_word + item_index * 3u;
        instance = cull_words.data[item];
        slot = cull_words.data[item + 1u];
        table_word = cull_words.data[item + 2u];
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

    if (pc.far_form != 0u) {
        float area = uintBitsToFloat(cull_words.data[table + 7u]) * scale2;
        if (velk_instance_is_far(cull_instances.data[instance].params.z, area, clip.w, vp, globals.viewport.y)) return;
    }

    g_instance = instance;
    g_table = table_word;
    if (!deforming && pc.pyramid_word != 0xFFFFFFFFu
        && velk_behind_pyramid(pc.pyramid_word, globals.prev_view_projection, center + globals.cam_moved, radius)
        && velk_retest(0xFFFFFFFFu, 0u, slot, 0.0)) {
        return;
    }

    if (pc.items_word == 0xFFFFFFFFu) {
        uint index = atomicAdd(cull_words.data[table + 4u], 1u);
        if (index < cull_words.data[table + 6u]) {
            cull_words.data[cull_words.data[table + 5u] + index] = slot;
        }
    }

    for (uint e = 0u; e < entries; ++e) {
        velk_lod_visit(first_entry + e * 14u, slot, world, sqrt(scale2), globals.cam_pos.xyz, g_lod_cam, vp,
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

void velk_lod_use(uint entry, uint cluster, uint slot, float error)
{
    uint at = atomicAdd(cull_words.data[g_chosen_word], 1u);
    if (at >= g_chosen_capacity) return;
    uint record = g_chosen_word + 4u + at * 4u;
    cull_words.data[record] = entry;
    cull_words.data[record + 1u] = cluster;
    cull_words.data[record + 2u] = slot;
    cull_words.data[record + 3u] = floatBitsToUint(error);
    atomicAdd(cull_words.data[cull_words.data[entry + 4u] + cluster], 1u);
}

bool velk_lod_occluded(uint word, mat4 world, float scale, vec3 cam, uint entry, uint cluster, uint slot, float error);
void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     float error, bool deforming);
)" VELK_LOD_SELECT_GLSL VELK_LOD_TAKE_GLSL R"(
bool velk_lod_occluded(uint word, mat4 world, float scale, vec3 cam, uint entry, uint cluster, uint slot, float error)
{
    GlobalData g = velk_globals.data[pc.globals_base];
    float radius = uintBitsToFloat(cull_words.data[word + 3u]) * scale;
    return velk_behind_pyramid(pc.pyramid_word, g.view_projection, velk_lod_center(word, world, cam), radius);
}

void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     float error, bool deforming)
{
    uint cluster = cull_words.data[entry + 2u] + c * VELK_LOD_CLUSTER_WORDS;
    if (deforming || !velk_lod_occluded(cluster + 3u, world, scale, cam, entry, c, slot, error)) {
        velk_lod_take(entry, c, slot, error, world, scale, cam, lod_cam, pixels_per_unit);
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
        float error = uintBitsToFloat(cull_words.data[record + 5u]);
        if (!velk_lod_occluded(sphere, world, scale, cam, entry, cluster, slot, error)) {
            mat4 vp = globals.view_projection;
            float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * globals.viewport.y * 0.5;
            velk_lod_take(entry, cluster, slot, error, world, scale, cam, lod_cam, pixels_per_unit);
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
    if (position < cull_words.data[table + 6u]) {
        cull_words.data[cull_words.data[table + 5u] + position] = slot;
    }

    uint first_entry = cull_words.data[table + 8u];
    uint entries = cull_words.data[table + 9u];
    for (uint e = 0u; e < entries; ++e) {
        velk_lod_visit(first_entry + e * 14u, slot, world, scale, cam, lod_cam, globals.view_projection,
                       globals.viewport.y, pc.lod_max_pixels);
    }
}
)";

/**
 * Lays out one primitive's draws by cluster, once its instances are counted. Each cluster in
 * use gets an indexed command, instanced over its own run of the list, so that instances
 * using the same cluster share a draw. An entry is kClusterEntryWords: the word of the list, which
 * is followed by each position's surface error and its cluster's record, its capacity, the word of the cluster table
 * (kLodClusterWords words per cluster, starting with the word of the cluster's first index in
 * the mesh-word arena and its triangle count), the cluster count, the words of the counts, the cursors and the commands, one of
 * each per cluster, how many commands are in use, the word of the tree over its clusters and its flags, how
 * many positions the first phase used, the second phase's commands and how many are in use, and
 * the draw data record its pixels name (kClusterEntryDrawWord).
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

/**
 * Adds each instance too small to draw to the pixels it falls on, as its area in pixels,
 * shared over the 4 nearest so it moves smoothly. Goes through the chunks of the
 * partitions culling kept, or all slots without a chunk list. Instances behind something
 * nearer in the g-buffer are left out.
 */
inline constexpr string_view instance_splat_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct SplatInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer SplatInstances { SplatInstance data[]; } splat_instances;
layout(set = 1, binding = 16, std430) buffer SplatWords { uint data[]; } splat_words;

layout(push_constant) uniform PC {
    uint instances_base;
    uint slot_count;
    uint table_word;
    uint globals_base;
    uint chunks_word;
    uint words_word;
    uint width;
    uint height;
    uint worldpos_id;
    uint normal_id;
} pc;
)" VELK_INSTANCE_FAR_GLSL R"(
void add(ivec2 at, float amount, vec3 albedo)
{
    if (at.x < 0 || at.y < 0 || at.x >= int(pc.width) || at.y >= int(pc.height)) return;
    uint word = pc.words_word + uint(at.y) * pc.width + uint(at.x);
    uint plane = pc.width * pc.height;
    atomicAdd(splat_words.data[word], uint(amount * 4096.0));
    atomicAdd(splat_words.data[word + plane], uint(amount * albedo.r * 4096.0));
    atomicAdd(splat_words.data[word + plane * 2u], uint(amount * albedo.g * 4096.0));
    atomicAdd(splat_words.data[word + plane * 3u], uint(amount * albedo.b * 4096.0));
}

void main()
{
    uint slot = gl_GlobalInvocationID.x;
    if (pc.chunks_word != 0xFFFFFFFFu) {
        slot = splat_words.data[splat_words.data[pc.chunks_word + gl_WorkGroupID.x * 2u] + gl_LocalInvocationIndex];
    }
    if (slot >= pc.slot_count) return;
    mat4 world = splat_instances.data[pc.instances_base + slot].world_matrix;
    if (world[3][3] == 0.0) return;

    uint table = pc.table_word + splat_instances.data[pc.instances_base + slot].params.y * 12u;
    float mesh_area = uintBitsToFloat(splat_words.data[table + 7u]);
    if (mesh_area <= 0.0) return;
    vec3 local_center = vec3(uintBitsToFloat(splat_words.data[table + 0u]), uintBitsToFloat(splat_words.data[table + 1u]),
                             uintBitsToFloat(splat_words.data[table + 2u]));
    float scale2 = max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)),
                       dot(world[2].xyz, world[2].xyz));
    float area = mesh_area * scale2;

    GlobalData globals = velk_globals.data[pc.globals_base];
    mat4 vp = globals.view_projection;
    vec3 center = velk_rel_position(world, local_center, globals);
    vec4 clip = vp * vec4(center, 1.0);
    if (!velk_instance_is_far(splat_instances.data[pc.instances_base + slot].params.z, area, clip.w, vp, globals.viewport.y)) return;

    vec2 ndc = clip.xy / clip.w;
    if (any(greaterThan(abs(ndc), vec2(1.02)))) return;
    vec2 dims = vec2(float(pc.width), float(pc.height));
    vec2 px = (ndc * 0.5 + 0.5) * dims;

    vec2 uv = px / dims;
    vec3 n = velk_texture(pc.normal_id, uv).xyz;
    if (dot(n, n) > 1e-6) {
        float reach = uintBitsToFloat(splat_words.data[table + 3u]) * sqrt(scale2);
        vec3 p = velk_texture(pc.worldpos_id, uv).xyz;
        if (length(p) < length(center) - reach) return;
    }

    float f = length(vec3(vp[0][1], vp[1][1], vp[2][1]));
    float pixels_per_unit = f * dims.y * 0.5 / clip.w;
    float pixels = area * pixels_per_unit * pixels_per_unit;

    vec3 albedo = unpackUnorm4x8(splat_words.data[table + 10u]).rgb;
    vec2 q = px - 0.5;
    ivec2 base = ivec2(floor(q));
    vec2 t = q - vec2(base);
    add(base, pixels * (1.0 - t.x) * (1.0 - t.y), albedo);
    add(base + ivec2(1, 0), pixels * t.x * (1.0 - t.y), albedo);
    add(base + ivec2(0, 1), pixels * (1.0 - t.x) * t.y, albedo);
    add(base + ivec2(1, 1), pixels * t.x * t.y, albedo);
}
)";

/**
 * Adds partitions without their instances to the same coverage as instance_splat_src. What
 * is left of each is the top of its tree, treated as the medium rays treat it as: a pixel gets
 * the optical depth of its view ray through those leaves with this visual's medium_id, as far
 * as the surface behind. Going through the tree, this costs by the pixel, however many
 * partitions there are. Such a leaf has no children or shapes, its first_shape is the id, and
 * aabb_max.w its area.
 */
inline constexpr string_view instance_box_splat_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 1, binding = 0, std430) readonly buffer SplatNodes { BvhNode data[]; } splat_nodes;
layout(set = 1, binding = 16, std430) buffer SplatWords { uint data[]; } splat_words;

layout(push_constant) uniform PC {
    uint globals_base;
    uint words_word;
    uint width;
    uint height;
    uint worldpos_id;
    uint normal_id;
    uint medium_id;
} pc;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;
    GlobalData globals = velk_globals.data[pc.globals_base];
    if (globals.bvh_node_count == 0u) return;

    vec2 uv = (vec2(coord) + 0.5) / vec2(float(pc.width), float(pc.height));
    vec3 cam = globals.cam_pos.xyz;
    vec3 dir;
    float t_max = 1e30;
    vec3 n = velk_texture(pc.normal_id, uv).xyz;
    if (dot(n, n) > 1e-6) {
        vec3 to_surface = velk_texture(pc.worldpos_id, uv).xyz;
        t_max = length(to_surface);
        dir = to_surface / t_max;
    } else {
        vec4 far_point = globals.inverse_view_projection * vec4(uv * 2.0 - 1.0, 1e-4, 1.0);
        dir = normalize(far_point.xyz / far_point.w);
    }
    vec3 inv = 1.0 / dir;

    float depth = 0.0;
    vec3 depth_color = vec3(0.0);
    uint stack[64];
    int sp = 0;
    stack[sp++] = globals.bvh_root;
    while (sp > 0) {
        BvhNode node = splat_nodes.data[stack[--sp]];
        // What is resident is drawn or averaged instance by instance.
        if (node.child_count == VELK_PARTITION_REF) continue;

        vec3 t0 = (node.aabb_min.xyz - cam) * inv;
        vec3 t1 = (node.aabb_max.xyz - cam) * inv;
        vec3 small_t = min(t0, t1);
        vec3 large_t = max(t0, t1);
        float enter = max(max(max(small_t.x, small_t.y), small_t.z), 0.0);
        float leave = min(min(min(large_t.x, large_t.y), large_t.z), t_max);
        if (leave <= enter) continue;

        if (node.child_count != 0u) {
            for (uint c = 0u; c < node.child_count; ++c) {
                if (sp < 64) stack[sp++] = node.first_child + c;
            }
        } else if (node.shape_count == 0u && node.first_shape == pc.medium_id && node.aabb_max.w > 0.0) {
            vec3 extent = node.aabb_max.xyz - node.aabb_min.xyz;
            float widest = max(max(extent.x, extent.y), extent.z);
            vec3 sides = max(extent, vec3(widest * 1e-3));
            float leaf_depth = node.aabb_max.w / (sides.x * sides.y * sides.z) * (leave - enter);
            depth += leaf_depth;
            depth_color += leaf_depth * unpackUnorm4x8(floatBitsToUint(node.aabb_min.w)).rgb;
        }
    }
    if (depth > 0.0) {
        float capped = min(depth, 64.0);
        vec3 color = depth_color / depth * capped;
        uint word = pc.words_word + uint(coord.y) * pc.width + uint(coord.x);
        uint plane = pc.width * pc.height;
        atomicAdd(splat_words.data[word], uint(capped * 4096.0));
        atomicAdd(splat_words.data[word + plane], uint(color.r * 4096.0));
        atomicAdd(splat_words.data[word + plane * 2u], uint(color.g * 4096.0));
        atomicAdd(splat_words.data[word + plane * 3u], uint(color.b * 4096.0));
    }
}
)";

/**
 * Covers the frame by what instance_splat_src added up. Instances overlap at random, so
 * coverage saturates rather than adds.
 *
 * An instance is lit from which way its mesh's surface faces: of the surface turned to the
 * viewer, how much is turned to the light, and how far. Indirect light is what reaches the
 * surface behind it, from the lighting pass's surface light image, or the nearest surface
 * below for a pixel with only sky behind it. The directions are kSurfaceDirections, and
 * instances' own rotations are not taken into account.
 *
 * Direct light is shadowed by what the lighting pass found for that surface, in the alpha
 * of the same image: a share V of
 * it arrives after passing through the layer of instances, an optical depth tl = -ln V, and
 * the viewer sees through a depth tv, the coverage. The instances seen, weighted by how
 * much of each is visible, then get tv / (tv + tl) * (1 - e^-(tv + tl)) / (1 - e^-tv) of
 * the light. Seen from a shallow angle tv is large, which leaves the sunlit top of the layer.
 * That applies to surface among the instances, which is also hidden by them. Surface facing
 * out of their layer, along the normal of the surface behind, gets the light unshadowed.
 */
inline constexpr string_view instance_splat_composite_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];
layout(set = 1, binding = 16, std430) readonly buffer SplatWords { uint data[]; } splat_words;

struct SplatLight {
    uvec4 flags;
    vec4 position;
    vec4 direction;
    vec4 color_intensity;
    vec4 params;
};
layout(set = 1, binding = 5, std430) readonly buffer SplatLights { SplatLight data[]; } splat_lights;

layout(push_constant) uniform PC {
    uint words_word;
    uint width;
    uint height;
    uint output_id;
    uint surface_light_id;
    uint normal_id;
    uint worldpos_id;
    uint globals_base;
    uint lights_base;
    uint light_count;
    float facing[14];
} pc;

const vec3 kDirections[14] = vec3[14](
    vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0),
    vec3(0.57735, 0.57735, 0.57735), vec3(-0.57735, 0.57735, 0.57735), vec3(0.57735, -0.57735, 0.57735),
    vec3(-0.57735, -0.57735, 0.57735), vec3(0.57735, 0.57735, -0.57735), vec3(-0.57735, 0.57735, -0.57735),
    vec3(0.57735, -0.57735, -0.57735), vec3(-0.57735, -0.57735, -0.57735));

// Of the surface turned to the viewer, how far it is turned to the light on average, as
// the part from surface that is out in the open, and the part from surface among the other
// instances. Surface facing `up`, out of their layer, is not hidden by the others. Surface
// facing sideways is spread through the layer's depth, so only `seen_share` of it is seen.
vec2 facing_light(vec3 to_light, vec3 to_viewer, vec3 up, float seen_share)
{
    vec2 lit = vec2(0.0);
    float seen = 0.0;
    for (int d = 0; d < 14; ++d) {
        float open = max(dot(kDirections[d], up), 0.0);
        float towards_viewer = max(dot(kDirections[d], to_viewer), 0.0) * pc.facing[d];
        float towards_light = max(dot(kDirections[d], to_light), 0.0);
        float seen_open = towards_viewer * open;
        float seen_among = towards_viewer * (1.0 - open) * seen_share;
        seen += seen_open + seen_among;
        lit += vec2(seen_open, seen_among) * towards_light;
    }
    return seen > 0.0 ? lit / seen : vec2(0.0);
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;
    // A pixel holds few instances, so its own count jumps as they cross its edges.
    // Neighbours are averaged in, which keeps the total the same.
    const float kTent[5] = float[5](1.0, 2.0, 3.0, 2.0, 1.0);
    float coverage = 0.0;
    vec3 coverage_color = vec3(0.0);
    uint plane = pc.width * pc.height;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            ivec2 at = coord + ivec2(i, j);
            if (at.x < 0 || at.y < 0 || at.x >= int(pc.width) || at.y >= int(pc.height)) continue;
            uint word = pc.words_word + uint(at.y) * pc.width + uint(at.x);
            float weight = kTent[i + 2] * kTent[j + 2];
            coverage += float(splat_words.data[word]) * weight;
            coverage_color += vec3(float(splat_words.data[word + plane]), float(splat_words.data[word + plane * 2u]),
                                   float(splat_words.data[word + plane * 3u])) * weight;
        }
    }
    vec3 albedo = coverage > 0.0 ? coverage_color / coverage : vec3(0.0);
    coverage /= 4096.0 * 81.0;
    if (coverage <= 0.0) return;

    vec2 dims = vec2(float(pc.width), float(pc.height));
    vec2 uv = (vec2(coord) + 0.5) / dims;

    vec3 behind_normal = vec3(0.0);
    vec3 behind_pos = vec3(0.0);
    vec4 behind_light = vec4(0.0, 0.0, 0.0, 1.0);
    bool has_surface = false;
    for (int k = 0; k < 8 && !has_surface; ++k) {
        vec2 at = uv + vec2(0.0, float(k * k * 2)) / dims;
        vec3 n = velk_texture(pc.normal_id, at).xyz;
        if (dot(n, n) > 1e-6) {
            has_surface = true;
            behind_normal = normalize(n);
            behind_pos = velk_texture(pc.worldpos_id, at).xyz;
            behind_light = velk_texture(pc.surface_light_id, at);
        }
    }

    GlobalData globals = velk_globals.data[pc.globals_base];
    // Through the near plane when there is no surface, where the matrix is most precise.
    vec3 towards = behind_pos;
    if (!has_surface) {
        vec4 near_point = globals.inverse_view_projection * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
        towards = near_point.xyz / near_point.w;
    }
    vec3 to_viewer = normalize(-towards);

    float to_viewer_depth = max(coverage, 1e-5);
    float seen_share = (1.0 - exp(-to_viewer_depth)) / to_viewer_depth;
    vec3 direct_open = vec3(0.0);
    vec3 direct_among = vec3(0.0);
    vec3 direct_on_behind = vec3(0.0);
    for (uint i = 0u; i < pc.light_count; ++i) {
        SplatLight light = splat_lights.data[pc.lights_base + i];
        if (light.flags.x == 3u) continue;
        vec3 L;
        float atten = 1.0;
        if (light.flags.x == 0u) {
            L = -light.direction.xyz;
        } else {
            if (!has_surface) continue;
            vec3 to_light = (light.position.xyz - globals.cam_pos.xyz) - behind_pos;
            float dist = length(to_light);
            L = to_light / max(dist, 1e-6);
            float t = clamp(1.0 - dist / max(light.params.x, 1e-6), 0.0, 1.0);
            atten = t * t;
            if (light.flags.x == 2u) {
                atten *= smoothstep(light.params.z, light.params.y, dot(-L, light.direction.xyz));
            }
        }
        vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * atten;
        vec2 facing = facing_light(L, to_viewer, behind_normal, seen_share);
        direct_open += radiance * facing.x;
        direct_among += radiance * facing.y;
        if (has_surface) {
            direct_on_behind += radiance * max(dot(behind_normal, L), 0.0);
        }
    }
    vec3 indirect = behind_light.rgb;

    const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
    float direct_behind = dot(direct_on_behind, kLuma);
    float arriving = 1.0;
    if (direct_behind > 1e-4) {
        float to_light = -log(clamp(behind_light.a, 1e-3, 0.999));
        float both = to_light + to_viewer_depth;
        arriving = to_viewer_depth / both * (1.0 - exp(-both)) / (1.0 - exp(-to_viewer_depth));
    }
    vec3 direct_on_instance = direct_open + direct_among * arriving;

    vec4 under = velk_texture(pc.output_id, uv);
    vec3 surface = albedo * (direct_on_instance + indirect);
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], coord,
               vec4(mix(under.rgb, surface, 1.0 - exp(-coverage)), under.a));
}
)";

/**
 * @brief One view's GPU culling pass, kept by the render path. Set @c dirty when the
 *        view's batches change.
 */
struct InstanceCullPass
{
    IRenderPass::Ptr pass;
    bool dirty = true;
    vector<uint32_t> count_words;  ///< Reset before every cull.
    IGpuBuffer* arena_buffer = nullptr;
    size_t arena_size = 0;
    uint32_t arena_generation = 0;  ///< Changes when the arena's buffer is replaced, which draws naming it must follow.
    uint64_t partition_tables = 0;  ///< Which tables the recording uses.
    /// The batches culled together: see instance_cull_compute_src. Holds the clusters chosen,
    /// then the entries, then the items.
    ArenaRegion together;
    uint32_t together_items = 0;
    uint32_t together_entries = 0;
    uint32_t together_chosen = 0;  ///< How many records fit.
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
    };
    vector<Cut> cuts;
    /// Visuals whose stores were too small, with the records they wanted: theirs are made
    /// again with room for those.
    vector<std::pair<const void*, uint32_t>> cut_growth;
    uint64_t cut_check_frame = 0;  ///< When the stores' needs were last read back.
    uint64_t cut_log_frame = 0;    ///< When the count of partitions choosing again was last logged.
    uint32_t cut_moves = 0;        ///< What the count was then.
    bool cut_moves_known = false;  ///< Whether that count is comparable with the next.
};

/**
 * Lays out the runs of draws by cluster and fills them, after a phase of culling.
 */
template <class Command>
inline void record_cluster_layout(Command& cmd, InstanceCullPass& state, const vector<IBatch::Ptr>& batches,
                                  const vector<InstanceCullInfo>& visuals, IGpuPipeline* runs_pipeline,
                                  IGpuPipeline* fill_pipeline, IGpuBuffer* arena_buffer, uint32_t together_word,
                                  uint32_t together_entries_word, bool second_phase)
{
    const bool together = state.together_items != 0;
    vector<InstanceCullInfo> cluster_draws;
    for (const auto& batch : batches) {
        InstanceCullInfo info{};
        if (batch && batch->get_instance_cull(info) && info.cluster_count != 0 && !info.cull_together
            && std::find_if(cluster_draws.begin(), cluster_draws.end(), [&](const InstanceCullInfo& other) {
                   return other.cluster_entry_word == info.cluster_entry_word;
               }) == cluster_draws.end()) {
            cluster_draws.push_back(info);
        }
    }
    if (cluster_draws.empty() && !together) {
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
    if (together) {
        record_runs({0u, together_entries_word, state.together_entries, together_word, phase},
                    (state.together_entries + 63u) / 64u);
    }
    for (const auto& info : cluster_draws) {
        record_runs({info.cluster_entry_word, kCullNoSlot, 1u, info.chosen_word, phase}, 1u);
    }
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
    if (together) {
        record_fill({together_word, state.together_chosen});
    }
    for (const auto& info : visuals) {
        if (info.chosen_capacity != 0) {
            record_fill({info.chosen_word, info.chosen_capacity});
        }
    }
    cmd.record_global_barrier();
}

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

/**
 * @brief Adds @p batches' GPU culling to @p graph, if any of them are culled. Runs
 *        before the passes that draw them. With @p far_form, instances too small to draw
 *        are left out, for the path to add with instance_splat_src. With @p whole_cut, all of
 *        each kept cut is drawn, in view or not, for a view that shows another camera's choice.
 */
inline void emit_instance_cull_pass(InstanceCullPass& state, const vector<IBatch::Ptr>* batches,
                                    uint32_t view_globals_base, FrameContext& ctx, IRenderGraph& graph,
                                    bool far_form = false, uint32_t pyramid_word = kCullNoSlot, bool whole_cut = false)
{
    if (!batches || !ctx.resources || !ctx.backend || !ctx.render_ctx) {
        return;
    }

    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) {
        return;
    }

    bool record = false;
    const bool batches_changed = state.dirty;
    if (state.dirty) {
        state.dirty = false;
        record = true;
        state.count_words.clear();
        vector<uint32_t> items;
        vector<uint32_t> entries;
        uint64_t chosen = 0;
        uint64_t retest_records = 0;
        vector<const void*> counted;
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (!batch || !batch->get_instance_cull(info)) {
                continue;
            }
            if (std::find(counted.begin(), counted.end(), info.key) == counted.end()) {
                counted.push_back(info.key);
                retest_records += uint64_t(info.slot_count) + info.chosen_capacity + info.cluster_capacity;
            }
            if (info.cull_together) {
                chosen += info.cluster_capacity;
                entries.push_back(info.cluster_entry_word);
                for (uint32_t slot = 0; slot < info.slot_count; ++slot) {
                    items.push_back(info.instances_base + slot);
                    items.push_back(slot);
                    items.push_back(info.table_word);
                }
            } else if (std::find(state.count_words.begin(), state.count_words.end(), info.count_word)
                       == state.count_words.end()) {
                state.count_words.push_back(info.count_word);
            }
        }
        state.together_items = static_cast<uint32_t>(items.size() / 3u);
        state.together_entries = static_cast<uint32_t>(entries.size());
        state.together_chosen = static_cast<uint32_t>(chosen);
        state.together = {};
        if (!items.empty()) {
            const uint64_t list_words = kCullChosenHeaderWords + chosen * kCullChosenRecordWords;
            state.together = arena->alloc((list_words + entries.size() + items.size()) * sizeof(uint32_t));
            if (state.together.valid()) {
                const uint64_t base = state.together.offset();
                arena->write_at(base + list_words * sizeof(uint32_t), entries.data(), entries.size() * sizeof(uint32_t));
                arena->write_at(base + (list_words + entries.size()) * sizeof(uint32_t), items.data(),
                                items.size() * sizeof(uint32_t));
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
    if (state.count_words.empty() && state.together_items == 0) {
        return;
    }

    // The BVH replaces a visual's tables when they fill up.
    uint64_t partition_tables = 0;
    for (const auto& batch : *batches) {
        InstanceCullInfo info{};
        if (batch && batch->get_instance_cull(info)) {
            partition_tables = partition_tables * 31u + info.partition_table_word;
            partition_tables = partition_tables * 31u + info.partition_count;
            partition_tables = partition_tables * 31u + info.chunk_list_word;
        }
    }
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

    // Each partitioned visual gets a kept cut for this view, every partition choosing on its
    // first frame. A visual's is made again when its partitions may have been laid out again,
    // so that none keeps records meant for another, or when its stores need room; the rest
    // carry over. (`dirty` also follows the camera, so it alone says nothing about partitions.)
    if (batches_changed || tables_changed || !state.cut_growth.empty()) {
        record = true;
        vector<InstanceCullPass::Cut> kept;
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (!batch || !batch->get_instance_cull(info) || info.cull_together || info.partition_count == 0
                || std::find_if(kept.begin(), kept.end(), [&](const InstanceCullPass::Cut& c) { return c.key == info.key; })
                       != kept.end()) {
                continue;
            }
            uint32_t wanted = 0;
            for (const auto& [key, records] : state.cut_growth) {
                wanted = key == info.key ? std::max(wanted, records) : wanted;
            }
            auto old = std::find_if(state.cuts.begin(), state.cuts.end(),
                                    [&](const InstanceCullPass::Cut& c) { return c.key == info.key; });
            if (!tables_changed && wanted == 0 && old != state.cuts.end() && old->partitions == info.partition_count) {
                kept.push_back(std::move(*old));
                continue;
            }
            InstanceCullPass::Cut cut;
            cut.key = info.key;
            cut.partitions = info.partition_count;
            cut.capacity = std::max({info.slot_count * 2u + 4096u, wanted + wanted / 2u,
                                     old != state.cuts.end() ? old->capacity : 0u});
            // Every partition's chunks at most: each one's slots, and a part-filled chunk each.
            const uint32_t chunks = (info.slot_count + kCullChunkSlots - 1u) / kCullChunkSlots + info.partition_count;
            const uint32_t keep_word = kCutHeaderWords;
            const uint32_t choose_word = keep_word + info.partition_count;
            const uint32_t state_word = choose_word + chunks * kCullChunkListWords;
            const uint32_t store_word = state_word + info.partition_count * kCutPartitionWords;
            const size_t words = size_t(store_word) + size_t(cut.capacity) * kCutRecordWords * 2u;
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
            initial[19] = info.partition_count;
            initial[20] = base + keep_word;
            initial[21] = base + choose_word;
            initial[22] = base + state_word;
            // Chosen from nowhere, and from a table entry no partition has: all choose.
            constexpr float kFar = 1e30f;
            uint32_t far_bits;
            std::memcpy(&far_bits, &kFar, sizeof(far_bits));
            for (uint32_t p = 0; p < info.partition_count; ++p) {
                uint32_t* partition = initial.data() + state_word + p * kCutPartitionWords;
                partition[0] = partition[1] = partition[2] = far_bits;
                partition[3] = kCullNoSlot;
            }
            arena->write_at(cut.region.offset(), initial.data(), initial.size() * sizeof(uint32_t));
            kept.push_back(std::move(cut));
            // The total of the counts is not comparable with the last one any more.
            state.cut_log_frame = ctx.present_counter;
            state.cut_moves_known = false;
        }
        state.cuts = std::move(kept);
        state.cut_growth.clear();
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
        for (const auto& c : state.cuts) {
            uint32_t header[2] = {};
            arena->read_at(c.region.offset() + 13u * sizeof(uint32_t), header, sizeof(header));
            records += header[0];
            moves += header[1];
            partitions += c.partitions;
        }
        const auto frames = static_cast<double>(ctx.present_counter - state.cut_log_frame);
        if (state.cut_moves_known) {
            VELK_LOG(I, "kept cut: %u records; partitions choosing again: %.1f a frame, of %u table entries", records,
                     static_cast<double>(moves - state.cut_moves) / frames, partitions);
        }
        state.cut_moves = moves;
        state.cut_moves_known = true;
        state.cut_log_frame = ctx.present_counter;
    }

    // After everything allocated above, which can grow the arena.
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

    emit_cached_view_pass(state.pass, record, "instance.cull", graph, [&](CachedPassRecording& rec) {
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
        const bool have_pipelines = pipeline && partition_pipeline && runs_pipeline && fill_pipeline && begin_pipeline
                                 && copy_pipeline && choose_pipeline && place_pipeline && args_pipeline && draw_pipeline;
        auto cmd = have_pipelines ? ctx.backend->create_command_buffer() : nullptr;
        if (!cmd) {
            return;
        }

        cmd->begin_recording();
        cmd->push_label("Instance culling");
        // Earlier frames may still be drawing from these lists.
        cmd->record_global_barrier();

        vector<const void*> done;
        vector<InstanceCullInfo> visuals;
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (batch && batch->get_instance_cull(info) && !info.cull_together
                && std::find(done.begin(), done.end(), info.key) == done.end()) {
                done.push_back(info.key);
                visuals.push_back(info);
            }
        }

        const bool together = state.together_items != 0;
        const auto together_word = together ? static_cast<uint32_t>(state.together.offset() / sizeof(uint32_t)) : 0u;
        const uint32_t together_entries_word =
            together_word + kCullChosenHeaderWords + state.together_chosen * kCullChosenRecordWords;
        const uint32_t together_items_word = together_entries_word + state.together_entries;
        if (together) {
            cmd->record_clear_buffer(arena_buffer, uint64_t(together_word) * sizeof(uint32_t), sizeof(uint32_t));
        }
        if (state.retest.valid()) {
            cmd->record_clear_buffer(arena_buffer, state.retest.offset(), sizeof(uint32_t));
        }
        // Only the count: the rest of the header holds what the last frame drew until
        // instance_tiny_args_src writes it again.
        if (state.tiny_word != kCullNoSlot) {
            cmd->record_clear_buffer(arena_buffer, uint64_t(state.tiny_word) * sizeof(uint32_t), sizeof(uint32_t));
        }
        for (const auto& info : visuals) {
            if (info.chosen_capacity != 0) {
                cmd->record_clear_buffer(arena_buffer, uint64_t(info.chosen_word) * sizeof(uint32_t), sizeof(uint32_t));
            }
        }
        cmd->record_global_barrier();

        const auto dispatch = [&](IGpuPipeline* used, const void* pc, uint32_t pc_size, uint32_t groups,
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

        // Partitioned visuals with a kept cut: a frame of it begins.
        vector<std::pair<InstanceCullInfo, uint32_t>> kept;
        for (const auto& info : visuals) {
            const uint32_t cut = info.partition_count != 0 ? cut_word(info.key) : kCullNoSlot;
            if (cut != kCullNoSlot) {
                kept.push_back({info, cut});
                dispatch(begin_pipeline.get(), &cut, sizeof(cut), 1u);
            }
        }

        // Partitions next: their visible chunks for the far instance splat, and for those with a
        // cut whether they keep their records.
        bool any_partitions = false;
        for (const auto& info : visuals) {
            if (info.partition_count != 0) {
                any_partitions = true;
                cmd->record_clear_buffer(arena_buffer, uint64_t(info.chunk_list_word) * sizeof(uint32_t), sizeof(uint32_t));
            }
        }
        if (any_partitions) {
            cmd->record_global_barrier();
            struct PartitionPushC
            {
                uint32_t table_word;
                uint32_t entry_count;
                uint32_t list_word;
                uint32_t globals_base;
                float max_distance;
                uint32_t cut_word;
                float refresh;
            };
            for (const auto& info : visuals) {
                if (info.partition_count == 0) {
                    continue;
                }
                const PartitionPushC pc{info.partition_table_word, info.partition_count, info.chunk_list_word,
                                        view_globals_base, info.max_distance, cut_word(info.key), kCutRefresh};
                dispatch(partition_pipeline.get(), &pc, sizeof(pc), (info.partition_count + 63u) / 64u);
            }
            cmd->record_global_barrier();
        }

        // The kept cuts: records copied for partitions that keep them, counted for those that
        // choose again, placed, written, then drawn from.
        if (!kept.empty()) {
            struct ChoosePushC
            {
                uint32_t cut_word;
                uint32_t instances_base;
                uint32_t slot_count;
                uint32_t table_word;
                uint32_t globals_base;
                float lod_max_pixels;
                uint32_t write;
            };
            for (const auto& [info, cut] : kept) {
                dispatch(copy_pipeline.get(), &cut, sizeof(cut), 1u, cut);
                const ChoosePushC pc{cut, info.instances_base, info.slot_count, info.table_word, view_globals_base,
                                     kLodMaxErrorPixels, 0u};
                dispatch(choose_pipeline.get(), &pc, sizeof(pc), 1u, cut + 4u);
            }
            cmd->record_global_barrier();
            for (const auto& [info, cut] : kept) {
                dispatch(place_pipeline.get(), &cut, sizeof(cut), (info.partition_count + 63u) / 64u);
            }
            cmd->record_global_barrier();
            for (const auto& [info, cut] : kept) {
                const ChoosePushC pc{cut, info.instances_base, info.slot_count, info.table_word, view_globals_base,
                                     kLodMaxErrorPixels, 1u};
                dispatch(choose_pipeline.get(), &pc, sizeof(pc), 1u, cut + 4u);
            }
            cmd->record_global_barrier();
            for (const auto& [info, cut] : kept) {
                dispatch(args_pipeline.get(), &cut, sizeof(cut), 1u);
            }
            cmd->record_global_barrier();
        }

        struct PushC
        {
            uint32_t instances_base;
            uint32_t slot_count;
            uint32_t table_word;
            uint32_t globals_base;
            float max_distance;
            uint32_t far_form;
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
            uint32_t far_form;
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
        };
        for (const auto& [info, cut] : kept) {
            const DrawPushC pc{cut, info.instances_base, info.table_word, view_globals_base, info.max_distance,
                               far_form ? 1u : 0u, kLodMaxErrorPixels, info.chosen_word, info.chosen_capacity,
                               pyramid_word, retest_word, state.retest_capacity,
                               state.tiny_word, state.tiny_capacity, state.tiny_pixels, whole_cut ? 1u : 0u};
            dispatch(draw_pipeline.get(), &pc, sizeof(pc), 1u, cut + 8u);
        }
        if (together) {
            const PushC pc{0u, state.together_items, 0u, view_globals_base, 0.f, 0u, kLodMaxErrorPixels,
                           together_items_word, together_word, state.together_chosen,
                           pyramid_word, retest_word, state.retest_capacity,
                           state.tiny_word, state.tiny_capacity, state.tiny_pixels};
            dispatch(pipeline.get(), &pc, sizeof(pc), (state.together_items + 63u) / 64u);
        }
        // Visuals without a kept cut: every slot, every frame.
        for (const auto& info : visuals) {
            if (info.partition_count != 0 && cut_word(info.key) != kCullNoSlot) {
                continue;
            }
            const PushC pc{info.instances_base, info.slot_count, info.table_word, view_globals_base, info.max_distance,
                           far_form ? 1u : 0u, kLodMaxErrorPixels, kCullNoSlot, info.chosen_word, info.chosen_capacity,
                           pyramid_word, retest_word, state.retest_capacity,
                           state.tiny_word, state.tiny_capacity, state.tiny_pixels};
            dispatch(pipeline.get(), &pc, sizeof(pc), (info.slot_count + 63u) / 64u);
        }
        cmd->record_global_barrier();

        // Draws by cluster: lay out each one's runs, then fill them.
        record_cluster_layout(*cmd, state, *batches, visuals, runs_pipeline.get(), fill_pipeline.get(), arena_buffer,
                              together_word, together_entries_word, false);

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
        rec.cmd = std::move(cmd);
        rec.held.push_back(std::move(pipeline));
        rec.held.push_back(std::move(partition_pipeline));
        rec.held.push_back(std::move(runs_pipeline));
        rec.held.push_back(std::move(fill_pipeline));
        rec.held.push_back(std::move(begin_pipeline));
        rec.held.push_back(std::move(copy_pipeline));
        rec.held.push_back(std::move(choose_pipeline));
        rec.held.push_back(std::move(place_pipeline));
        rec.held.push_back(std::move(args_pipeline));
        rec.held.push_back(std::move(draw_pipeline));
    });
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

    emit_cached_view_pass(state.retest_pass, state.retest_dirty, "instance.retest", graph, [&](CachedPassRecording& rec) {
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

        vector<const void*> done;
        vector<InstanceCullInfo> visuals;
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (batch && batch->get_instance_cull(info) && !info.cull_together
                && std::find(done.begin(), done.end(), info.key) == done.end()) {
                done.push_back(info.key);
                visuals.push_back(info);
            }
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
        for (const auto& info : visuals) {
            if (info.chosen_capacity != 0) {
                cmd->record_clear_buffer(arena_buffer, uint64_t(info.chosen_word) * sizeof(uint32_t), sizeof(uint32_t));
            }
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

        record_cluster_layout(*cmd, state, *batches, visuals, runs_pipeline.get(), fill_pipeline.get(), arena_buffer,
                              together_word, together_entries_word, true);

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
