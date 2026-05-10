#ifndef VELK_RENDER_EXT_PERSISTENT_BUFFER_H
#define VELK_RENDER_EXT_PERSISTENT_BUFFER_H

#include <velk/api/velk.h>

#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/plugin.h>
#include <velk-render/render_path/frame_context.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace velk {

/**
 * @brief Helper that owns a per-owner persistent IBuffer and stages
 *        bytes into it via the standard write_diff + ensure_buffer_storage
 *        pipeline.
 *
 * Typical use is one `PersistentBuffer` member per logical CPU array
 * the owner wants to mirror to the GPU with a stable device address
 * (lights, RT shapes, BVH nodes, env material data, etc.). The first
 * `upload()` lazily creates the backing IBuffer (an `impl::GpuBuffer`)
 * via the registered `ClassId::GpuBuffer`. Subsequent calls write_diff
 * the new bytes against the previous frame's; on real change the
 * resource manager's `ensure_buffer_storage` allocates / resizes the
 * backend buffer, the bytes are mapped + memcpy'd, and the dirty flag
 * is cleared. The returned GPU address stays stable across frames
 * unless the buffer reallocates (size change).
 *
 * Drop the owner / its `PersistentBuffer` member to release: the
 * IBuffer::Ptr drops, and the resource manager observer cascade
 * defers backend destruction.
 */
class PersistentBuffer
{
public:
    struct Result
    {
        uint64_t address = 0;  ///< Stable GPU device address, or 0 on size==0 / failure.
        bool changed = false;  ///< True when bytes actually differed from the previous upload.
    };

    /// Lazily creates the buffer on first call, write_diffs @p bytes
    /// into it, uploads if changed, and returns the stable GPU device
    /// address paired with a "changed since last call" bool. Callers
    /// gate notify-side-effects on `changed` so observers fire only on
    /// real content change.
    Result upload(const void* bytes, size_t size, FrameContext& ctx)
    {
        if (size == 0 || !ctx.resources || !ctx.backend) return {};
        if (!buf_) {
            buf_ = ::velk::instance().create<IBuffer>(ClassId::GpuBuffer);
            if (!buf_) return {};
        }
        auto* buf = buf_.get();
        bool changed = buf->write_diff(bytes, size);
        if (buf->is_dirty()) {
            GpuBufferDesc desc{};
            desc.size = size;
            desc.cpu_writable = true;
            if (auto* be = ctx.resources->ensure_buffer_storage(buf, desc)) {
                if (auto gb = be->buffer.lock()) {
                    if (auto* dst = gb->map()) {
                        std::memcpy(dst, buf->get_data(), size);
                    }
                }
            }
            buf->clear_dirty();
        }
        return {get_gpu_address(buf), changed};
    }

    /// Underlying IBuffer (for consumers that need the IBuffer::Ptr,
    /// e.g. IBvh::get_nodes_buffer overrides). Null until first upload.
    IBuffer::Ptr buffer() const { return buf_; }

private:
    IBuffer::Ptr buf_;
};

} // namespace velk

#endif // VELK_RENDER_EXT_PERSISTENT_BUFFER_H
