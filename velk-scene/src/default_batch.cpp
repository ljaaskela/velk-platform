#include "default_batch.h"

#include <velk/api/velk.h>

#include <velk-render/interface/intf_gpu_buffer.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/plugin.h>

#include <cstring>

namespace velk::impl {

void DefaultBatch::finalize_storage(uint32_t prim_count, bool indexed)
{
    if (!storage_) {
        storage_ = ::velk::instance().create<IBuffer>(ClassId::GpuBuffer);
        if (!storage_) return;
    }

    // Fill the fixed 112-byte blob via IBuffer::write — the impl supplies
    // its own scratch, the lambda writes args/count, and the impl
    // diff+dirty-tracks against the previous frame's blob. The header +
    // material_ptr are written later by emit; instance bytes live in the
    // shared instance arena, not here.
    size_t blob_size = Layout::kBufferSize;
    storage_->write(blob_size, [&](void* dst, size_t /*n*/) {
        auto* bytes = static_cast<uint8_t*>(dst);
        if (indexed) {
            uint32_t args[5] = { prim_count, instance_count_, 0u, 0u, 0u };
            std::memcpy(bytes + Layout::kArgsOffset, args, sizeof(args));
        } else {
            uint32_t args[4] = { prim_count, instance_count_, 0u, 0u };
            std::memcpy(bytes + Layout::kArgsOffset, args, sizeof(args));
        }
        uint32_t count = 1;
        std::memcpy(bytes + Layout::kCountOffset, &count, sizeof(count));
    });
    // Reset cached mapped pointer — a size change forces
    // ensure_buffer_storage to reallocate, invalidating the prior map.
    storage_mapped_ = nullptr;
    // Instance bytes were just (re)written; the upload sweep re-uploads the
    // arena region.
    instances_dirty_ = true;

    notify_render_state_changed(::velk::RenderStateChange::All);
}

void DefaultBatch::update_instance_at(uint32_t instance_index,
                                      array_view<const uint8_t> bytes)
{
    if (instance_stride_ == 0) return;
    if (instance_index >= instance_count_) return;
    size_t offset = static_cast<size_t>(instance_index) * instance_stride_;
    if (offset + bytes.size() > instance_data_.size()) return;
    std::memcpy(instance_data_.data() + offset, bytes.begin(), bytes.size());
    // Mark dirty so the upload sweep re-uploads the region this frame.
    instances_dirty_ = true;
}

uint64_t DefaultBatch::storage_gpu_address() const
{
    return get_gpu_address(storage_);
}

} // namespace velk::impl
