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
constexpr uint32_t kCullFirstEntryWord = 8;
constexpr uint32_t kCullEntryCountWord = 9;

/// Words in an entry of a primitive drawn by cluster. See instance_cluster_runs_src.
constexpr uint32_t kClusterEntryWords = 10;
constexpr uint32_t kClusterEntryListWord = 0;
constexpr uint32_t kClusterEntryCapacityWord = 1;
constexpr uint32_t kClusterEntryTableWord = 2;
constexpr uint32_t kClusterEntryClustersWord = 3;
constexpr uint32_t kClusterEntryCountsWord = 4;
constexpr uint32_t kClusterEntryCursorsWord = 5;
constexpr uint32_t kClusterEntryCommandsWord = 6;
constexpr uint32_t kClusterEntryDrawsWord = 7;
constexpr uint32_t kClusterEntryLevelsWord = 8;
constexpr uint32_t kClusterEntryLevelCountWord = 9;
/// Words per cluster: its count, its cursor and its command.
constexpr uint32_t kClusterWords = 7;
/// Most instances of clusters in one list. At 256 triangles each this is about a billion
/// triangles, more than a frame could draw.
constexpr uint64_t kMaxClusterRuns = 1ull << 22;

} // namespace

const DefaultBatch::ClusterDraw* DefaultBatch::find_cluster_draw(uint32_t mesh, const IMeshPrimitive* primitive) const
{
    for (const auto& draw : cull_cluster_draws_) {
        if (draw.mesh == mesh && draw.primitive == primitive) {
            return &draw;
        }
    }
    return nullptr;
}

void DefaultBatch::ensure_instance_cull(IGpuResourceManager& resources)
{
    if (!instance_cull_ || cull_owner_ || cull_region_.valid()) {
        return;
    }
    auto arena = resources.shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) {
        return;
    }
    // Without a source, this batch is one mesh of its own primitive.
    const uint32_t mesh_count = instance_source_ ? instance_source_->get_mesh_count() : 1u;
    uint64_t words = uint64_t(mesh_count) * kCullTableWords;

    // Primitives of several clusters are drawn by cluster, the rest from the mesh's list.
    struct PendingDraw
    {
        uint32_t mesh;
        const IMeshPrimitive* primitive;
        MeshLodInfo lod;
        uint32_t capacity;
    };
    vector<PendingDraw> pending;
    for (uint32_t m = 0; m < mesh_count; ++m) {
        // Without a source, visible instances go to the shared list of the batches culled together.
        const uint32_t instances = instance_source_ ? instance_source_->get_mesh_instance_count(m) : instance_count_;
        words += instance_source_ ? instances : 0u;
        const auto add_primitive = [&](const IMeshPrimitive::Ptr& primitive) {
            auto* internal = interface_cast<IMeshPrimitiveInternal>(primitive.get());
            MeshLodInfo lod;
            if (internal && internal->ensure_lod_data(resources, lod)) {
                // A coarser choice uses fewer clusters than the full mesh.
                const auto capacity = static_cast<uint32_t>(std::min(uint64_t(instances) * lod.level0_clusters, kMaxClusterRuns));
                pending.push_back({m, primitive.get(), lod, capacity});
                words += kClusterEntryWords + uint64_t(lod.cluster_count) * kClusterWords + uint64_t(capacity) * 2u;
            }
        };
        if (!instance_source_) {
            if (primitive_) {
                add_primitive(primitive_);
            }
        } else if (auto mesh = instance_source_->get_mesh(m)) {
            for (const auto& primitive : mesh->get_primitives()) {
                add_primitive(primitive);
            }
        }
    }

    // Nothing to choose between, so it draws all its instances as a plain indexed draw.
    if (!instance_source_ && pending.empty()) {
        instance_cull_ = false;
        return;
    }

    cull_chosen_capacity_ = 0;
    if (instance_source_) {
        for (const PendingDraw& draw : pending) {
            cull_chosen_capacity_ += draw.capacity;
        }
        if (cull_chosen_capacity_ != 0) {
            words += kCullChosenHeaderWords + uint64_t(cull_chosen_capacity_) * kCullChosenRecordWords;
        }
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
        if (instance_source_) {
            instance_source_->get_cull_sphere(m, sphere);
        } else {
            const aabb local = primitive_->get_bounds();
            const vec3 half = (local.max() - local.min()) * 0.5f;
            sphere[0] = local.min().x + half.x;
            sphere[1] = local.min().y + half.y;
            sphere[2] = local.min().z + half.z;
            sphere[3] = vec3::length(half);
        }
        std::memcpy(entry, sphere, sizeof(sphere));
        const uint32_t capacity = instance_source_ ? instance_source_->get_mesh_instance_count(m) : instance_count_;
        cull_list_words_.push_back(next_list);
        if (!instance_source_) {
            continue;
        }
        entry[kCullListWord] = next_list;
        entry[kCullCapacityWord] = capacity;
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

    // Each mesh's entries in a row, then each entry's counts, cursors, commands and list.
    cull_cluster_draws_.clear();
    vector<uint32_t> entries;
    entries.resize(pending.size() * kClusterEntryWords, 0u);
    const uint32_t entries_word = next_list;
    next_list += static_cast<uint32_t>(entries.size());
    for (size_t i = 0; i < pending.size(); ++i) {
        const PendingDraw& draw = pending[i];
        const uint32_t entry_word = entries_word + static_cast<uint32_t>(i) * kClusterEntryWords;
        uint32_t* table_entry = table.data() + size_t(draw.mesh) * kCullTableWords;
        if (table_entry[kCullEntryCountWord] == 0u) {
            table_entry[kCullFirstEntryWord] = entry_word;
        }
        ++table_entry[kCullEntryCountWord];

        const uint32_t clusters = draw.lod.cluster_count;
        const uint32_t counts_word = next_list;
        const uint32_t cursors_word = counts_word + clusters;
        const uint32_t commands_word = cursors_word + clusters;
        const uint32_t list_word = commands_word + clusters * 5u;
        const uint32_t errors_word = list_word + draw.capacity;
        next_list = errors_word + draw.capacity;

        vector<uint32_t> zeros;
        zeros.resize(clusters, 0u);
        arena->write_at(uint64_t(counts_word) * sizeof(uint32_t), zeros.data(), zeros.size() * sizeof(uint32_t));

        uint32_t* entry = entries.data() + i * kClusterEntryWords;
        entry[kClusterEntryListWord] = list_word;
        entry[kClusterEntryCapacityWord] = draw.capacity;
        entry[kClusterEntryTableWord] = draw.lod.cluster_word;
        entry[kClusterEntryClustersWord] = clusters;
        entry[kClusterEntryCountsWord] = counts_word;
        entry[kClusterEntryCursorsWord] = cursors_word;
        entry[kClusterEntryCommandsWord] = commands_word;
        entry[kClusterEntryLevelsWord] = draw.lod.level_word;
        entry[kClusterEntryLevelCountWord] = draw.lod.level_count;
        cull_cluster_draws_.push_back({draw.mesh, draw.primitive, clusters, entry_word, list_word, commands_word, draw.lod.index_word, errors_word, draw.capacity});
    }
    // Batches culled together keep their records in one place, with the culling pass.
    cull_chosen_word_ = next_list;
    arena->write_at(uint64_t(entries_word) * sizeof(uint32_t), entries.data(), entries.size() * sizeof(uint32_t));
    arena->write_at(cull_region_.offset(), table.data(), table.size() * sizeof(uint32_t));
}

bool DefaultBatch::get_instance_cull(InstanceCullInfo& out) const
{
    if (!instance_cull_) {
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
    if (const ClusterDraw* draw = owner->find_cluster_draw(cull_mesh_, primitive_.get())) {
        out.visible_word = draw->list_word;
        out.cluster_count = draw->clusters;
        out.cluster_entry_word = draw->entry_word;
        out.cluster_commands_word = draw->commands_word;
        out.cluster_draws_word = draw->entry_word + kClusterEntryDrawsWord;
        out.cluster_index_word = draw->index_word;
        out.cluster_errors_word = draw->errors_word;
        out.cluster_capacity = draw->capacity;
    }
    out.chosen_word = owner->cull_chosen_word_;
    out.chosen_capacity = owner->cull_chosen_capacity_;
    if (!instance_source_) {
        out.cull_together = true;
        return true;
    }
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
