#ifndef VELK_RENDER_INTF_GPU_ARENA_H
#define VELK_RENDER_INTF_GPU_ARENA_H

#include <velk/api/velk.h>

#include <velk-render/interface/intf_buffer.h>

#include <cstdint>

namespace velk {

class IGpuArena;
class IGpuResourceManager;
class IRenderBackend;

/// A byte range within an IGpuArena's bound buffer. The producer derives an
/// element base (@c offset / element_size) and pushes it so the shader reads
/// @c data[base + index]. Also used for the arena's internal free spans.
struct GpuArenaRegion
{
    uint64_t offset = 0;
    uint64_t size   = 0;
    bool valid() const { return size != 0; }
};

/// RAII handle to a suballocated region, returned by IGpuArena::alloc.
/// Defined below the interface, since it holds an IGpuArena::WeakPtr.
class ArenaRegion;

/**
 * @brief A frame-invariant bound storage buffer (descriptor set = 1) read by
 *        index instead of by buffer_device_address.
 *
 * The buffer handle is stable, so cached / simultaneous-use command buffers can
 * bind it once and never go stale. Producers `alloc` a region, fill it with
 * `write_at`, and push its element base (@c offset / element_size) so the
 * shader reads `data[base + i]`.
 *
 * A region's bytes may be rewritten in place at a stable base (globals, lights,
 * instances: small, drifting content). Data that changes wholesale (the BVH)
 * must instead `alloc` a fresh region and drop the old handle, since the free
 * is fence-deferred but an in-place write is not, and a frame still reading the
 * previous contents would otherwise see a partial update.
 *
 * The backing buffer is bound to its slot on first allocation and re-bound only
 * on growth, rare after warmup, so steady state never touches the descriptor.
 *
 * Chain: IInterface -> IGpuArena
 */
class IGpuArena
    : public Interface<IGpuArena, IInterface,
                       VELK_UID("0267a3dc-1b37-4e65-bc3d-8bc24a6fd099")>
{
public:
    /// One-time setup, called by the resource manager that creates the arena.
    /// @p slot is the set = 1 binding (IRenderBackend::GlobalBufferSlot);
    /// @p element_size is the record stride, which keeps region offsets a
    /// whole multiple of the element so they divide cleanly into an element
    /// base. @p resources and @p backend are retained for buffer allocation,
    /// slot binding, and fence markers, so allocating needs no per-frame
    /// context and callers outside the render loop can suballocate.
    ///
    /// @p index_buffer allocates the backing buffer with INDEX_BUFFER usage,
    /// for an arena whose regions are also bound for indexed draws (mesh
    /// geometry). @p reserve_bytes pre-sizes the first allocation; growth
    /// recopies the whole arena, which is cheap for records and not for bulk
    /// geometry, so a bulk tenant should reserve rather than grow into place.
    virtual void init(uint32_t slot, uint32_t element_size,
                      IGpuResourceManager* resources, IRenderBackend* backend,
                      bool index_buffer = false, uint64_t reserve_bytes = 0) = 0;

    /// The arena's backing GPU buffer, for consumers that must bind it rather
    /// than index it (mesh geometry bound as an index buffer). Null before the
    /// first allocation. The handle CHANGES on growth, so callers must re-ask
    /// each time rather than caching it.
    virtual IGpuBuffer* buffer() const = 0;

    /// Reserves a @p size-byte region from the arena's free-list (growing the
    /// buffer if no span fits) and returns an owning handle. The offset
    /// survives across frames until the handle drops; the shader base is
    /// @c offset / element_size. Fill it with
    /// `write_at`. Returns an invalid handle on failure. First reclaims any
    /// freed regions whose in-flight frame has retired.
    ///
    /// @p alignment, when non-zero, forces the returned offset to a multiple
    /// of it (0 uses the arena's element_size). Heterogeneous records that
    /// share one arena but derive their shader base as @c offset / record_size
    /// pass their own record size so the base stays integral (e.g. per-material
    /// data whose struct size varies by material type).
    virtual ArenaRegion alloc(uint64_t size, uint64_t alignment = 0) = 0;

    /// Writes @p size bytes into the arena at @p offset (a region obtained
    /// from `alloc`). Call only when the region's contents change; unchanged
    /// regions keep their bytes across frames (no re-upload). Not fenced: use
    /// this for content that drifts, and allocate a fresh region instead when
    /// the data changes wholesale.
    virtual void write_at(uint64_t offset, const void* data, uint64_t size) = 0;

    /// Internal: called by ArenaRegion on drop to free a region. The arena
    /// defers the reclaim past the in-flight frame's fence, so the range is
    /// never reused while an earlier frame may still read it.
    virtual void release_region(uint64_t offset, uint64_t size) = 0;

    /// Returns freed regions whose in-flight frame has retired to the
    /// free-list. Driven by GpuResourceManager::drain_deferred each frame so
    /// reclaim happens even without a new alloc.
    virtual void reclaim() = 0;

    /// Creates an IBuffer whose storage is a region of this arena, sized
    /// @p size bytes. The region's lifetime rides the returned Ptr, so it is
    /// freed (fence-deferred) when the last reference drops, like any other
    /// GPU resource.
    ///
    /// Arena memory is allocated for sequential writes, so reads from it are
    /// slow: the returned buffer is **write-only from the CPU side**. It
    /// serves `write`, and its `get_data` / `write_diff` report nothing:
    /// implementing only the half of IBuffer the storage supports is
    /// deliberate. Consumers get its location with `get_gpu_ref`, which
    /// returns a Kind::Index ref, never an address.
    /// @p alignment (0 = the arena's element_size) forces the region's offset
    /// to a multiple of it, and is re-applied when the buffer regrows. An
    /// arena whose records are read at more than one stride is byte-granular
    /// (element_size 1), so its refs carry a byte offset and each consumer
    /// divides by the stride it reads with; such a buffer passes its record
    /// size here to keep those divisions integral.
    virtual IBuffer::Ptr create_buffer(uint64_t size, uint64_t alignment = 0) = 0;

    /// Writable view of @p offset within the arena's mapped storage, or null
    /// when unmapped. Write-only memory: never read through this pointer.
    /// Exists for arena-backed buffers to let a caller's writer fill the
    /// region in place instead of staging and copying.
    virtual void* mapped_at(uint64_t offset) = 0;

    /// The set = 1 slot this arena's buffer is bound to.
    virtual uint32_t slot() const = 0;

    /// Record stride. A region's shader base is its byte offset divided by
    /// this, which is why offsets are kept a whole multiple of it.
    virtual uint32_t element_size() const = 0;
};


/// RAII handle to a suballocated region (from IGpuArena::alloc).
/// Move-only; on destruction it tells its source arena to free the region
/// (which the arena defers past the in-flight frame's fence), or does nothing
/// if the arena is already gone. The arena reference is weak because regions
/// can outlive it: a mesh primitive is a scene asset, torn down after the
/// renderer that owns the arenas. Deliberately lightweight, not an
/// IGpuResource: there may be very many small regions.
class ArenaRegion
{
public:
    ArenaRegion() = default;
    ArenaRegion(IGpuArena::WeakPtr arena, uint64_t offset, uint64_t size)
        : arena_(std::move(arena)), offset_(offset), size_(size) {}
    ~ArenaRegion() { release(); }

    ArenaRegion(ArenaRegion&& o) noexcept
        : arena_(std::move(o.arena_)), offset_(o.offset_), size_(o.size_)
    {
        o.arena_ = {};
        o.offset_ = 0;
        o.size_ = 0;
    }
    ArenaRegion& operator=(ArenaRegion&& o) noexcept
    {
        if (this != &o) {
            release();
            arena_ = std::move(o.arena_); offset_ = o.offset_; size_ = o.size_;
            o.arena_ = {};
            o.offset_ = 0;
            o.size_ = 0;
        }
        return *this;
    }
    ArenaRegion(const ArenaRegion&) = delete;
    ArenaRegion& operator=(const ArenaRegion&) = delete;

    bool valid() const { return size_ != 0 && !arena_.expired(); }
    uint64_t offset() const { return offset_; }
    uint64_t size() const { return size_; }

    /// Frees the region back to its arena (deferred past the in-flight fence)
    /// and clears this handle. Called automatically on destruction / move.
    /// A no-op once the arena has been destroyed.
    void release();

private:
    IGpuArena::WeakPtr arena_;
    uint64_t offset_ = 0;
    uint64_t size_ = 0;
};

inline void ArenaRegion::release()
{
    // The lock fails when the arena is already destroyed, which happens for
    // holders that outlive the renderer (mesh primitives). Their bytes died
    // with the arena's buffer, so there is nothing to give back.
    if (auto arena = arena_.lock()) arena->release_region(offset_, size_);
    arena_ = {};
    offset_ = 0;
    size_ = 0;
}

} // namespace velk

#endif // VELK_RENDER_INTF_GPU_ARENA_H
