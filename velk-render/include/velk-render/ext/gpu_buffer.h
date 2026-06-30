#ifndef VELK_RENDER_EXT_GPU_BUFFER_H
#define VELK_RENDER_EXT_GPU_BUFFER_H

#include <velk/vector.h>

#include <cstdint>
#include <cstring>
#include <utility>
#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_gpu_buffer.h>

namespace velk::ext {

/**
 * @brief CRTP base for byte-blob `IBuffer` implementations.
 *
 * Provides:
 *   - `data_` (committed bytes), `pending_` (reused fill scratch),
 *     `dirty_` (upload signal) — protected so derived classes can
 *     specialise (e.g. `MeshBuffer` clears `data_` post-upload).
 *   - Default implementations of `get_data_size`, `get_data`,
 *     `is_dirty`, `clear_dirty`, `write_diff`, and the callback
 *     `write` — covers the standard "owner stages bytes; renderer
 *     uploads on dirty" flow.
 *   - `IGpuResource::get_type` returns `Buffer`.
 *
 * Derived classes typically only need:
 *   - `VELK_CLASS_UID(...)` for type registration.
 *   - Specialised methods (e.g. `IMeshBuffer::set_data`).
 *   - Optional overrides of any IBuffer method when default doesn't fit
 *     (e.g. `MeshBuffer::clear_dirty` releases CPU bytes after upload;
 *     `MeshBuffer::get_data_size` returns the logical vbo+ibo size
 *     that survives `clear_dirty`).
 *
 * Usage:
 * ```cpp
 * class GpuBuffer
 *     : public ext::GpuBuffer<GpuBuffer, IBuffer, IGpuBuffer, IGpuBufferStorageOwner> {
 *   public:
 *     VELK_CLASS_UID(ClassId::GpuBuffer, "GpuBuffer");
 * };
 * ```
 *
 * The interface chain must include `IBuffer`, `IGpuBuffer`, and
 * `IGpuBufferStorageOwner` (in any order alongside any specialised
 * sub-interface like `IProgramDataBuffer` or `IMeshBuffer`).
 */
template <class FinalClass, class... Interfaces>
class GpuBuffer : public GpuResource<FinalClass, Interfaces...>
{
public:
    ::velk::GpuResourceType get_type() const override { return ::velk::GpuResourceType::Buffer; }

    // IGpuBuffer (null-safe forwards to the attached storage)
    size_t   size_bytes() const override { return gpu_buffer_ ? gpu_buffer_->size_bytes() : 0; }
    uint64_t gpu_address() const override { return gpu_buffer_ ? gpu_buffer_->gpu_address() : 0; }
    void*    map() override          { return gpu_buffer_ ? gpu_buffer_->map() : nullptr; }
    void     update(size_t offset, size_t size, const void* data) override
    {
        if (gpu_buffer_) gpu_buffer_->update(offset, size, data);
    }

    // IGpuBufferStorageOwner
    void attach_gpu_buffer(IGpuBuffer::Ptr gb) override { gpu_buffer_ = std::move(gb); }
    IGpuBuffer::Ptr attached_gpu_buffer() const override { return gpu_buffer_; }
    const IGpuBuffer::Ptr& gpu_buffer() const { return gpu_buffer_; }

    // IBuffer
    size_t get_data_size() const override { return data_.size(); }
    const uint8_t* get_data() const override { return data_.empty() ? nullptr : data_.data(); }
    bool is_dirty() const override { return dirty_; }
    void clear_dirty() override { dirty_ = false; }

    using ::velk::IBuffer::write; // bring the lambda overload into scope.

    bool write(size_t sz, IBuffer::WriteFn fn, void* ctx) override
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
        if (data_.size() == sz && std::memcmp(data_.data(), pending_.data(), sz) == 0) {
            return false;
        }
        std::swap(data_, pending_);
        dirty_ = true;
        return true;
    }

    bool write_diff(const void* bytes, size_t size) override
    {
        // Fast no-change path: memcmp directly against the committed
        // bytes. Avoids the pending+memcpy+memcmp cost that the
        // callback `write` incurs unconditionally.
        if (data_.size() == size && (size == 0 || std::memcmp(data_.data(), bytes, size) == 0)) {
            return false;
        }
        data_.resize(size);
        if (size) {
            std::memcpy(data_.data(), bytes, size);
        }
        dirty_ = true;
        return true;
    }

protected:
    ::velk::vector<uint8_t>& mutable_data() { return data_; }
    ::velk::vector<uint8_t>& pending_data() { return pending_; }

    /// Marks the buffer dirty so the next upload sweep re-uploads
    /// `data_`. Counterpart to the public `clear_dirty`. Used by
    /// derived classes that mutate `data_` through `mutable_data()`
    /// directly (e.g. `MeshBuffer::set_data`).
    void set_dirty() { dirty_ = true; }

private:
    /// GPU-side storage. Null until attached via `attach_gpu_buffer`.
    IGpuBuffer::Ptr gpu_buffer_;

    /// Committed bytes published via `get_data`.
    ::velk::vector<uint8_t> data_;

    /// Reused fill scratch for the callback `write`. Holds garbage
    /// between calls — specifically:
    ///   - After a `write` that returned true, `pending_` holds the
    ///     PREVIOUS committed bytes (now displaced by the swap).
    ///   - After a `write` that returned false, `pending_` holds the
    ///     bytes the callback wrote (which matched data_).
    ///   - After `write_diff`, `pending_` is unchanged from whatever
    ///     it held before.
    /// In all cases the next `write` begins with `pending_.assign(sz, 0)`
    /// which both resizes (with one realloc on growth past capacity)
    /// and zeroes, so the prior content is overwritten before the
    /// fill callback runs. Capacity is retained across calls to
    /// avoid per-frame allocation traffic; on dramatic shrink, callers
    /// who care can call `pending_data().shrink_to_fit()`.
    ::velk::vector<uint8_t> pending_;

    bool dirty_ = false;
};

} // namespace velk::ext

#endif // VELK_RENDER_EXT_GPU_BUFFER_H
