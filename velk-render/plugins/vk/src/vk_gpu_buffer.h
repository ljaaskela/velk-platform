#ifndef VELK_VK_GPU_BUFFER_H
#define VELK_VK_GPU_BUFFER_H

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_gpu_buffer.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/platform.h>
#include <vma/vk_mem_alloc.h>
#include <volk/volk.h>

namespace velk::vk {

/**
 * @brief Backend-internal sibling of `IGpuBuffer` exposing the
 *        Vulkan handles. `record_draw_loop` and friends
 *        `interface_cast` to this to extract `VkBuffer` without
 *        depending on the concrete `VkGpuBuffer` type.
 */
class IVkGpuBuffer
    : public ::velk::Interface<IVkGpuBuffer, ::velk::IInterface,
                               VELK_UID("af838c8c-b166-49b0-81db-d13faeb11db4")>
{
public:
    virtual void init(::velk::IRenderBackend* backend,
                      ::VkBuffer buffer, VmaAllocation allocation,
                      void* mapped, size_t size, uint64_t address) = 0;
    virtual ::VkBuffer    vk_buffer()     const = 0;
    virtual VmaAllocation vk_allocation() const = 0;
};

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
    : public ::velk::ext::GpuResource<VkGpuBuffer, ::velk::IGpuBuffer, IVkGpuBuffer>
{
public:
    VELK_CLASS_UID(::velk::ClassId::VkGpuBuffer, "VkGpuBuffer");

    VkGpuBuffer() = default;
    ~VkGpuBuffer() override;

    // IGpuResource
    ::velk::GpuResourceType get_type() const override
    {
        return ::velk::GpuResourceType::Buffer;
    }

    // IGpuBuffer
    size_t   size_bytes() const override  { return size_; }
    uint64_t gpu_address() const override { return address_; }
    void*    map() override               { return mapped_; }
    void     update(size_t offset, size_t size, const void* data) override;

    // IVkGpuBuffer
    void init(::velk::IRenderBackend* backend,
              ::VkBuffer buffer, VmaAllocation allocation,
              void* mapped, size_t size, uint64_t address) override;
    ::VkBuffer    vk_buffer()     const override { return buffer_; }
    VmaAllocation vk_allocation() const override { return allocation_; }

private:
    ::velk::IRenderBackend* backend_ = nullptr;
    ::VkBuffer    buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    void*         mapped_ = nullptr;
    size_t        size_ = 0;
    uint64_t      address_ = 0;
};

} // namespace velk::vk

#endif // VELK_VK_GPU_BUFFER_H
