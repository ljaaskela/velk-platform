#ifndef VELK_UI_FRAME_DATA_MANAGER_H
#define VELK_UI_FRAME_DATA_MANAGER_H

#include <velk/ext/core_object.h>

#include <velk-render/interface/intf_frame_data_manager.h>
#include <velk-render/plugin.h>

#include <cstdint>

namespace velk {

/**
 * @brief Concrete IFrameDataManager backed by a per-slot persistently
 *        mapped GPU buffer. Renderer holds one of these and one Slot
 *        per in-flight frame.
 */
class FrameDataManager : public ext::ObjectCore<FrameDataManager, IFrameDataManager>
{
public:
    VELK_CLASS_UID(ClassId::FrameDataManager, "FrameDataManager");

    void init(size_t initial_size = kInitialSize) override { buffer_size_ = initial_size; }

    uint64_t write(const void* data, size_t size, size_t alignment = 16) override;
    WriteResult reserve(size_t size, size_t alignment = 16) override;

    void init_slot(Slot& slot, IRenderBackend& backend, IGpuResourceManager& resources) override;
    void ensure_slot(Slot& slot, IRenderBackend& backend, IGpuResourceManager& resources) override;

    void begin_frame(Slot& slot) override;
    void ensure_capacity(IRenderBackend& backend, IGpuResourceManager& resources) override;
    void grow(IRenderBackend& backend, IGpuResourceManager& resources) override;

    bool overflowed() const override { return overflow_; }
    size_t get_buffer_size() const override { return buffer_size_; }
    size_t get_peak_usage() const override { return peak_usage_; }

    GpuBufferHandle active_buffer() const override { return active_ ? active_->handle : 0; }
    uint64_t active_buffer_base() const override { return active_ ? active_->gpu_base : 0; }

private:
    void alloc_slot(Slot& slot, IRenderBackend& backend, IGpuResourceManager& resources);

    size_t buffer_size_ = 0;
    size_t write_offset_ = 0;
    size_t peak_usage_ = 0;
    bool overflow_ = false;
    Slot* active_ = nullptr;
};

} // namespace velk

#endif // VELK_UI_FRAME_DATA_MANAGER_H
