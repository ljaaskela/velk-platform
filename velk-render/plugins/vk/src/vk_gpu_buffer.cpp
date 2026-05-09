#include "vk_gpu_buffer.h"

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

} // namespace velk::vk
