#ifndef VELK_RENDER_EXT_GPU_BUFFER_H
#define VELK_RENDER_EXT_GPU_BUFFER_H

#include <velk/vector.h>

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/plugin.h>

#include <cstdint>

namespace velk::impl {

/**
 * @brief Generic CPU-resident byte blob with a backend handle observable
 *        through `IGpuResource`.
 *
 * Hive-pooled via `velk::instance().create<IBuffer>(ClassId::GpuBuffer)`.
 * Composed by any owner that needs persistent GPU-side storage without
 * being a GPU resource itself (e.g. `DefaultBatch`'s indirect-args + count
 * + instance blob). The renderer's standard upload pipeline
 * (`IGpuResourceManager::ensure_buffer_storage` + `map` + `memcpy(get_data())`)
 * writes the blob to the GPU when `is_dirty()` returns true; the device
 * address lands in `set_gpu_handle(GpuResourceKey::Default, ...)`.
 *
 * Owners mutate the blob via `mutable_data()` (which sets dirty) and
 * release it by dropping the `IBuffer::Ptr`; the resource manager's
 * observer cascade defers backend destruction.
 */
class GpuBuffer : public ext::GpuResource<GpuBuffer, IBuffer>
{
public:
    VELK_CLASS_UID(::velk::ClassId::GpuBuffer, "GpuBuffer");

    GpuResourceType get_type() const override { return GpuResourceType::Buffer; }

    // IBuffer
    size_t get_data_size() const override { return data_.size(); }
    const uint8_t* get_data() const override
    {
        return data_.empty() ? nullptr : data_.data();
    }
    bool is_dirty() const override { return dirty_; }
    void clear_dirty() override { dirty_ = false; }

    /// Access the bytes for in-place mutation. Marks the buffer dirty so
    /// the next upload pass re-uploads (or, if the size changed,
    /// re-allocates and uploads).
    ::velk::vector<uint8_t>& mutable_data()
    {
        dirty_ = true;
        return data_;
    }

    const ::velk::vector<uint8_t>& data() const { return data_; }

    /// Owner has mutated bytes through a previously-mapped pointer
    /// (write-through path). The buffer's `data_` already matches GPU
    /// memory, but we still flag dirty so next size-change still
    /// reuploads correctly. Use sparingly; the typical mutation path is
    /// `mutable_data()`.
    void set_dirty() { dirty_ = true; }

private:
    ::velk::vector<uint8_t> data_;
    bool dirty_ = false;
};

} // namespace velk::impl

#endif // VELK_RENDER_EXT_GPU_BUFFER_H
