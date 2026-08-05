#include "gpu_arena.h"

#include "arena_buffer.h"

#include <algorithm>
#include <cstring>

#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/render_path/frame_context.h>

namespace velk::impl {

void GpuArena::init(uint32_t slot, uint32_t element_size,
                    IGpuResourceManager* resources, IRenderBackend* backend)
{
    slot_ = slot;
    element_size_ = element_size ? element_size : 1u;
    resources_ = resources;
    backend_ = backend;
}

ArenaRegion GpuArena::alloc(uint64_t size, uint64_t alignment)
{
    if (!backend_ || size == 0) return {};
    drain_zombies();

    const uint64_t align = alignment ? alignment : element_size_;
    const uint64_t need = (size + element_size_ - 1) / element_size_ * element_size_;
    if (!persistent_buffer_) {
        uint64_t cap = need * 4;
        constexpr uint64_t kFloor = uint64_t(1) << 20;  // 1 MiB
        if (cap < kFloor) cap = kFloor;
        if (!grow_persistent(cap)) return {};
        free_spans_.push_back({0, persistent_capacity_});
    }

    for (;;) {
        for (size_t i = 0; i < free_spans_.size(); ++i) {
            const uint64_t span_off  = free_spans_[i].offset;
            const uint64_t span_size = free_spans_[i].size;
            // Align the region's start; the skipped bytes become a free span
            // so a later smaller-alignment alloc can reuse them.
            const uint64_t aligned = (span_off + align - 1) / align * align;
            const uint64_t pad = aligned - span_off;
            if (span_size < pad + need) continue;
            free_spans_.erase(free_spans_.begin() + static_cast<long>(i));
            if (pad > 0) free_spans_.push_back({span_off, pad});
            const uint64_t tail_off = aligned + need;
            const uint64_t tail_size = (span_off + span_size) - tail_off;
            if (tail_size > 0) free_spans_.push_back({tail_off, tail_size});
            return ArenaRegion{this, aligned, need};
        }
        // No span fits: grow (the fresh tail becomes a free span) and retry.
        const uint64_t old_cap = persistent_capacity_;
        uint64_t want = persistent_capacity_ * 2;
        if (want < persistent_capacity_ + need + align) want = persistent_capacity_ + need + align;
        if (!grow_persistent(want)) return {};
        free_spans_.push_back({old_cap, persistent_capacity_ - old_cap});
        coalesce_free();
    }
}

void GpuArena::write_at(uint64_t offset, const void* data, uint64_t size)
{
    if (persistent_mapped_ && data && size) {
        std::memcpy(static_cast<char*>(persistent_mapped_) + offset, data,
                    static_cast<size_t>(size));
    }
}

void GpuArena::release_region(uint64_t offset, uint64_t size)
{
    // Defer the reclaim past the in-flight frame: the range may still be read
    // by GPU work already submitted this frame, so tag it with that frame's
    // completion marker and return it to the free-list only once the marker
    // retires (drain_zombies).
    const uint64_t marker =
        backend_ ? backend_->pending_frame_completion_marker() : 0;
    zombies_.push_back({offset, size, marker});
}

void GpuArena::reclaim() { drain_zombies(); }

IBuffer::Ptr GpuArena::create_buffer(uint64_t size)
{
    auto buf = ::velk::instance().create<IBuffer>(ClassId::ArenaBuffer);
    if (!buf) return {};
    if (auto* ab = interface_cast<IArenaBufferInternal>(buf)) {
        ab->init(this, size);
    }
    return buf;
}

void* GpuArena::mapped_at(uint64_t offset)
{
    if (!persistent_mapped_ || offset >= persistent_capacity_) return nullptr;
    return static_cast<char*>(persistent_mapped_) + offset;
}

bool GpuArena::grow_persistent(uint64_t want)
{
    want = (want + element_size_ - 1) / element_size_ * element_size_;
    GpuBufferDesc desc{};
    desc.size = want;
    desc.cpu_writable = true;
    auto new_buffer = resources_ ? resources_->create_gpu_buffer(desc)
                                 : backend_->create_gpu_buffer(desc);
    if (!new_buffer) return false;
    void* new_mapped = new_buffer->map();
    if (new_mapped && persistent_mapped_ && persistent_capacity_) {
        std::memcpy(new_mapped, persistent_mapped_,
                    static_cast<size_t>(persistent_capacity_));
    }
    // Reassigning drops the old Ptr; ~VkGpuBuffer defers its own destroy past
    // in-flight frames, so we must NOT also explicit-defer it here.
    persistent_buffer_ = std::move(new_buffer);
    persistent_mapped_ = new_mapped;
    persistent_capacity_ = want;
    backend_->set_global_buffer(slot_, persistent_buffer_.get());
    return true;
}

void GpuArena::drain_zombies()
{
    if (zombies_.empty() || !backend_) return;
    bool freed = false;
    for (size_t i = 0; i < zombies_.size();) {
        if (backend_->is_frame_complete(zombies_[i].marker)) {
            free_spans_.push_back({zombies_[i].offset, zombies_[i].size});
            zombies_[i] = zombies_.back();
            zombies_.pop_back();
            freed = true;
        } else {
            ++i;
        }
    }
    if (freed) coalesce_free();
}

void GpuArena::coalesce_free()
{
    if (free_spans_.size() < 2) return;
    std::sort(free_spans_.begin(), free_spans_.end(),
              [](const GpuArenaRegion& a, const GpuArenaRegion& b) {
                  return a.offset < b.offset;
              });
    vector<GpuArenaRegion> merged;
    merged.push_back(free_spans_[0]);
    for (size_t i = 1; i < free_spans_.size(); ++i) {
        auto& last = merged.back();
        if (last.offset + last.size == free_spans_[i].offset) {
            last.size += free_spans_[i].size;
        } else {
            merged.push_back(free_spans_[i]);
        }
    }
    free_spans_ = std::move(merged);
}

} // namespace velk::impl
