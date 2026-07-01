#ifndef VELK_RENDER_GPU_ARENA_H
#define VELK_RENDER_GPU_ARENA_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/interface/intf_gpu_buffer.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Bound storage buffer (set = 1) read by index, in two styles.
 *
 * A given arena instance uses one style over its backing buffer:
 *
 * - **Transient (ring), via `write`.** For data rebuilt wholesale every frame
 *   (BVH, globals). The buffer is `frame_slot_count()` equal-size sub-buffers;
 *   each frame writes the sub-buffer for the backend's fenced current slot, so
 *   an in-flight frame never reads the region being written. Within a frame,
 *   `write` bump-allocates distinct regions so producers coexist.
 *
 * - **Persistent, via `alloc` / `write_at`.** For data with stable identity
 *   (instances). A single buffer with a byte free-list; `alloc` returns an
 *   owning `ArenaRegion` whose offset survives across frames. Dropping the
 *   handle frees the region, deferred past the in-flight frame's completion
 *   marker (`reclaim`, driven by GpuResourceManager::drain_deferred), so a
 *   range is never reused while an earlier frame may still read it.
 *
 * Backing buffers come from the resource manager; the backend only binds them
 * to the set = 1 slot. Buffers are re-bound only on growth (rare after
 * warmup), so steady state never touches the descriptor.
 */
class GpuArena : public ::velk::ext::ObjectCore<GpuArena, ::velk::IGpuArena>
{
public:
    VELK_CLASS_UID(::velk::ClassId::GpuArena, "GpuArena");

    void init(uint32_t slot, uint32_t element_size) override;
    GpuArenaRegion write(const void* data, uint64_t size, FrameContext& ctx) override;
    ArenaRegion alloc(uint64_t size, FrameContext& ctx) override;
    void write_at(uint64_t offset, const void* data, uint64_t size) override;
    void release_region(uint64_t offset, uint64_t size) override;
    void reclaim() override;
    uint32_t slot() const override { return slot_; }

private:
    void recreate_buffer(uint32_t regions, FrameContext& ctx);
    bool grow_persistent(uint64_t want, FrameContext& ctx);
    void drain_zombies();
    void coalesce_free();

    struct Zombie { uint64_t offset; uint64_t size; uint64_t marker; };

    uint32_t slot_ = 0;
    uint32_t element_size_ = 1;

    // Ring (transient) state, used by write().
    uint64_t sub_stride_ = 0;            ///< Bytes per per-frame sub-buffer.
    uint64_t bump_ = 0;                  ///< Bytes used in the current frame's sub-buffer.
    uint32_t last_slot_ = 0xFFFFFFFFu;   ///< Sentinel forces a bump reset on the first write.
    IGpuBuffer::Ptr buffer_;
    void* mapped_ = nullptr;

    // Persistent (alloc) state. Separate buffer from the ring; a given arena
    // instance uses one style, so only one buffer is created.
    IRenderBackend* backend_ = nullptr;  ///< Cached for release_region / reclaim.
    IGpuBuffer::Ptr persistent_buffer_;
    void* persistent_mapped_ = nullptr;
    uint64_t persistent_capacity_ = 0;
    vector<GpuArenaRegion> free_spans_;
    vector<Zombie> zombies_;             ///< Freed regions awaiting their fence.
};

} // namespace velk::impl

#endif // VELK_RENDER_GPU_ARENA_H
