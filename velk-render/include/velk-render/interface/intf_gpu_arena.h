#ifndef VELK_RENDER_INTF_GPU_ARENA_H
#define VELK_RENDER_INTF_GPU_ARENA_H

#include <velk/api/velk.h>

#include <cstdint>

namespace velk {

struct FrameContext;

class IGpuArena;

/// A written region within an IGpuArena. @c offset is the byte offset of
/// the data within the arena's bound buffer; the producer derives an
/// element base (@c offset / element_size) and pushes it so the shader
/// reads @c data[base + index].
struct GpuArenaRegion
{
    uint64_t offset = 0;
    uint64_t size   = 0;
    bool valid() const { return size != 0; }
};

/// RAII handle to a persistent suballocated region (from IGpuArena::alloc).
/// Move-only; on destruction it tells its source arena to free the region
/// (which the arena defers past the in-flight frame's fence). Deliberately
/// lightweight, not an IGpuResource: there may be very many small regions.
class ArenaRegion
{
public:
    ArenaRegion() = default;
    ArenaRegion(IGpuArena* arena, uint64_t offset, uint64_t size)
        : arena_(arena), offset_(offset), size_(size) {}
    ~ArenaRegion() { release(); }

    ArenaRegion(ArenaRegion&& o) noexcept
        : arena_(o.arena_), offset_(o.offset_), size_(o.size_) { o.arena_ = nullptr; }
    ArenaRegion& operator=(ArenaRegion&& o) noexcept
    {
        if (this != &o) {
            release();
            arena_ = o.arena_; offset_ = o.offset_; size_ = o.size_;
            o.arena_ = nullptr;
        }
        return *this;
    }
    ArenaRegion(const ArenaRegion&) = delete;
    ArenaRegion& operator=(const ArenaRegion&) = delete;

    bool valid() const { return arena_ != nullptr; }
    uint64_t offset() const { return offset_; }
    uint64_t size() const { return size_; }

    /// Frees the region back to its arena (deferred past the in-flight fence)
    /// and clears this handle. Called automatically on destruction / move.
    void release();

private:
    IGpuArena* arena_ = nullptr;
    uint64_t offset_ = 0;
    uint64_t size_ = 0;
};

/**
 * @brief A frame-invariant bound storage buffer (descriptor set = 1) read by
 *        index instead of by buffer_device_address.
 *
 * The buffer handle is stable, so cached / simultaneous-use command buffers
 * can bind it once and never go stale. Per-frame-dynamic data (rebuilt
 * wholesale each frame: BVH, globals, instances) is written via a **ring of
 * `frame_slot_count()` sub-buffers**: each frame writes the sub-buffer for the
 * backend's fenced current slot, so no in-flight frame reads the region being
 * written. The returned region's element base (@c offset / element_size) goes
 * into the per-frame push constant / draw header; the shader reads
 * `data[base + i]`.
 *
 * The backing buffer is bound to its slot on the first write and re-bound only
 * on growth, rare after warmup, so steady state never touches the descriptor.
 *
 * Chain: IInterface -> IGpuArena
 */
class IGpuArena
    : public Interface<IGpuArena, IInterface,
                       VELK_UID("0267a3dc-1b37-4e65-bc3d-8bc24a6fd099")>
{
public:
    /// One-time setup: @p slot is the set = 1 binding
    /// (IRenderBackend::GlobalBufferSlot); @p element_size keeps ring
    /// per-region strides a whole multiple of the element so `write`'s
    /// offset divides cleanly into an element base.
    virtual void init(uint32_t slot, uint32_t element_size) = 0;

    /// Writes @p size bytes into this frame's ring region (the backend's
    /// fenced current slot), growing + re-binding the slot only if @p size
    /// exceeds the current per-region stride. Returns the written region;
    /// @c offset / element_size is the base the shader adds to every index.
    /// Visible to this frame's reads only.
    virtual GpuArenaRegion write(const void* data, uint64_t size,
                                 FrameContext& ctx) = 0;

    /// Persistent style. Reserves a stable @p size-byte region from the
    /// arena's free-list (growing the buffer if no span fits) and returns an
    /// owning handle. The offset survives across frames until the handle
    /// drops; the shader base is @c offset / element_size. Fill it with
    /// `write_at`. Returns an invalid handle on failure. First reclaims any
    /// freed regions whose in-flight frame has retired.
    virtual ArenaRegion alloc(uint64_t size, FrameContext& ctx) = 0;

    /// Writes @p size bytes into the persistent buffer at @p offset (a region
    /// obtained from `alloc`). Call only when the region's contents change;
    /// unchanged regions keep their bytes across frames (no re-upload).
    virtual void write_at(uint64_t offset, const void* data, uint64_t size) = 0;

    /// Internal: called by ArenaRegion on drop to free a persistent region.
    /// The arena defers the reclaim past the in-flight frame's fence, so the
    /// range is never reused while an earlier frame may still read it.
    virtual void release_region(uint64_t offset, uint64_t size) = 0;

    /// Returns freed persistent regions whose in-flight frame has retired to
    /// the free-list. Driven by GpuResourceManager::drain_deferred each frame
    /// so reclaim happens even without a new alloc. No-op for ring arenas.
    virtual void reclaim() = 0;

    /// The set = 1 slot this arena's buffer is bound to.
    virtual uint32_t slot() const = 0;
};

inline void ArenaRegion::release()
{
    if (arena_) arena_->release_region(offset_, size_);
    arena_ = nullptr;
    offset_ = 0;
    size_ = 0;
}

} // namespace velk

#endif // VELK_RENDER_INTF_GPU_ARENA_H
