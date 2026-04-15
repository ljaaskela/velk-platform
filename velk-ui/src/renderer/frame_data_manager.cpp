#include "frame_data_manager.h"

#include <velk/api/velk.h>

#include <cstring>

namespace velk::ui {

uint64_t FrameDataManager::write(const void* data, size_t size, size_t alignment)
{
    auto r = reserve(size, alignment);
    if (!r.ptr) {
        return 0;
    }
    std::memcpy(r.ptr, data, size);
    return r.gpu_addr;
}

FrameDataManager::WriteResult FrameDataManager::reserve(size_t size, size_t alignment)
{
    write_offset_ = (write_offset_ + alignment - 1) & ~(alignment - 1);

    if (write_offset_ + size > buffer_size_) {
        size_t required = write_offset_ + size;
        if (required > peak_usage_) {
            peak_usage_ = required;
        }
        overflow_ = true;
        return {};
    }

    auto* ptr = static_cast<uint8_t*>(active_->ptr) + write_offset_;
    uint64_t gpu_addr = active_->gpu_base + write_offset_;
    write_offset_ += size;
    if (write_offset_ > peak_usage_) {
        peak_usage_ = write_offset_;
    }

    return {ptr, gpu_addr};
}

void FrameDataManager::begin_frame(Slot& slot)
{
    active_ = &slot;
    write_offset_ = 0;
    overflow_ = false;
}

void FrameDataManager::ensure_capacity(IRenderBackend& backend)
{
    if (peak_usage_ <= buffer_size_ * 3 / 4) {
        return;
    }

    size_t new_size = buffer_size_;
    while (new_size < peak_usage_ * 2) {
        new_size *= 2;
    }

    VELK_LOG(I,
             "Renderer: growing frame data %zu -> %zu KB (peak usage: %zu KB)",
             buffer_size_ / 1024,
             new_size / 1024,
             peak_usage_ / 1024);

    buffer_size_ = new_size;
    peak_usage_ = 0;

    if (active_ && active_->buffer_size < new_size) {
        alloc_slot(*active_, backend);
    }
}

void FrameDataManager::grow(IRenderBackend& backend)
{
    size_t new_size = buffer_size_ * 4;
    while (new_size < peak_usage_ * 2) {
        new_size *= 2;
    }

    VELK_LOG(I,
             "Renderer: frame data overflow, growing %zu -> %zu KB (need %zu KB)",
             buffer_size_ / 1024,
             new_size / 1024,
             peak_usage_ / 1024);

    buffer_size_ = new_size;
    peak_usage_ = 0;
    alloc_slot(*active_, backend);
}

void FrameDataManager::alloc_slot(Slot& slot, IRenderBackend& backend)
{
    if (slot.handle) {
        backend.destroy_buffer(slot.handle);
    }
    GpuBufferDesc desc;
    desc.size = buffer_size_;
    desc.cpu_writable = true;
    slot.handle = backend.create_buffer(desc);
    slot.ptr = backend.map(slot.handle);
    slot.gpu_base = backend.gpu_address(slot.handle);
    slot.buffer_size = buffer_size_;
}

void FrameDataManager::init_slot(Slot& slot, IRenderBackend& backend)
{
    if (buffer_size_ == 0) {
        return;
    }
    alloc_slot(slot, backend);
}

void FrameDataManager::ensure_slot(Slot& slot, IRenderBackend& backend)
{
    if (slot.buffer_size >= buffer_size_) {
        return;
    }
    alloc_slot(slot, backend);
}

} // namespace velk::ui
