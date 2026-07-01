#include "gpu_arena.h"

#include <algorithm>
#include <cstring>

#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/render_path/frame_context.h>

namespace velk::impl {

void GpuArena::init(uint32_t slot, uint32_t element_size)
{
    slot_ = slot;
    element_size_ = element_size ? element_size : 1u;
}

GpuArenaRegion GpuArena::write(const void* data, uint64_t size, FrameContext& ctx)
{
    if (!ctx.backend) return {};
    const uint32_t regions = ctx.backend->frame_slot_count();
    const uint32_t slot = ctx.backend->current_frame_slot();

    // Slot rotates once per frame, so a slot change marks a new frame: reset
    // the bump so this frame's producers start at the sub-buffer base.
    // Producers within one frame share the slot and bump past each other into
    // distinct regions.
    if (slot != last_slot_) {
        bump_ = 0;
        last_slot_ = slot;
    }

    const uint64_t aligned = (size + element_size_ - 1) / element_size_ * element_size_;

    if (bump_ + aligned > sub_stride_ || !buffer_) {
        // Grow the per-frame sub-buffer with 4x headroom over a 1 MiB floor so
        // the stride settles on the first frame and does not grow again (a
        // later growth re-binds the shared descriptor while in-flight frames
        // read it). Element-aligned so offset / element stays integral.
        uint64_t want = sub_stride_;
        if (bump_ + aligned > want) {
            want = (bump_ + aligned) * 4;
            constexpr uint64_t kFloor = uint64_t(1) << 20;  // 1 MiB
            if (want < kFloor) want = kFloor;
        }
        want = (want + element_size_ - 1) / element_size_ * element_size_;
        sub_stride_ = want;
        recreate_buffer(regions, ctx);
    }

    const uint64_t offset = uint64_t(slot) * sub_stride_ + bump_;
    if (mapped_ && size) {
        std::memcpy(static_cast<char*>(mapped_) + offset, data, static_cast<size_t>(size));
    }
    bump_ += aligned;
    return GpuArenaRegion{offset, size};
}

ArenaRegion GpuArena::alloc(uint64_t size, FrameContext& ctx)
{
    if (!ctx.backend || size == 0) return {};
    backend_ = ctx.backend;
    drain_zombies();

    const uint64_t need = (size + element_size_ - 1) / element_size_ * element_size_;
    if (!persistent_buffer_) {
        uint64_t cap = need * 4;
        constexpr uint64_t kFloor = uint64_t(1) << 20;  // 1 MiB
        if (cap < kFloor) cap = kFloor;
        if (!grow_persistent(cap, ctx)) return {};
        free_spans_.push_back({0, persistent_capacity_});
    }

    for (;;) {
        for (size_t i = 0; i < free_spans_.size(); ++i) {
            if (free_spans_[i].size < need) continue;
            const uint64_t offset = free_spans_[i].offset;
            if (free_spans_[i].size == need) {
                free_spans_.erase(free_spans_.begin() + static_cast<long>(i));
            } else {
                free_spans_[i].offset += need;
                free_spans_[i].size -= need;
            }
            return ArenaRegion{this, offset, need};
        }
        // No span fits: grow (the fresh tail becomes a free span) and retry.
        const uint64_t old_cap = persistent_capacity_;
        uint64_t want = persistent_capacity_ * 2;
        if (want < persistent_capacity_ + need) want = persistent_capacity_ + need;
        if (!grow_persistent(want, ctx)) return {};
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

void GpuArena::recreate_buffer(uint32_t regions, FrameContext& ctx)
{
    GpuBufferDesc desc{};
    desc.size = uint64_t(regions) * sub_stride_;
    desc.cpu_writable = true;
    // Buffer from the resource manager (tracked + deferred-destroy on
    // reassignment); the backend only binds it to the set = 1 slot. Reassigning
    // buffer_ drops the old Ptr; ~VkGpuBuffer defers its own destroy past
    // in-flight frames, so we must NOT also explicit-defer it here.
    buffer_ = ctx.resources ? ctx.resources->create_gpu_buffer(desc)
                            : ctx.backend->create_gpu_buffer(desc);
    mapped_ = buffer_ ? buffer_->map() : nullptr;
    if (buffer_) {
        ctx.backend->set_global_buffer(slot_, buffer_.get());
    }
}

bool GpuArena::grow_persistent(uint64_t want, FrameContext& ctx)
{
    want = (want + element_size_ - 1) / element_size_ * element_size_;
    GpuBufferDesc desc{};
    desc.size = want;
    desc.cpu_writable = true;
    auto new_buffer = ctx.resources ? ctx.resources->create_gpu_buffer(desc)
                                     : ctx.backend->create_gpu_buffer(desc);
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
    ctx.backend->set_global_buffer(slot_, persistent_buffer_.get());
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
