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
 * Objects that also implement `ISurface` (images, environment maps) are
 * uploaded via the backend's texture path (`upload_texture`). Plain IBuffer
 * objects are uploaded via `create_buffer` + `map` + memcpy and read by
 * shaders via a buffer-reference / device-address dereference.
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

    // GPU virtual address access goes through `IGpuResource::get_gpu_handle` /
    // `set_gpu_handle` with `GpuResourceKey::Default`. The renderer
    // populates the address after `create_buffer` (and after any
    // size-change reallocation); materials read it via
    // `buffer->get_gpu_handle(GpuResourceKey::Default)` inside
    // `write_gpu_data` and dereference it from shaders via a
    // buffer-reference / device-address read.
};

/**
 * @brief Convenience: returns the buffer's primary GPU handle (device address)
 *        for any pointer type that holds an `IBuffer`. Returns 0 if null.
 */
template <typename T>
uint64_t get_gpu_address(const T& ptr)
{
    auto* buf = interface_cast<IBuffer>(ptr);
    return buf ? buf->get_gpu_handle(GpuResourceKey::Default) : 0;
}

} // namespace velk

#endif // VELK_RENDER_INTF_BUFFER_H
