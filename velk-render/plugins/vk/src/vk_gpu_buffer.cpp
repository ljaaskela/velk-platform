#include "vk_gpu_buffer.h"

#include "vk_backend.h"

namespace velk::vk {

VkGpuBuffer::~VkGpuBuffer()
{
    if (buffer_ == VK_NULL_HANDLE || backend_ == nullptr) return;
    if (!is_managed()) {
        // Nobody's managing us so destroy immediately.
        backend_->defer_destroy_gpu_buffer(this);
    }
}

void VkGpuBuffer::init(IRenderBackend* backend, ::VkBuffer buffer, VmaAllocation allocation, void* mapped,
                       size_t size, uint64_t address)
{
    assert(buffer_ == VK_NULL_HANDLE);
    backend_ = backend;
    buffer_ = buffer;
    allocation_ = allocation;
    mapped_ = mapped;
    size_ = size;
    address_ = address;
}

void VkGpuBuffer::update(size_t offset, size_t size, const void* data)
{
    if (buffer_ == VK_NULL_HANDLE || backend_ == nullptr || size == 0) return;
    auto* be = static_cast<VkBackend*>(backend_);
    vkCmdUpdateBuffer(be->primary_cb(), buffer_, offset, size, data);
    be->mark_pending_buffer_update_barrier();
}

} // namespace velk::vk
