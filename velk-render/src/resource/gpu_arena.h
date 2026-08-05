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
 * @brief Bound storage buffer (set = 1) suballocated into regions read by
 *        index instead of by device address.
 *
 * One buffer with a byte free-list. `alloc` returns an owning `ArenaRegion`
 * whose offset survives across frames; `write_at` fills it. Dropping the handle
 * frees the region, deferred past the in-flight frame's completion marker
 * (`reclaim`, driven by GpuResourceManager::drain_deferred), so a range is
 * never reused while an earlier frame may still read it.
 *
 * Producers whose data changes wholesale (the BVH) allocate a fresh region and
 * drop the old handle rather than overwriting in place, which is what keeps a
 * mid-flight change safe; producers whose bytes drift in small ways (globals,
 * lights, instances) write in place at a stable base.
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
    ArenaRegion alloc(uint64_t size, FrameContext& ctx, uint64_t alignment = 0) override;
    void write_at(uint64_t offset, const void* data, uint64_t size) override;
    void release_region(uint64_t offset, uint64_t size) override;
    void reclaim() override;
    uint32_t slot() const override { return slot_; }

private:
    bool grow_persistent(uint64_t want, FrameContext& ctx);
    void drain_zombies();
    void coalesce_free();

    struct Zombie { uint64_t offset; uint64_t size; uint64_t marker; };

    uint32_t slot_ = 0;
    uint32_t element_size_ = 1;

    IRenderBackend* backend_ = nullptr;  ///< Cached for release_region / reclaim.
    IGpuBuffer::Ptr persistent_buffer_;
    void* persistent_mapped_ = nullptr;
    uint64_t persistent_capacity_ = 0;
    vector<GpuArenaRegion> free_spans_;
    vector<Zombie> zombies_;             ///< Freed regions awaiting their fence.
};

} // namespace velk::impl

#endif // VELK_RENDER_GPU_ARENA_H
