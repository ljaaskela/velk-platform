#ifndef VELK_RENDER_FRAME_LIGHT_POOL_H
#define VELK_RENDER_FRAME_LIGHT_POOL_H

#include <velk/string_view.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_gpu_arena.h>

#include <cstdint>
#include <cstdlib>
#include <utility>

namespace velk {

/// A view's light pool, in the culling arena: where the light of the kept cuts' records is kept
/// (kCutRecordWords, its last word). A record's light word is one of:
///   kLightNone      not read yet
///   kLightPending   being given its light, for the moment it takes
///   below kLightOneValue  the first page of its light points (kLightPageWords each)
///   kLightOneValue and up  one value for the whole record, in the bits under kLightOneValue
/// A record gets its light the first time a pixel shows it (light_claim_src), so that points of
/// light are about kLightPixels apart on screen wherever they are: one value for a cluster
/// smaller than that on screen, a whole mesh, a shape drawn by its own pass and a cluster of a
/// mesh that deforms, whose vertices are not its own; else pages, with a point per vertex of its
/// cluster, and where its largest triangle is wider than kLightPixels a grid of points in each
/// of its triangles too (velk_light_grid_point). A quad record's light is on the node of its quad
/// tree that is as wide as kLightPixels over it, one value for all the quads under that node. A
/// record laid out for a smaller size than it has grown to, twice over, is laid out again. Its
/// light goes with it while its partition keeps its cut, and to its like when the partition
/// chooses again. Pages no record names any more are found free every frame (light_mark_src,
/// light_sweep_src).
///
/// The pool is a header of kLightPoolHeaderWords, then a stack of the free pages, then a bit per
/// page marking those in use, both for as many pages as the pool may grow to, and its pages in
/// segments of kLightSegmentPages, each a region of its own, so that growing moves no page. The
/// header:
///   0  free pages on the stack
///   1  pages, in all the segments
///   2  pages asked for this frame that were not there (a record without asks every frame)
///   3  records with pages, as the last mark found
///   4  records with one value, likewise
///   5  segments
///   6  the word of the stack
///   7  the word of the marks
///   8  pages taken off the stack this frame, from its top down: the free stack is only read
///      while records take pages, so taking needs no more than counting
///   16 on, the word of each segment
/// A record's first page starts with kLightPageHeaderWords: its cluster's vertices, the level of
/// the grids in its triangles (below 2 for none), how many points it has, then its other pages,
/// at most kLightMaxPages in all, kLightNone for none. Its points follow, then those of its other
/// pages in order: a point per vertex, then each triangle's grid points in turn.
inline constexpr uint32_t kLightNone = 0xFFFFFFFFu;
inline constexpr uint32_t kLightPending = 0xFFFFFFFEu;
inline constexpr uint32_t kLightOneValue = 0x80000000u;
inline constexpr uint32_t kLightPageWords = 64;
inline constexpr uint32_t kLightPageHeaderWords = 18;
inline constexpr uint32_t kLightMaxPages = 16;
/// The finest grid in a triangle: that many points along each of its edges.
inline constexpr uint32_t kLightMaxGrid = 8;
inline constexpr uint32_t kLightSegmentPages = 16384;
inline constexpr uint32_t kLightMaxSegments = 64;
inline constexpr uint32_t kLightPoolSegmentWord = 16;
inline constexpr uint32_t kLightPoolHeaderWords = kLightPoolSegmentWord + kLightMaxSegments;

// Reading the pool, for a shader that defines VELK_LIGHT_WORDS as the culling arena's buffer.
#define VELK_LIGHT_POOL_GLSL R"(
#define VELK_LIGHT_NONE 0xFFFFFFFFu
#define VELK_LIGHT_PENDING 0xFFFFFFFEu
#define VELK_LIGHT_ONE_VALUE 0x80000000u
#define VELK_LIGHT_PAGE_WORDS 64u
#define VELK_LIGHT_PAGE_HEADER_WORDS 18u
#define VELK_LIGHT_MAX_PAGES 16u
#define VELK_LIGHT_MAX_GRID 8u

// The first word of page `page` of the pool at `pool`.
uint velk_light_page(uint pool, uint page)
{
    return VELK_LIGHT_WORDS.data[pool + 16u + (page >> 14u)] + (page & 16383u) * VELK_LIGHT_PAGE_WORDS;
}

// Points a triangle's grid of level `n` adds to its corners, which are its vertices' points.
uint velk_light_grid_points(uint n)
{
    return n < 2u ? 0u : (n + 1u) * (n + 2u) / 2u - 3u;
}

// The point at place (i, j) of the grid of level `n` in triangle `t`, with i + j at most n: at
// weights (1 - (i + j) / n, i / n, j / n) of its corners, whose points are `corners`. The grid's
// places go row by row in j, each row in i, the three corners left out.
uint velk_light_grid_point(uint vertices, uint n, uint t, uvec3 corners, uint i, uint j)
{
    if (j == 0u && i == 0u) return corners.x;
    if (j == 0u && i == n) return corners.y;
    if (j == n) return corners.z;
    uint along = j * (n + 1u) - (j * (j - 1u)) / 2u + i;
    return vertices + t * velk_light_grid_points(n) + along - (j == 0u ? 1u : 2u);
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

// The word of point `p` of the record whose first page starts at word `first`.
uint velk_light_point_word(uint pool, uint first, uint p)
{
    uint at = VELK_LIGHT_PAGE_HEADER_WORDS + p;
    uint k = at / VELK_LIGHT_PAGE_WORDS;
    uint page_word = k == 0u ? first : velk_light_page(pool, VELK_LIGHT_WORDS.data[first + 2u + k]);
    return page_word + at % VELK_LIGHT_PAGE_WORDS;
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
        float radius = uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 10u]);
        if (radius < 0.0) break;
        vec3 center = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[w]), uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 1u]),
                           uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 2u]));
        vec3 u = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 3u]), uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 4u]),
                      uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 5u]));
        vec3 v = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 6u]), uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 7u]),
                      uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 8u]));
        float near = length(center - cam) - radius;
        if (near <= 0.0 || 2.0 * max(length(u), length(v)) * pixels_per_unit > light_pixels * near) break;
        i = parent;
    }
    return tree + i * VELK_QUAD_NODE_WORDS;
}
)"

/**
 * Gives each record a pixel shows its light, the first time one does, laid out as kLightNone says
 * by how large it is on screen now, and lays out again a record that has grown to more than twice
 * what it was laid out for. Pages come from the free stack, each point kLightNone until it is lit.
 * A record the pool has no room for keeps what it had, asks again on the next frame, and says how
 * many pages it wanted. A thread per pixel. `light_pixels` is kLightPixels.
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
} pc;

#define VELK_LIGHT_WORDS light_words
)" VELK_LIGHT_POOL_GLSL R"(
// Takes `count` pages off the free stack into `pages`, false when there are not that many.
bool velk_light_take(uint pool, uint count, out uint pages[VELK_LIGHT_MAX_PAGES])
{
    uint taken = atomicAdd(light_words.data[pool + 8u], count);
    uint free_pages = light_words.data[pool];
    if (taken + count > free_pages) return false;
    uint stack = light_words.data[pool + 6u];
    for (uint k = 0u; k < VELK_LIGHT_MAX_PAGES; ++k) {
        pages[k] = k < count ? light_words.data[stack + free_pages - 1u - taken - k] : VELK_LIGHT_NONE;
    }
    return true;
}

// The finest grid in the triangles of a cluster of `vertices` and `triangles` whose largest
// triangle is `largest` pixels wide that fits the pages a record may have: 1 for none.
uint velk_light_grid(float largest, uint vertices, uint triangles)
{
    uint n = uint(clamp(ceil(largest / pc.light_pixels), 1.0, float(VELK_LIGHT_MAX_GRID)));
    uint room = VELK_LIGHT_MAX_PAGES * VELK_LIGHT_PAGE_WORDS - VELK_LIGHT_PAGE_HEADER_WORDS;
    while (n > 1u && vertices + triangles * velk_light_grid_points(n) > room) --n;
    return n;
}

// Gives `record`, whose light was `was`, pages for `vertices` points and a grid of level `n` in
// each of its `triangles`; on no room, puts `was` back.
void velk_light_lay_out(uint record, uint was, uint vertices, uint triangles, uint n)
{
    uint points = vertices + triangles * velk_light_grid_points(n);
    uint count = (VELK_LIGHT_PAGE_HEADER_WORDS + points + VELK_LIGHT_PAGE_WORDS - 1u) / VELK_LIGHT_PAGE_WORDS;
    uint pages[VELK_LIGHT_MAX_PAGES];
    if (count > VELK_LIGHT_MAX_PAGES || !velk_light_take(pc.pool_word, count, pages)) {
        atomicAdd(light_words.data[pc.pool_word + 2u], count);
        light_words.data[record + 3u] = was;
        return;
    }
    uint first = velk_light_page(pc.pool_word, pages[0]);
    light_words.data[first] = vertices;
    light_words.data[first + 1u] = n;
    light_words.data[first + 2u] = points;
    for (uint k = 1u; k < VELK_LIGHT_MAX_PAGES; ++k) light_words.data[first + 2u + k] = pages[k];
    for (uint p = 0u; p < points; ++p) {
        uint at = VELK_LIGHT_PAGE_HEADER_WORDS + p;
        light_words.data[velk_light_page(pc.pool_word, pages[at / VELK_LIGHT_PAGE_WORDS]) + at % VELK_LIGHT_PAGE_WORDS] = VELK_LIGHT_NONE;
    }
    light_words.data[record + 3u] = pages[0];
}

void main()
{
    uvec2 coord = gl_GlobalInvocationID.xy;
    if (coord.x >= pc.width || coord.y >= pc.height) return;
    uvec2 ids = velk_pixel_ids(pc.ids_word, pc.width, ivec2(coord));
    if (ids.x == 0u) return;
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint record = velk_ids_record(d, ids);
    if (record == VELK_NO_RECORD) return;
    GlobalData g = velk_globals.data[d.globals_base];
    mat4 vp = g.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
    uint entry = light_words.data[record];

    // A quad: one value on the node over it as wide as the light spacing.
    if (entry == VELK_CUT_QUAD) {
        uint node = velk_light_quad_node(light_words.data[record + 2u], light_words.data[record + 1u], g.cam_pos.xyz,
                                         pixels_per_unit, pc.light_pixels);
        if (light_words.data[node + 11u] == VELK_LIGHT_NONE) {
            atomicCompSwap(light_words.data[node + 11u], VELK_LIGHT_NONE, VELK_LIGHT_ONE_VALUE);
        }
        return;
    }

    uint was = light_words.data[record + 3u];
    if (was == VELK_LIGHT_PENDING) return;
    // A whole mesh or a shape drawn by its own pass: one value.
    if (entry == 0xFFFFFFFFu) {
        if (was == VELK_LIGHT_NONE) atomicCompSwap(light_words.data[record + 3u], VELK_LIGHT_NONE, VELK_LIGHT_ONE_VALUE);
        return;
    }

    // A cluster: as wide on screen as it is now.
    uint cluster = light_words.data[entry + 2u] + light_words.data[record + 1u] * VELK_LOD_CLUSTER_WORDS;
    uint vertices = light_words.data[cluster + 19u];
    mat4 world = light_instances.data[d.instances_base + velk_ids_slot(d, ids)].world_matrix;
    float scale = max(max(length(world[0].xyz), length(world[1].xyz)), length(world[2].xyz));
    vec3 own = vec3(uintBitsToFloat(light_words.data[cluster + 13u]), uintBitsToFloat(light_words.data[cluster + 14u]),
                    uintBitsToFloat(light_words.data[cluster + 15u]));
    float radius = uintBitsToFloat(light_words.data[cluster + 16u]) * scale;
    float away = length(mat3(world) * own + (world[3].xyz - g.cam_pos.xyz));
    float across = 2.0 * radius * pixels_per_unit / max(away, 1e-3);
    float largest = uintBitsToFloat(light_words.data[cluster + 17u]) * scale * pixels_per_unit / max(away - radius, 1e-3);
    bool one = vertices == 0u || across < pc.light_pixels;

    if (was == VELK_LIGHT_NONE) {
        if (atomicCompSwap(light_words.data[record + 3u], VELK_LIGHT_NONE, VELK_LIGHT_PENDING) != VELK_LIGHT_NONE) return;
        if (one) {
            light_words.data[record + 3u] = VELK_LIGHT_ONE_VALUE;
            return;
        }
        velk_light_lay_out(record, VELK_LIGHT_NONE, vertices, light_words.data[cluster + 1u],
                           velk_light_grid(largest, vertices, light_words.data[cluster + 1u]));
        return;
    }
    // Grown to more than twice what it was laid out for: laid out again.
    if (vertices == 0u) return;
    uint n = velk_light_grid(largest, vertices, light_words.data[cluster + 1u]);
    if (was >= VELK_LIGHT_ONE_VALUE) {
        if (across < 2.0 * pc.light_pixels) return;
    } else {
        if (was >= light_words.data[pc.pool_word + 1u]) return;
        uint laid = light_words.data[velk_light_page(pc.pool_word, was) + 1u];
        if (n <= 2u * max(laid, 1u)) return;
    }
    if (atomicCompSwap(light_words.data[record + 3u], was, VELK_LIGHT_PENDING) != was) return;
    velk_light_lay_out(record, was, vertices, light_words.data[cluster + 1u], n);
}
)";

/**
 * Marks the pages the records of a kept cut's current store name, and counts its records with
 * pages and with one value. A thread per record, dispatched by the cut's drawing counts.
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
    if (page >= light_words.data[pc.pool_word + 1u]) return;
    atomicOr(light_words.data[light_words.data[pc.pool_word + 7u] + (page >> 5u)], 1u << (page & 31u));
}

void main()
{
    uint cut = pc.cut_word;
    uint index = gl_GlobalInvocationID.x;
    if (index >= light_words.data[cut + 28u]) return;
    uint parity = light_words.data[cut + 12u];
    uint light = light_words.data[light_words.data[cut + 17u + parity] + index * 4u + 3u];
    if (light == VELK_LIGHT_NONE || light == VELK_LIGHT_PENDING) return;
    if (light >= VELK_LIGHT_ONE_VALUE) {
        atomicAdd(light_words.data[pc.pool_word + 4u], 1u);
        return;
    }
    if (light >= light_words.data[pc.pool_word + 1u]) return;
    atomicAdd(light_words.data[pc.pool_word + 3u], 1u);
    velk_light_mark(light);
    uint first = velk_light_page(pc.pool_word, light);
    for (uint k = 1u; k < VELK_LIGHT_MAX_PAGES; ++k) {
        uint page = light_words.data[first + 2u + k];
        if (page != VELK_LIGHT_NONE) velk_light_mark(page);
    }
}
)";

/**
 * Puts every page no mark says is in use on the free stack, and clears the marks for the next
 * frame. A thread per 32 pages. Starts and ends the frame's marking: with `begin` set, one
 * thread empties the stack and the mark's counts instead.
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

void main()
{
    uint pool = pc.pool_word;
    uint word = gl_GlobalInvocationID.x;
    if (pc.begin != 0u) {
        if (word == 0u) {
            light_words.data[pool] = 0u;
            light_words.data[pool + 2u] = 0u;
            light_words.data[pool + 3u] = 0u;
            light_words.data[pool + 4u] = 0u;
            light_words.data[pool + 8u] = 0u;
        }
        return;
    }
    uint pages = light_words.data[pool + 1u];
    if (word * 32u >= pages) return;
    uint marks = light_words.data[pool + 7u] + word;
    uint used = light_words.data[marks];
    uint free_pages = 0u;
    for (uint b = 0u; b < 32u && word * 32u + b < pages; ++b) {
        if ((used & (1u << b)) == 0u) ++free_pages;
    }
    uint at = light_words.data[pool + 6u] + atomicAdd(light_words.data[pool], free_pages);
    for (uint b = 0u; b < 32u && word * 32u + b < pages; ++b) {
        if ((used & (1u << b)) == 0u) light_words.data[at++] = word * 32u + b;
    }
    light_words.data[marks] = 0u;
}
)";

/// A view's light pool (see kLightNone): its header with the stack and the marks, and its
/// segments.
struct LightPool
{
    ArenaRegion header;
    vector<ArenaRegion> segments;
    uint32_t max_segments = 0;     ///< As many as the device has room for.
    uint64_t check_frame = 0;      ///< When pages asked for were last read back.
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
    const uint64_t words = uint64_t(kLightPoolHeaderWords) + max_pages + max_pages / 32u;
    pool.header = arena.alloc(words * sizeof(uint32_t));
    if (!pool.header.valid()) {
        return false;
    }
    const uint32_t base = pool.word();
    vector<uint32_t> initial;
    initial.resize(kLightPoolHeaderWords + max_pages / 32u, 0u);
    initial[6] = base + kLightPoolHeaderWords;
    initial[7] = base + kLightPoolHeaderWords + max_pages;
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
        arena.update_at(pool.header.offset() + (uint64_t(kLightPoolSegmentWord) + index) * sizeof(uint32_t), &word, sizeof(word));
        pool.segments.push_back(std::move(segment));
        ++added;
    }
    const uint32_t pages = pool.pages();
    arena.update_at(pool.header.offset() + sizeof(uint32_t), &pages, sizeof(pages));
    const auto segments = static_cast<uint32_t>(pool.segments.size());
    arena.update_at(pool.header.offset() + 5u * sizeof(uint32_t), &segments, sizeof(segments));
    return added;
}

} // namespace velk

#endif // VELK_RENDER_FRAME_LIGHT_POOL_H
