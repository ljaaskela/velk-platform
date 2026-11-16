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
/// A record gets its light the first time a pixel shows it (light_claim_src): one value for a
/// record that is one point of light, pages for one with a point per vertex. Its light goes with
/// it while its partition keeps its cut, and to its like when the partition chooses again.
/// Pages no record names any more are found free every frame (light_mark_src, light_sweep_src).
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
/// A record's first page starts with kLightPageHeaderWords: how many points it has, then its
/// other pages, kLightNone for none. Its points follow, then those of its other pages in order.
inline constexpr uint32_t kLightNone = 0xFFFFFFFFu;
inline constexpr uint32_t kLightPending = 0xFFFFFFFEu;
inline constexpr uint32_t kLightOneValue = 0x80000000u;
inline constexpr uint32_t kLightPageWords = 64;
inline constexpr uint32_t kLightPageHeaderWords = 5;
inline constexpr uint32_t kLightSegmentPages = 16384;
inline constexpr uint32_t kLightMaxSegments = 64;
inline constexpr uint32_t kLightPoolSegmentWord = 16;
inline constexpr uint32_t kLightPoolHeaderWords = kLightPoolSegmentWord + kLightMaxSegments;

// Reading and writing the pool, for a shader that declares `light_words`, the culling arena,
// writable.
#define VELK_LIGHT_POOL_GLSL R"(
#define VELK_LIGHT_NONE 0xFFFFFFFFu
#define VELK_LIGHT_PENDING 0xFFFFFFFEu
#define VELK_LIGHT_ONE_VALUE 0x80000000u
#define VELK_LIGHT_PAGE_WORDS 64u
#define VELK_LIGHT_PAGE_HEADER_WORDS 5u

// The first word of page `page` of the pool at `pool`.
uint velk_light_page(uint pool, uint page)
{
    return light_words.data[pool + 16u + (page >> 14u)] + (page & 16383u) * VELK_LIGHT_PAGE_WORDS;
}
)"

/**
 * Gives each record a pixel shows its light, the first time one does: one value for a quad, a
 * whole mesh, a shape drawn by its own pass and a cluster of a mesh that deforms, whose vertices
 * are not its own; else a point per vertex of its cluster, in pages from the free stack, each
 * point kLightNone until it is lit. A record the pool has no room for asks again on the next
 * frame, and says how many pages it wanted. A thread per pixel.
 */
inline constexpr string_view light_claim_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint ids_word;
    uint width;
    uint height;
    uint pool_word;
} pc;
)" VELK_LIGHT_POOL_GLSL R"(
// Points of light the record at `record` has.
uint velk_light_points(uint record)
{
    uint entry = light_words.data[record];
    if (entry == 0xFFFFFFFFu || entry == VELK_CUT_QUAD) return 1u;
    uint cluster = light_words.data[entry + 2u] + light_words.data[record + 1u] * VELK_LOD_CLUSTER_WORDS;
    return max(light_words.data[cluster + 19u], 1u);
}

// Takes `count` pages off the free stack into `pages`, false when there are not that many.
bool velk_light_take(uint pool, uint count, out uint pages[5])
{
    uint taken = atomicAdd(light_words.data[pool + 8u], count);
    uint free_pages = light_words.data[pool];
    if (taken + count > free_pages) return false;
    uint stack = light_words.data[pool + 6u];
    for (uint k = 0u; k < 5u; ++k) {
        pages[k] = k < count ? light_words.data[stack + free_pages - 1u - taken - k] : VELK_LIGHT_NONE;
    }
    return true;
}

void main()
{
    uvec2 coord = gl_GlobalInvocationID.xy;
    if (coord.x >= pc.width || coord.y >= pc.height) return;
    uvec2 ids = velk_pixel_ids(pc.ids_word, pc.width, ivec2(coord));
    if (ids.x == 0u) return;
    uint record = velk_ids_record(velk_draw_data.data[velk_ids_draw(ids)], ids);
    if (record == VELK_NO_RECORD || light_words.data[record + 3u] != VELK_LIGHT_NONE) return;
    if (atomicCompSwap(light_words.data[record + 3u], VELK_LIGHT_NONE, VELK_LIGHT_PENDING) != VELK_LIGHT_NONE) return;

    uint points = velk_light_points(record);
    if (points == 1u) {
        light_words.data[record + 3u] = VELK_LIGHT_ONE_VALUE;
        return;
    }
    uint first_room = VELK_LIGHT_PAGE_WORDS - VELK_LIGHT_PAGE_HEADER_WORDS;
    uint count = 1u + (points > first_room ? (points - first_room + VELK_LIGHT_PAGE_WORDS - 1u) / VELK_LIGHT_PAGE_WORDS : 0u);
    uint pages[5];
    if (count > 5u || !velk_light_take(pc.pool_word, count, pages)) {
        atomicAdd(light_words.data[pc.pool_word + 2u], count);
        light_words.data[record + 3u] = VELK_LIGHT_NONE;
        return;
    }
    uint first = velk_light_page(pc.pool_word, pages[0]);
    light_words.data[first] = points;
    for (uint k = 1u; k < 5u; ++k) light_words.data[first + k] = pages[k];
    for (uint p = 0u; p < points; ++p) {
        uint at = VELK_LIGHT_PAGE_HEADER_WORDS + p;
        uint page = at / VELK_LIGHT_PAGE_WORDS;
        light_words.data[velk_light_page(pc.pool_word, pages[page]) + at % VELK_LIGHT_PAGE_WORDS] = VELK_LIGHT_NONE;
    }
    light_words.data[record + 3u] = pages[0];
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
    for (uint k = 1u; k < 5u; ++k) {
        uint page = light_words.data[first + k];
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
