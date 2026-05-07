#ifndef VELK_RENDER_RESOURCE_GPU_BUFFER_H
#define VELK_RENDER_RESOURCE_GPU_BUFFER_H

#include <velk/vector.h>

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/plugin.h>

#include <cstdint>
#include <cstring>

namespace velk::impl {

/**
 * @brief Generic CPU-resident byte blob with a backend handle observable
 *        through `IGpuResource`.
 *
 * Hive-pooled via `velk::instance().create<IBuffer>(ClassId::GpuBuffer)`.
 * Composed by any owner that needs persistent GPU-side storage without
 * being a GPU resource itself (e.g. `DefaultBatch`'s indirect-args +
 * count + instance blob, ViewPreparer's per-view lights / env_data,
 * SceneBvh's nodes / shapes / mesh-instance arrays). The renderer's
 * standard upload pipeline (`IGpuResourceManager::ensure_buffer_storage`
 * + `map` + `memcpy(get_data())`) writes the blob to the GPU when
 * `is_dirty()` returns true; the device address lands in
 * `set_gpu_handle(GpuResourceKey::Default, ...)`.
 *
 * Owners stage bytes through the `IBuffer::write_diff` interface (the
 * `PersistentBuffer` ext helper wraps the lazy-create + write_diff +
 * upload pipeline). Direct access to the underlying byte vector is
 * intentionally not exposed on the interface — `write_diff` is the
 * supported mutation path.
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

    using IBuffer::write;  // bring the lambda-friendly template overload into scope.
    bool write(size_t sz, WriteFn fn, void* ctx) override
    {
        if (sz == 0 || !fn) {
            if (!data_.empty()) {
                data_.clear();
                dirty_ = true;
                return true;
            }
            return false;
        }
        pending_.assign(sz, 0);
        fn(pending_.data(), sz, ctx);
        if (data_.size() == sz
            && std::memcmp(data_.data(), pending_.data(), sz) == 0) {
            return false;
        }
        std::swap(data_, pending_);
        dirty_ = true;
        return true;
    }

    bool write_diff(const void* bytes, size_t size) override
    {
        // Fast no-change path: bypass the `write` callback's
        // mandatory pending+memcpy+memcmp dance. write_diff's whole
        // point is gating uploads cheaply when bytes are stable, so
        // we memcmp against `data_` directly and return.
        if (data_.size() == size
            && (size == 0 || std::memcmp(data_.data(), bytes, size) == 0)) {
            return false;
        }
        data_.resize(size);
        if (size) std::memcpy(data_.data(), bytes, size);
        dirty_ = true;
        return true;
    }

private:
    ::velk::vector<uint8_t> data_;     ///< Committed bytes.
    ::velk::vector<uint8_t> pending_;  ///< Reused fill scratch, diffed vs `data_`.
    bool dirty_ = false;
};

} // namespace velk::impl

#endif // VELK_RENDER_RESOURCE_GPU_BUFFER_H
