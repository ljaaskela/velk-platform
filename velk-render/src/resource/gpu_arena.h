#ifndef VELK_RENDER_GPU_ARENA_H
#define VELK_RENDER_GPU_ARENA_H

#include <velk/ext/object.h>

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

    uint32_t slot_ = 0;
    uint32_t element_size_ = 1;
    uint64_t sub_stride_ = 0;            ///< Bytes per per-frame sub-buffer.
    uint64_t bump_ = 0;                  ///< Bytes used in the current frame's sub-buffer.
    uint32_t last_slot_ = 0xFFFFFFFFu;   ///< Sentinel forces a bump reset on the first write.
    IGpuBuffer::Ptr buffer_;
    void* mapped_ = nullptr;
};

} // namespace velk::impl

#endif // VELK_RENDER_GPU_ARENA_H
