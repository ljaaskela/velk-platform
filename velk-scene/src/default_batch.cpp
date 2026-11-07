#include "default_batch.h"

#include <velk/api/velk.h>

#include <velk-render/interface/intf_gpu_buffer.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_backend.h>
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
    // Instance bytes were just (re)written; the upload sweep re-uploads the
    // arena region.
    instances_dirty_ = true;

    notify_render_state_changed(::velk::RenderStateChange::All);
}

namespace {

/// Words per mesh in the table at the start of a culling region. See instance_cull_compute_src.
constexpr uint32_t kCullTableWords = 12;
constexpr uint32_t kCullCountWord = 4;
constexpr uint32_t kCullListWord = 5;
constexpr uint32_t kCullCapacityWord = 6;
constexpr uint32_t kCullAreaWord = 7;

} // namespace

void DefaultBatch::ensure_instance_cull(IGpuResourceManager& resources)
{
    if (!instance_cull_ || cull_owner_ || cull_region_.valid() || !instance_source_) {
        return;
    }
    auto arena = resources.shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) {
        return;
    }
    const uint32_t mesh_count = instance_source_->get_mesh_count();
    uint64_t words = uint64_t(mesh_count) * kCullTableWords;
    for (uint32_t m = 0; m < mesh_count; ++m) {
        words += instance_source_->get_mesh_instance_count(m);
    }
    cull_region_ = arena->alloc(words * sizeof(uint32_t));
    if (!cull_region_.valid()) {
        VELK_LOG(E, "DefaultBatch: no room to cull %u instances", instance_count_);
        return;
    }

    const auto table_word = static_cast<uint32_t>(cull_region_.offset() / sizeof(uint32_t));
    vector<uint32_t> table;
    table.resize(size_t(mesh_count) * kCullTableWords, 0u);
    cull_list_words_.clear();
    uint32_t next_list = table_word + mesh_count * kCullTableWords;
    float far_weight = 0.f;
    float far_sum[3] = {0.f, 0.f, 0.f};
    std::fill(std::begin(cull_far_facing_), std::end(cull_far_facing_), 0.f);
    for (uint32_t m = 0; m < mesh_count; ++m) {
        uint32_t* entry = table.data() + size_t(m) * kCullTableWords;
        float sphere[4];
        instance_source_->get_cull_sphere(m, sphere);
        std::memcpy(entry, sphere, sizeof(sphere));
        const uint32_t capacity = instance_source_->get_mesh_instance_count(m);
        entry[kCullListWord] = next_list;
        entry[kCullCapacityWord] = capacity;
        cull_list_words_.push_back(next_list);
        next_list += capacity;

        float area = 0.f;
        float albedo[3];
        float facing[kSurfaceDirectionCount];
        instance_source_->get_far_look(m, area, albedo, facing);
        std::memcpy(entry + kCullAreaWord, &area, sizeof(area));
        const float weight = area * static_cast<float>(capacity);
        far_weight += weight;
        for (int k = 0; k < 3; ++k) {
            far_sum[k] += albedo[k] * weight;
        }
        for (int d = 0; d < kSurfaceDirectionCount; ++d) {
            cull_far_facing_[d] += facing[d] * static_cast<float>(capacity);
        }
    }
    for (int k = 0; k < 3; ++k) {
        cull_far_albedo_[k] = far_weight > 0.f ? far_sum[k] / far_weight : 1.f;
    }
    arena->write_at(cull_region_.offset(), table.data(), table.size() * sizeof(uint32_t));
}

bool DefaultBatch::get_instance_cull(InstanceCullInfo& out) const
{
    if (!instance_cull_ || !instance_source_) {
        return false;
    }
    const auto* owner = cull_owner_ ? static_cast<const DefaultBatch*>(cull_owner_.get()) : this;
    if (!owner->cull_region_.valid() || instance_stride_ == 0) {
        return false;
    }
    const auto table_word = static_cast<uint32_t>(owner->cull_region_.offset() / sizeof(uint32_t));
    out.key = owner;
    out.instances_base = static_cast<uint32_t>(instance_region_offset() / instance_stride_);
    out.slot_count = instance_count_;
    out.table_word = table_word;
    out.visible_word = owner->cull_list_words_[cull_mesh_];
    out.count_word = table_word + cull_mesh_ * kCullTableWords + kCullCountWord;
    out.max_distance = instance_source_->get_draw_distance();
    out.far_form = instance_source_->get_far_form();
    for (int k = 0; k < 3; ++k) {
        out.far_albedo[k] = owner->cull_far_albedo_[k];
    }
    for (int d = 0; d < kSurfaceDirectionCount; ++d) {
        out.far_facing[d] = owner->cull_far_facing_[d];
    }
    instance_source_->get_partition_cull(out.partition_table_word, out.partition_count, out.chunk_list_word);
    return true;
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

} // namespace velk::impl
