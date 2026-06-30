#ifndef VELK_RENDER_INTF_GPU_ARENA_H
#define VELK_RENDER_INTF_GPU_ARENA_H

#include <velk/api/velk.h>

#include <cstdint>
#include <velk-render/render_path/frame_context.h>

namespace velk {

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

/**
 * @brief A frame-invariant bound storage buffer (descriptor set = 1) that
 *        compute shaders read by index instead of by buffer_device_address.
 *
 * The buffer handle is stable, so cached / simultaneous-use command
 * buffers can bind it once and never go stale. Per-frame-dynamic data is
 * handled by a **ring of `frame_slot_count()` regions**: each frame's
 * `write` targets the region indexed by the backend's fenced current
 * frame slot, so no in-flight frame is reading the region being written,
 * and the descriptor is written once (not per frame). The returned
 * region's element base goes into the per-frame push constant.
 *
 * The backing buffer is bound to its slot the first time data is written,
 * and re-bound only when it must grow (a frame's data exceeds the current
 * per-region stride) — rare after warmup, so steady state never touches
 * the descriptor.
 *
 * Chain: IInterface -> IGpuArena
 */
class IGpuArena
    : public Interface<IGpuArena, IInterface,
                       VELK_UID("0267a3dc-1b37-4e65-bc3d-8bc24a6fd099")>
{
public:
    /// One-time setup: @p slot is the set = 1 binding
    /// (IRenderBackend::GlobalBufferSlot); @p element_size keeps the
    /// per-region stride a whole multiple of the element so the returned
    /// offset divides cleanly into an element base.
    virtual void init(uint32_t slot, uint32_t element_size) = 0;

    /// Writes @p size bytes into this frame's ring region (picked from the
    /// backend's current fenced frame slot), growing the buffer and
    /// re-binding the slot only if @p size exceeds the current per-region
    /// stride. Returns the written region; its @c offset / element_size is
    /// the base the shader adds to every index. New bytes are visible to
    /// this frame's shader reads.
    virtual GpuArenaRegion write(const void* data, uint64_t size,
                                 FrameContext& ctx) = 0;

    /// The set = 1 slot this arena's buffer is bound to.
    virtual uint32_t slot() const = 0;

    /// The bound buffer's GPU address. Stable until the buffer grows; used
    /// only as a CPU-side identity token for pipeline cache keys, never by
    /// shaders (which reach the data by index).
    virtual uint64_t gpu_address() const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_GPU_ARENA_H
