#ifndef VELK_RENDER_GPU_ARENA_H
#define VELK_RENDER_GPU_ARENA_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <algorithm>
#include <cstring>
#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/interface/intf_gpu_buffer.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Suballocating ring-buffer IGpuArena over a single backend buffer.
 *
 * Shared across all producers that target a given set = 1 slot (e.g. every
 * scene's BVH nodes). The buffer is `frame_slot_count()` equal-size
 * sub-buffers; each frame uses the sub-buffer for the backend's current
 * (fence-retired) frame slot, so an in-flight frame never reads the
 * sub-buffer being written. Within a frame, each `write` **bump-allocates**
 * a distinct region inside that sub-buffer, so multiple producers (multiple
 * BVHs) coexist without overwriting each other. The producer pushes the
 * returned region's element base; the shader reads `data[base + index]`.
 *
 * The descriptor is bound once and re-bound only when a frame's total
 * outgrows the sub-buffer stride (rare with the headroom below). The first
 * allocation happens before any in-flight frame uses set = 1 (gated on
 * bvh_node_count == 0), so it is safe; generous sizing keeps it to that one
 * safe allocation in the common case.
 */
class GpuArena : public ::velk::ext::ObjectCore<GpuArena, ::velk::IGpuArena>
{
public:
    VELK_CLASS_UID(::velk::ClassId::GpuArena, "GpuArena");

    void init(uint32_t slot, uint32_t element_size) override
    {
        slot_ = slot;
        element_size_ = element_size ? element_size : 1u;
    }

    GpuArenaRegion write(const void* data, uint64_t size, FrameContext& ctx) override
    {
        if (!ctx.backend) return {};
        const uint32_t regions = ctx.backend->frame_slot_count();
        const uint32_t slot = ctx.backend->current_frame_slot();

        // Slot rotates once per frame, so a slot change marks a new frame:
        // reset the bump so this frame's producers start at the sub-buffer
        // base. Producers within one frame share the slot and bump past each
        // other into distinct regions.
        if (slot != last_slot_) {
            bump_ = 0;
            last_slot_ = slot;
        }

        const uint64_t aligned = (size + element_size_ - 1) / element_size_ * element_size_;

        if (bump_ + aligned > sub_stride_ || !buffer_) {
            // Grow the per-frame sub-buffer with 4x headroom over a 1 MiB
            // floor so the stride settles on the first frame and does not
            // grow again (a later growth re-binds the shared descriptor while
            // in-flight frames read it). Element-aligned so offset / element
            // stays integral.
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

    ArenaRegion alloc(uint64_t size, FrameContext& ctx) override
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

    void write_at(uint64_t offset, const void* data, uint64_t size) override
    {
        if (persistent_mapped_ && data && size) {
            std::memcpy(static_cast<char*>(persistent_mapped_) + offset, data,
                        static_cast<size_t>(size));
        }
    }

    void release_region(uint64_t offset, uint64_t size) override
    {
        // Defer the reclaim past the in-flight frame: the range may still be
        // read by GPU work already submitted this frame, so tag it with that
        // frame's completion marker and return it to the free-list only once
        // the marker retires (drain_zombies).
        const uint64_t marker =
            backend_ ? backend_->pending_frame_completion_marker() : 0;
        zombies_.push_back({offset, size, marker});
    }

    uint32_t slot() const override { return slot_; }

private:
    void recreate_buffer(uint32_t regions, FrameContext& ctx)
    {
        GpuBufferDesc desc{};
        desc.size = uint64_t(regions) * sub_stride_;
        desc.cpu_writable = true;
        // Reassigning buffer_ drops the old Ptr; ~VkGpuBuffer defers its
        // destruction past in-flight frames on its own, so we must NOT also
        // defer it explicitly here (that double-queues the same VkBuffer and
        // double-frees it in drain_deferred_buffers).
        buffer_ = ctx.backend->create_gpu_buffer(desc);
        mapped_ = buffer_ ? buffer_->map() : nullptr;
        if (buffer_) {
            ctx.backend->set_global_buffer(slot_, buffer_.get());
        }
    }

    // Grows the persistent buffer to >= @p want bytes, copying live contents
    // forward so every outstanding region offset stays valid.
    bool grow_persistent(uint64_t want, FrameContext& ctx)
    {
        want = (want + element_size_ - 1) / element_size_ * element_size_;
        GpuBufferDesc desc{};
        desc.size = want;
        desc.cpu_writable = true;
        auto new_buffer = ctx.backend->create_gpu_buffer(desc);
        if (!new_buffer) return false;
        void* new_mapped = new_buffer->map();
        if (new_mapped && persistent_mapped_ && persistent_capacity_) {
            std::memcpy(new_mapped, persistent_mapped_,
                        static_cast<size_t>(persistent_capacity_));
        }
        // Reassigning drops the old Ptr; ~VkGpuBuffer defers its own destroy
        // past in-flight frames, so we must NOT also explicit-defer it here.
        persistent_buffer_ = std::move(new_buffer);
        persistent_mapped_ = new_mapped;
        persistent_capacity_ = want;
        ctx.backend->set_global_buffer(slot_, persistent_buffer_.get());
        return true;
    }

    // Returns freed regions to the free-list once their in-flight frame has
    // completed (so the range is never reused while still being read).
    void drain_zombies()
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

    // Merges adjacent free spans so fragmentation stays bounded.
    void coalesce_free()
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

    struct Zombie { uint64_t offset; uint64_t size; uint64_t marker; };

    uint32_t slot_ = 0;
    uint32_t element_size_ = 1;

    // Ring (transient) state, used by write().
    uint64_t sub_stride_ = 0;            ///< Bytes per per-frame sub-buffer.
    uint64_t bump_ = 0;                  ///< Bytes used in the current frame's sub-buffer.
    uint32_t last_slot_ = 0xFFFFFFFFu;   ///< Sentinel forces a bump reset on the first write.
    IGpuBuffer::Ptr buffer_;
    void* mapped_ = nullptr;

    // Persistent (alloc) state. Separate buffer from the ring; a given arena
    // instance uses one style, so only one buffer is created.
    IRenderBackend* backend_ = nullptr;  ///< Cached for release_region / drain.
    IGpuBuffer::Ptr persistent_buffer_;
    void* persistent_mapped_ = nullptr;
    uint64_t persistent_capacity_ = 0;
    vector<GpuArenaRegion> free_spans_;
    vector<Zombie> zombies_;             ///< Freed regions awaiting their fence.
};

} // namespace velk::impl

#endif // VELK_RENDER_GPU_ARENA_H
