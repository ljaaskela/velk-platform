#include "vk_gpu_buffer.h"

namespace velk::vk {

VkGpuBuffer::~VkGpuBuffer()
{
    if (buffer_ == VK_NULL_HANDLE || backend_ == nullptr) return;
    // Defer here while derived members + vtable are still intact —
    // observer callbacks fire from ~ext::GpuResource after this body
    // returns and would hit the now-pure IVkGpuBuffer virtuals.
    backend_->defer_destroy_gpu_buffer(this);
}

void VkGpuBuffer::init(IRenderBackend* backend, ::VkBuffer buffer, VmaAllocation allocation,
                       void* mapped, size_t size, uint64_t address)
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
    backend_->record_buffer_update(*this, offset, size, data);
}

} // namespace velk::vk
