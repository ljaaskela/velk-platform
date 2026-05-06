#include "default_batch.h"

#include <velk/api/velk.h>

#include <velk-render/ext/gpu_buffer.h>
#include <velk-render/interface/intf_gpu_resource.h>

#include <cstring>

namespace velk::impl {

void DefaultBatch::finalize_storage(uint32_t prim_count, bool indexed)
{
    if (!storage_) {
        storage_ = ::velk::instance().create<IBuffer>(ClassId::GpuBuffer);
    }
    // `storage_` is always a GpuBuffer (created via ClassId::GpuBuffer
    // above); the cast is a hot-path internal-only access for
    // `mutable_data()`.
    auto* gpu = static_cast<GpuBuffer*>(storage_.get());
    auto& blob = gpu->mutable_data();
    blob.resize(Layout::kInstanceOffset + instance_data_.size());

    if (indexed) {
        uint32_t args[5] = { prim_count, instance_count_, 0u, 0u, 0u };
        std::memcpy(blob.data() + Layout::kArgsOffset, args, sizeof(args));
    } else {
        uint32_t args[4] = { prim_count, instance_count_, 0u, 0u };
        std::memcpy(blob.data() + Layout::kArgsOffset, args, sizeof(args));
    }
    uint32_t count = 1;
    std::memcpy(blob.data() + Layout::kCountOffset, &count, sizeof(count));
    if (!instance_data_.empty()) {
        std::memcpy(blob.data() + Layout::kInstanceOffset,
                    instance_data_.data(), instance_data_.size());
    }
    // mutable_data() already set the dirty flag. Reset cached mapped
    // pointer — a size change forces ensure_buffer_storage to
    // reallocate, invalidating the prior map.
    storage_mapped_ = nullptr;
}

void DefaultBatch::update_instance_at(uint32_t instance_index,
                                      array_view<const uint8_t> bytes)
{
    if (instance_stride_ == 0) return;
    if (instance_index >= instance_count_) return;
    size_t offset = static_cast<size_t>(instance_index) * instance_stride_;
    if (offset + bytes.size() > instance_data_.size()) return;
    std::memcpy(instance_data_.data() + offset, bytes.begin(), bytes.size());
    if (storage_mapped_) {
        std::memcpy(storage_mapped_ + Layout::kInstanceOffset + offset,
                    bytes.begin(), bytes.size());
    }
}

uint64_t DefaultBatch::storage_gpu_address() const
{
    return storage_ ? storage_->get_gpu_handle(GpuResourceKey::Default) : 0;
}

} // namespace velk::impl
