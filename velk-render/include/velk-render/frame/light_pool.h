#ifndef VELK_RENDER_FRAME_LIGHT_POOL_H
#define VELK_RENDER_FRAME_LIGHT_POOL_H

#include <velk/vector.h>

#include <velk-render/frame/light_pool_layout.h>
#include <velk-render/frame/light_pool_shaders.h>
#include <velk-render/interface/intf_gpu_arena.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace velk {

/// A view's light pool (see kLightNone): its header with the stack, the marks and the search
/// list, and its segments.
struct LightPool
{
    ArenaRegion header;
    vector<ArenaRegion> segments;
    uint32_t max_segments = 0;     ///< As many as the device has room for.
    uint64_t check_frame = 0;      ///< When pages asked for were last read back.
    uint64_t low_since = 0;        ///< Since when it has been under a sixteenth free, 0 while not.
    uint64_t ahead_frame = 0;      ///< When it last grew ahead of a frame asking (kLightPoolAheadFrames).
    uint32_t short_checks = 0;     ///< Checks in a row that found pages asked for and not there.
    uint64_t grew_frame = 0;       ///< When it last grew for pages asked for.
    bool reads = false;            ///< Whether VELK_LIGHT_READ named pixels, logged every frame.
    ArenaRegion debug;             ///< Its debug block (VELK_LIGHT_DEBUG_BLOCK), made only with reads.
    uint64_t log_frame = 0;        ///< When its counts were last logged.
    uint32_t recorded_pages = 0;   ///< What the culling recording sweeps.
    float rays = kLightTraceRays;  ///< The trace's budget in rays a frame (adjust_light_rays).
    /// Threads that fill the device (IRenderBackend::compute_fill_threads): a budget of fewer
    /// rays leaves it idle and takes about as long, so it is the least the budget goes to.
    uint32_t fill = 4096;
    float trace_ms = 0.f;          ///< The GPU time of its trace's last measured frame.
    float most_trace_ms = 0.f;     ///< And the most of it since its counts were last logged.
    uint32_t written_rays = kLightTraceRays; ///< What the header holds of it.

    uint32_t word() const
    {
        return header.valid() ? static_cast<uint32_t>(header.offset() / sizeof(uint32_t)) : 0xFFFFFFFFu;
    }
    uint32_t pages() const { return static_cast<uint32_t>(segments.size()) * kLightSegmentPages; }
};

/// Makes @p pool's header on its first use, with a segment, and as many segments as
/// @p device_bytes of device memory has room for at most: a thirty-second of it, up to
/// kLightMaxSegments. @p fill_threads is IRenderBackend::compute_fill_threads.
inline bool ensure_light_pool(LightPool& pool, IGpuArena& arena, uint64_t device_bytes, uint32_t fill_threads)
{
    if (pool.header.valid()) {
        return true;
    }
    pool.fill = fill_threads;
    constexpr uint64_t segment_bytes = uint64_t(kLightSegmentPages) * kLightSegmentWordsPerPage * sizeof(uint32_t);
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
    const uint64_t pieces_word = uint64_t(kLightPoolHeaderWords) + max_pages + max_pages / 32u;
    const uint64_t list_word = pieces_word + max_pages / 32u;
    const uint64_t split_word = list_word + uint64_t(kLightSearchCapacity) * kLightSearchWords;
    const uint64_t trace_word = split_word + uint64_t(kLightSplitCapacity) * kLightSplitWords;
    const uint64_t points_word = trace_word + uint64_t(kLightTraceCapacity) * kLightTraceWords;
    const uint64_t point_list_words = uint64_t(kLightPointCapacity) * kLightPointWords;
    const uint64_t words = points_word + kLightPointLists * point_list_words;
    pool.header = arena.alloc(words * sizeof(uint32_t));
    if (!pool.header.valid()) {
        return false;
    }
    const uint32_t base = pool.word();
    vector<uint32_t> initial;
    initial.resize(kLightPoolHeaderWords + 2u * (max_pages / 32u), 0u);
    initial[kLightPoolStackWord] = base + kLightPoolHeaderWords;
    initial[kLightPoolMarksWord] = base + kLightPoolHeaderWords + max_pages;
    initial[kLightPoolPiecesWord] = base + static_cast<uint32_t>(pieces_word);
    initial[kLightPoolSearchWord] = base + static_cast<uint32_t>(list_word);
    initial[kLightPoolSplitWord] = base + static_cast<uint32_t>(split_word);
    initial[kLightPoolTraceWord] = base + static_cast<uint32_t>(trace_word);
    initial[kLightPoolRayBudget] = kLightTraceRays;
    initial[kLightPoolFillThreads] = fill_threads;
    initial[kLightPoolUntracedPointsWord] = base + static_cast<uint32_t>(points_word);
    initial[kLightPoolCoarsePointsWord] = base + static_cast<uint32_t>(points_word + point_list_words);
    initial[kLightPoolFinePointsWord] = base + static_cast<uint32_t>(points_word + 2u * point_list_words);
    initial[kLightPoolSunSkip] = kLightNone;
    // A development aid: VELK_LIGHT_READ=x,y;x,y;... names up to kLightReadPixels pixels whose
    // lighting terms the lighting writes and the log prints, in a debug block of its own made only
    // then (kLightPoolDebug).
    initial[kLightPoolDebug] = kLightNone;
    uint32_t read_pixels[kLightReadPixels];
    for (uint32_t k = 0; k < kLightReadPixels; ++k) read_pixels[k] = kLightNone;
    if (const char* read = std::getenv("VELK_LIGHT_READ")) {
        uint32_t k = 0;
        for (const char* at = read; at && *at && k < kLightReadPixels;) {
            char* end = nullptr;
            const unsigned long x = std::strtoul(at, &end, 10);
            if (end && *end == ',') {
                const unsigned long y = std::strtoul(end + 1, &end, 10);
                if (x < 65536u && y < 65536u) {
                    read_pixels[k++] = uint32_t(x) | (uint32_t(y) << 16u);
                    pool.reads = true;
                }
            }
            at = std::strchr(at, ';');
            if (at) ++at;
        }
    }
    if (pool.reads) {
        pool.debug = arena.alloc(uint64_t(kLightDebugEnd) * sizeof(uint32_t));
        if (pool.debug.valid()) {
            vector<uint32_t> block;
            block.resize(kLightDebugEnd, 0u);
            for (uint32_t k = 0; k < kLightReadPixels; ++k) block[kLightDebugReadPixels + k] = read_pixels[k];
            arena.write_at(pool.debug.offset(), block.data(), uint64_t(kLightDebugEnd) * sizeof(uint32_t));
            initial[kLightPoolDebug] = static_cast<uint32_t>(pool.debug.offset() / sizeof(uint32_t));
        } else {
            pool.reads = false;
        }
    }
    // The header, then the marks and the piece bits, all clear.
    arena.write_at(pool.header.offset(), initial.data(), kLightPoolHeaderWords * sizeof(uint32_t));
    arena.write_at(pool.header.offset() + (uint64_t(kLightPoolHeaderWords) + max_pages) * sizeof(uint32_t),
                   initial.data() + kLightPoolHeaderWords, uint64_t(2u * (max_pages / 32u)) * sizeof(uint32_t));
    pool.segments.clear();
    return true;
}

/// Adds @p count segments to @p pool, as many as it may have at most, and says so in its header.
/// Returns how many were added.
inline uint32_t grow_light_pool(LightPool& pool, IGpuArena& arena, uint32_t count)
{
    uint32_t added = 0;
    while (added < count && pool.segments.size() < pool.max_segments) {
        ArenaRegion segment = arena.alloc(uint64_t(kLightSegmentPages) * kLightSegmentWordsPerPage * sizeof(uint32_t));
        if (!segment.valid()) {
            break;
        }
        const auto index = static_cast<uint32_t>(pool.segments.size());
        const auto word = static_cast<uint32_t>(segment.offset() / sizeof(uint32_t));
        arena.update_at(pool.header.offset() + (uint64_t(kLightPoolSegmentWords) + index) * sizeof(uint32_t), &word, sizeof(word));
        pool.segments.push_back(std::move(segment));
        ++added;
    }
    const uint32_t pages = pool.pages();
    arena.update_at(pool.header.offset() + uint64_t(kLightPoolPages) * sizeof(uint32_t), &pages, sizeof(pages));
    const auto segments = static_cast<uint32_t>(pool.segments.size());
    arena.update_at(pool.header.offset() + uint64_t(kLightPoolSegments) * sizeof(uint32_t), &segments, sizeof(segments));
    return added;
}

/// Moves @p pool's budget in rays a frame toward what @p target_ms of GPU time would take,
/// by @p trace_ms, what its trace took the last frame measured (0 when not known); no limit when
/// @p target_ms is 0 or less. A quarter of the way up a frame, half of it down, at most twice or a
/// quarter as much, as the time comes a frame or two late. It grows only while the frame's work
/// used the budget, so that frames needing little do not leave a budget that heavier work would
/// spend all at once. Never under the threads that fill the device (LightPool::fill): fewer rays
/// take about the time of the longest walk among them, so cutting them saves nothing.
inline void adjust_light_rays(LightPool& pool, IGpuArena& arena, float trace_ms, float target_ms)
{
    if (!pool.header.valid()) {
        return;
    }
    if (trace_ms > 0.f) {
        pool.trace_ms = trace_ms;
        pool.most_trace_ms = trace_ms > pool.most_trace_ms ? trace_ms : pool.most_trace_ms;
    }
    uint32_t budget = kLightTraceRaysUnlimited;
    if (target_ms > 0.f) {
        if (trace_ms > 0.f) {
            uint32_t charged = 0;
            arena.read_at(pool.header.offset() + uint64_t(kLightPoolLastRaysCharged) * sizeof(uint32_t), &charged,
                          sizeof(charged));
            float scale = target_ms / trace_ms;
            scale = scale < 0.25f ? 0.25f : scale > 2.f ? 2.f : scale;
            if (scale > 1.f && float(charged) < 0.9f * pool.rays) {
                scale = 1.f;
            }
            // Half the way down, a quarter up: a frame far over its time is cut back in a few.
            pool.rays *= 1.f + (scale < 1.f ? 0.5f : 0.25f) * (scale - 1.f);
            const float least = float(pool.fill);
            pool.rays = pool.rays < least ? least : pool.rays > float(kLightTraceRaysUnlimited) ? float(kLightTraceRaysUnlimited) : pool.rays;
        }
        budget = static_cast<uint32_t>(pool.rays);
    }
    if (budget != pool.written_rays) {
        arena.update_at(pool.header.offset() + uint64_t(kLightPoolRayBudget) * sizeof(uint32_t), &budget, sizeof(budget));
        pool.written_rays = budget;
    }
}

} // namespace velk

#endif // VELK_RENDER_FRAME_LIGHT_POOL_H
