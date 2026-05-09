#ifndef VELK_VK_GPU_BUFFER_H
#define VELK_VK_GPU_BUFFER_H

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_gpu_buffer.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/platform.h>
#include <vma/vk_mem_alloc.h>
#include <volk/volk.h>

namespace velk::vk {

class VkBackend;

/**
 * @brief Vulkan-backed `IGpuBuffer`. Owns one VkBuffer + VmaAllocation
 *        plus the cached BDA / mapped pointer.
 *
 * Destruction defers the `vmaDestroyBuffer` to the backend's
 * deferred-free queue, keyed by the current pending-frame-completion
 * marker, so an in-flight frame can finish reading before the memory
 * is reclaimed.
 */
class VkGpuBuffer
    : public ::velk::ext::GpuResource<VkGpuBuffer, ::velk::IGpuBuffer>
{
public:
    VELK_CLASS_UID(::velk::ClassId::VkGpuBuffer, "VkGpuBuffer");

    VkGpuBuffer() = default;
    ~VkGpuBuffer() override;

    void init(IRenderBackend* backend, ::VkBuffer buffer, VmaAllocation allocation, void* mapped, size_t size,
              uint64_t address);

    // IGpuResource
    ::velk::GpuResourceType get_type() const override
    {
        return ::velk::GpuResourceType::Buffer;
    }

    // IGpuBuffer
    size_t   size_bytes() const override  { return size_; }
    uint64_t gpu_address() const override { return address_; }
    void*    map() override               { return mapped_; }

    ::VkBuffer    vk_buffer()     const { return buffer_; }
    VmaAllocation vk_allocation() const { return allocation_; }

private:
    IRenderBackend* backend_ = nullptr;
    ::VkBuffer    buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    void*         mapped_ = nullptr;
    size_t        size_ = 0;
    uint64_t      address_ = 0;
};

} // namespace velk::vk

#endif // VELK_VK_GPU_BUFFER_H
