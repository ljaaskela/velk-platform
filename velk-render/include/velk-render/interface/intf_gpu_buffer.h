#ifndef VELK_RENDER_INTF_GPU_BUFFER_H
#define VELK_RENDER_INTF_GPU_BUFFER_H

#include <velk-render/interface/intf_gpu_resource.h>

#include <cstddef>
#include <cstdint>

namespace velk {

/**
 * @brief Raw GPU memory with a stable buffer device address.
 *
 * Backend-allocated, Ptr-managed. Dropping the last reference defers
 * destruction through the backend's frame-completion-marker queue so
 * an in-flight frame can finish reading before the memory is freed.
 *
 * `IBuffer` (CPU shadow + dirty tracking + write API) extends this
 * interface. Producers that don't need a CPU shadow can hold a bare
 * `IGpuBuffer::Ptr` directly.
 *
 * Chain: IInterface -> IGpuResource -> IGpuBuffer
 */
class IGpuBuffer
    : public Interface<IGpuBuffer, IGpuResource,
                       VELK_UID("64a5dae4-002d-4294-adad-11058f699781")>
{
public:
    virtual size_t   size_bytes() const = 0;
    virtual uint64_t gpu_address() const = 0;

    /// Persistent CPU mapping for host-visible allocations; nullptr
    /// when device-local.
    virtual void* map() = 0;

    /// Schedules an in-place update of @p size bytes at @p offset.
    /// @p data is copied; the caller may release it on return. The
    /// new bytes are guaranteed visible to subsequent shader reads
    /// in this frame.
    virtual void update(size_t offset, size_t size, const void* data) = 0;
};

/**
 * @brief Implemented by objects that hold a `IGpuBuffer` as their
 *        GPU storage. The resource manager calls `attach_gpu_buffer`
 *        after allocating the backing storage and again with a null
 *        Ptr to detach.
 */
class IGpuBufferStorageOwner
    : public Interface<IGpuBufferStorageOwner, IInterface,
                       VELK_UID("969fe6b0-d35c-4b62-884d-f71cbfbe9168")>
{
public:
    virtual void attach_gpu_buffer(IGpuBuffer::Ptr gb) = 0;

    /// The attached GPU storage, or null before attach. Lets a backend
    /// reach the concrete GPU buffer (e.g. to bind it into a descriptor)
    /// from a CPU-shadow IBuffer wrapper, whose own IGpuBuffer methods
    /// forward to this storage but cannot expose the backend handle.
    virtual IGpuBuffer::Ptr attached_gpu_buffer() const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_GPU_BUFFER_H
