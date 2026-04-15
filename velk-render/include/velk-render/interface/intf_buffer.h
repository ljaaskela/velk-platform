#ifndef VELK_RENDER_INTF_BUFFER_H
#define VELK_RENDER_INTF_BUFFER_H

#include <velk-render/interface/intf_gpu_resource.h>

#include <cstdint>
#include <cstddef>

namespace velk {

/**
 * @brief A chunk of CPU-resident bytes that needs to be uploaded to the GPU.
 *
 * `IBuffer` is the unified base for any uploadable GPU resource. It captures
 * the part of the lifecycle that is identical regardless of how the bytes
 * are interpreted on the GPU side: a size, a CPU-resident pointer, a dirty
 * flag, and the IGpuResource observer hooks.
 *
 * Subtypes specialize on the GPU representation:
 *
 * - `ITexture` adds image-shaped metadata (width, height, pixel format) and
 *   is uploaded via the backend's texture path (`upload_texture`), which
 *   produces a bindless image binding.
 * - Future subtypes (vertex buffers, index buffers, BVH nodes, etc.) can
 *   live in this hierarchy without inventing parallel resource interfaces.
 * - A plain IBuffer with no specialization is the right shape for "raw
 *   shader-readable bytes," uploaded via `create_buffer` + `map` + memcpy
 *   and bound via `buffer_reference` (GPU virtual address).
 *
 * Two lifecycles fit the same interface:
 *
 * - **Dynamic source** (e.g. font glyph buffers that grow lazily): keeps
 *   `get_data()` valid, sets `is_dirty()` after mutation, and the renderer
 *   re-uploads on the next frame. The buffer can grow between dirty cycles;
 *   the renderer reallocates the GPU-side handle when this happens.
 * - **Static source** (e.g. a decoded png): `get_data()` returns the bytes
 *   once, `is_dirty()` returns true on first observation, the renderer
 *   uploads, the buffer clears its dirty flag and may free its CPU bytes
 *   (so `get_data()` returns nullptr afterwards).
 *
 * Chain: IInterface -> IGpuResource -> IBuffer
 */
class IBuffer : public Interface<IBuffer, IGpuResource>
{
public:
    /** @brief Returns the size of the CPU-resident byte block, in bytes. */
    virtual size_t get_data_size() const = 0;

    /**
     * @brief Returns the CPU-resident bytes, or nullptr if not available
     *        (e.g. a static resource whose CPU bytes have been freed after
     *        upload, or a GPU-only resource).
     */
    virtual const uint8_t* get_data() const = 0;

    /**
     * @brief Returns true if `get_data()` content (or size) has changed
     *        since the last upload and the renderer should re-upload on the
     *        next frame.
     */
    virtual bool is_dirty() const = 0;

    /**
     * @brief Called by the renderer after uploading from `get_data()`.
     *        Implementations may free their CPU byte buffer here if they no
     *        longer need it (e.g. one-shot static images).
     */
    virtual void clear_dirty() = 0;

    /**
     * @brief Returns the GPU virtual address where this buffer's contents
     *        currently live, or 0 if not yet uploaded.
     *
     * Set by the renderer after `create_buffer` (and after any reallocation
     * caused by a size change). Read by materials that bind the buffer via
     * `buffer_reference` in their shader: the material reads the address
     * directly from the buffer object inside `write_gpu_data` and emits it
     * into the per-draw GPU data.
     *
     * Default returns 0, which is correct for resources that don't bind via
     * GPU virtual address (e.g. textures, which are bound bindlessly through
     * a backend-managed image array index instead).
     */
    virtual uint64_t get_gpu_address() const { return 0; }

    /**
     * @brief Stores the GPU virtual address. Called by the renderer after
     *        upload and after any size-change reallocation.
     *
     * Default is a no-op so resources that don't need address tracking
     * (e.g. textures) can ignore the call.
     */
    virtual void set_gpu_address(uint64_t /*addr*/) {}
};

/**
 * @brief Helper for getting the gpu address of a buffer.
 */
inline uint64_t get_gpu_address(const IBuffer::Ptr& buffer)
{
    return buffer ? buffer->get_gpu_address() : 0;
}

} // namespace velk

#endif // VELK_RENDER_INTF_BUFFER_H
