#include "scene_bvh.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-scene/interface/intf_element.h>
#include <velk-scene/interface/intf_visual.h>

#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/plugin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace velk::impl {

namespace {

/// Records in a static partition when it is cut. A partition rebuild is bounded by
/// twice this.
constexpr uint32_t kPartitionRecords = 1024;
/// Frames a record of the dynamic partition stays still before it returns to a
/// static partition.
constexpr uint32_t kQuietFrames = 120;
/// Records with more shapes than this get a local tree instead of a linear test.
constexpr size_t kMaxShapesWithoutTree = 4;
/// A full re-cut runs when the top tree's SAH cost grows past this factor.
constexpr float kRecutDrift = 1.5f;
/// New records up to this count always start in the dynamic partition.
constexpr size_t kMaxUnplacedToDynamic = 16;

float surface_area(const float* lo, const float* hi)
{
    const float x = std::max(0.f, hi[0] - lo[0]);
    const float y = std::max(0.f, hi[1] - lo[1]);
    const float z = std::max(0.f, hi[2] - lo[2]);
    return 2.f * (x * y + y * z + z * x);
}

void merge_bounds(float* lo, float* hi, const float* other_lo, const float* other_hi)
{
    for (int k = 0; k < 3; ++k) {
        lo[k] = std::min(lo[k], other_lo[k]);
        hi[k] = std::max(hi[k], other_hi[k]);
    }
}

/// Allocates @p count elements of @p stride in @p arena into @p region, releasing what
/// it held, and returns the element base through @p base. Allocating instead of
/// overwriting is what keeps a frame still reading the old contents safe: the old
/// region is freed past that frame's fence.
bool alloc_elements(IGpuArena* arena, ArenaRegion& region, size_t count, size_t stride, uint32_t& base)
{
    region = {};
    base = 0;
    if (!arena || count == 0) {
        return false;
    }
    region = arena->alloc(count * stride);
    if (!region.valid()) {
        return false;
    }
    base = static_cast<uint32_t>(region.offset() / stride);
    return true;
}

/// Whether two shapes describe the same geometry, ignoring color and material.
bool same_geometry(const RtShape& a, const RtShape& b)
{
    // A mesh shape's other geometry fields are only its bounds, which a deforming
    // mesh changes without becoming other geometry.
    if (a.shape_kind == kRtShapeKindMesh && b.shape_kind == kRtShapeKindMesh) {
        return a.mesh_static_base == b.mesh_static_base;
    }
    return std::memcmp(a.origin, b.origin, sizeof(a.origin)) == 0
        && std::memcmp(a.u_axis, b.u_axis, sizeof(a.u_axis)) == 0
        && std::memcmp(a.v_axis, b.v_axis, sizeof(a.v_axis)) == 0
        && std::memcmp(a.w_axis, b.w_axis, sizeof(a.w_axis)) == 0
        && std::memcmp(a.params, b.params, sizeof(a.params)) == 0
        && a.shape_kind == b.shape_kind
        && a.mesh_static_base == b.mesh_static_base;
}

/// Shapes emitted for one record, before they are compared with what the slot holds.
struct EmittedRecord
{
    RtRecord gpu{};
    vector<RtShape> shapes;
    vector<BlasNode> tree;
    BvhItemBounds bounds{};
};

void set_inverted_bounds(GpuBvhNode& node)
{
    node = {};
    node.aabb_min[0] = 1.f;
    node.aabb_max[0] = -1.f;
}

} // namespace

void SceneBvh::update_probe_volume(const float bounds_min[3], const float bounds_max[3], uint32_t frame)
{
    // Growing past the volume adds this fraction of its size as margin, so an
    // object moving outward does not reset the probes every frame.
    constexpr float kGrowMargin = 0.125f;
    // Bounds shrinking below this fraction of the volume on any axis re-fit it,
    // since a loose volume spreads the probes thinly.
    constexpr float kRefitBelow = 0.5f;

    float vol_min[3] = {probe_volume_.min.x, probe_volume_.min.y, probe_volume_.min.z};
    float vol_max[3] = {probe_volume_.max.x, probe_volume_.max.y, probe_volume_.max.z};

    bool refit = !probe_volume_placed_;
    bool escaped = false;
    for (int i = 0; i < 3; ++i) {
        escaped = escaped || bounds_min[i] < vol_min[i] || bounds_max[i] > vol_max[i];
        refit = refit || (bounds_max[i] - bounds_min[i]) < kRefitBelow * (vol_max[i] - vol_min[i]);
    }
    if (!refit && !escaped) return;

    for (int i = 0; i < 3; ++i) {
        if (refit) {
            vol_min[i] = bounds_min[i];
            vol_max[i] = bounds_max[i];
        } else {
            const float margin = kGrowMargin * (bounds_max[i] - bounds_min[i]);
            if (bounds_min[i] < vol_min[i]) vol_min[i] = bounds_min[i] - margin;
            if (bounds_max[i] > vol_max[i]) vol_max[i] = bounds_max[i] + margin;
        }
    }
    probe_volume_.min = {vol_min[0], vol_min[1], vol_min[2]};
    probe_volume_.max = {vol_max[0], vol_max[1], vol_max[2]};
    probe_volume_.reset_frame = frame;
    probe_volume_placed_ = true;
}

BvhItemBounds SceneBvh::instance_bounds(const InstanceGroup& group, uint32_t slot) const
{
    const InstanceTransform& t = group.visual->get_transforms()[slot];
    const mat4& w = group.world;
    // The instance's matrix applied to the mesh bounds' center and half extents.
    float center[3];
    float half[3];
    for (int k = 0; k < 3; ++k) {
        center[k] = 0.5f * (group.local_bounds.min[k] + group.local_bounds.max[k]);
        half[k] = 0.5f * (group.local_bounds.max[k] - group.local_bounds.min[k]);
    }
    float instance_center[3];
    float instance_half[3];
    for (int row = 0; row < 3; ++row) {
        instance_center[row] = t.rows[row][3];
        instance_half[row] = 0.f;
        for (int col = 0; col < 3; ++col) {
            instance_center[row] += t.rows[row][col] * center[col];
            instance_half[row] += std::abs(t.rows[row][col]) * half[col];
        }
    }
    // And then the element's.
    BvhItemBounds out{};
    for (int row = 0; row < 3; ++row) {
        float c = w(row, 3);
        float h = 0.f;
        for (int col = 0; col < 3; ++col) {
            c += w(row, col) * instance_center[col];
            h += std::abs(w(row, col)) * instance_half[col];
        }
        out.min[row] = c - h;
        out.max[row] = c + h;
    }
    return out;
}

RtRecord SceneBvh::instance_record(const InstanceGroup& group, uint32_t slot) const
{
    RtRecord record = make_rt_record(group.world * group.visual->get_transforms()[slot].to_mat4());
    record.first_shape = group.first_shape;
    record.shape_count = static_cast<uint32_t>(group.shapes.size());
    return record;
}

void SceneBvh::free_instance_partitions(InstanceGroup& group)
{
    for (uint32_t p : group.partitions) {
        partitions_[p] = Partition{};
        free_partitions_.push_back(p);
    }
    group.partitions.clear();
    group.open_partition = kUnplaced;
    group.dynamic_partition = kUnplaced;
    group.movers.clear();
    top_dirty_ = true;
    root_dirty_ = true;
}

bool SceneBvh::is_instance_dynamic(uint32_t partition) const
{
    const Partition& p = partitions_[partition];
    return p.group && p.group->dynamic_partition == partition;
}

void SceneBvh::promote_instance(InstanceGroup& group, uint32_t slot)
{
    if (group.dynamic_partition == kUnplaced) {
        group.dynamic_partition = alloc_partition();
        partitions_[group.dynamic_partition].group = &group;
        group.partitions.push_back(group.dynamic_partition);
    }
    const uint32_t home = group.slot_partition[slot];
    auto& members = partitions_[home].members;
    members.erase(std::remove(members.begin(), members.end(), slot), members.end());
    partitions_[home].dirty = true;

    partitions_[group.dynamic_partition].members.push_back(slot);
    partitions_[group.dynamic_partition].dirty = true;
    group.slot_partition[slot] = group.dynamic_partition;
    group.movers[slot] = {home, update_frame_};
}

void SceneBvh::demote_quiet_instances(InstanceGroup& group)
{
    if (group.dynamic_partition == kUnplaced) {
        return;
    }
    for (auto it = group.movers.begin(); it != group.movers.end();) {
        if (update_frame_ - it->second.moved_frame < kQuietFrames) {
            ++it;
            continue;
        }
        const uint32_t slot = it->first;
        uint32_t home = it->second.home;
        // Its partition may have emptied and gone since.
        if (!partitions_[home].active || partitions_[home].group != &group || home == group.dynamic_partition) {
            if (group.open_partition == kUnplaced) {
                group.open_partition = alloc_partition();
                partitions_[group.open_partition].group = &group;
                group.partitions.push_back(group.open_partition);
            }
            home = group.open_partition;
        }
        auto& members = partitions_[group.dynamic_partition].members;
        members.erase(std::remove(members.begin(), members.end(), slot), members.end());
        partitions_[group.dynamic_partition].dirty = true;
        partitions_[home].members.push_back(slot);
        partitions_[home].dirty = true;
        group.slot_partition[slot] = home;
        it = group.movers.erase(it);
    }
}

void SceneBvh::cut_instances(InstanceGroup& group, FrameContext& ctx)
{
    VELK_PERF_SCOPE("renderer.instance_cut");
    free_instance_partitions(group);

    const auto slot_count = static_cast<uint32_t>(group.visual->get_transforms().size());
    group.slot_partition.clear();
    group.slot_partition.resize(slot_count, kUnplaced);

    struct Keyed
    {
        uint64_t key;
        uint32_t slot;
    };
    vector<Keyed> keyed;
    keyed.reserve(slot_count);
    float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    float hi[3] = {-lo[0], -lo[0], -lo[0]};
    vector<float> centers;
    centers.reserve(size_t(slot_count) * 3);
    for (uint32_t slot = 0; slot < slot_count; ++slot) {
        if (!group.visual->is_live(slot)) {
            continue;
        }
        const BvhItemBounds bounds = instance_bounds(group, slot);
        for (int k = 0; k < 3; ++k) {
            const float c = 0.5f * (bounds.min[k] + bounds.max[k]);
            lo[k] = std::min(lo[k], c);
            hi[k] = std::max(hi[k], c);
            centers.push_back(c);
        }
        keyed.push_back({0u, slot});
    }
    if (keyed.empty()) {
        return;
    }

    // One grid scale on all axes, so a flat forest still gets cells that are square.
    const float extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-20f});
    const float scale = 2097151.f / extent;
    const auto spread = [](uint64_t v) {
        v &= 0x1FFFFFu;
        v = (v | v << 32) & 0x1F00000000FFFFull;
        v = (v | v << 16) & 0x1F0000FF0000FFull;
        v = (v | v << 8) & 0x100F00F00F00F00Full;
        v = (v | v << 4) & 0x10C30C30C30C30C3ull;
        return (v | v << 2) & 0x1249249249249249ull;
    };
    for (size_t i = 0; i < keyed.size(); ++i) {
        const float* c = centers.data() + i * 3;
        keyed[i].key = spread(static_cast<uint64_t>((c[0] - lo[0]) * scale))
            | spread(static_cast<uint64_t>((c[1] - lo[1]) * scale)) << 1
            | spread(static_cast<uint64_t>((c[2] - lo[2]) * scale)) << 2;
    }
    std::sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) { return a.key < b.key; });

    // Reserving up front avoids growing these arenas part by part. The margin is for
    // the other partitions and the top tree.
    const size_t reserve_count = keyed.size() + keyed.size() / kPartitionRecords + kPartitionRecords;
    if (ctx.records_arena) {
        ctx.records_arena->reserve(reserve_count * sizeof(RtRecord));
    }
    if (ctx.bvh_nodes_arena) {
        ctx.bvh_nodes_arena->reserve(reserve_count * 2u * sizeof(GpuBvhNode));
    }

    for (size_t begin = 0; begin < keyed.size(); begin += kPartitionRecords) {
        const size_t end = std::min(keyed.size(), begin + size_t(kPartitionRecords));
        const uint32_t index = alloc_partition();
        Partition& partition = partitions_[index];
        partition.group = &group;
        partition.members.reserve(end - begin);
        for (size_t i = begin; i < end; ++i) {
            partition.members.push_back(keyed[i].slot);
            group.slot_partition[keyed[i].slot] = index;
        }
        group.partitions.push_back(index);
        group.open_partition = partition.members.size() < kPartitionRecords ? index : kUnplaced;
    }
}

void SceneBvh::update_instances(IElement* element, IInstancedMeshInternal& visual, const EmitContext& emit)
{
    auto visual_state = read_state<IVisual3D>(&visual);
    auto mesh = visual_state ? visual_state->mesh.get<IMesh>() : nullptr;
    auto element_state = read_state<IElement>(element);
    if (!mesh || !element_state) {
        return;
    }

    // One run of shapes for all instances, local to each of them.
    struct CollectShapes
    {
        const EmitContext& emit;
        vector<RtShape> shapes;
    };
    CollectShapes collect{emit, {}};
    const mat4 identity = mat4::identity();
    emit_mesh_shapes(interface_cast<IVisual>(&visual), *mesh, identity, ::velk::color::white(), nullptr,
                     +[](void* u, ShapeSite& site) {
                         auto& c = *static_cast<CollectShapes*>(u);
                         c.emit.cb(c.emit.user, site);
                         c.shapes.push_back(site.geometry);
                     }, &collect);
    if (collect.shapes.empty()) {
        if (std::find(unresolved_.begin(), unresolved_.end(), element) == unresolved_.end()) {
            unresolved_.push_back(element);
        }
        return;
    }

    auto& group = instance_groups_[element];
    const bool new_group = group.visual == nullptr;
    group.visual = &visual;

    bool rebuild = new_group;
    const size_t shape_bytes = collect.shapes.size() * sizeof(RtShape);
    if (group.shapes.size() != collect.shapes.size()) {
        alloc_elements(emit.frame.bvh_shapes_arena, group.shape_region, collect.shapes.size(), sizeof(RtShape),
                       group.first_shape);
        if (!group.shape_region.valid()) {
            VELK_LOG(E, "SceneBvh: instance shape allocation failed");
            return;
        }
        emit.frame.bvh_shapes_arena->write_at(group.shape_region.offset(), collect.shapes.data(), shape_bytes);
        group.shapes = std::move(collect.shapes);
        rebuild = true;
    } else if (std::memcmp(group.shapes.data(), collect.shapes.data(), shape_bytes) != 0) {
        emit.frame.bvh_shapes_arena->update_at(group.shape_region.offset(), collect.shapes.data(), shape_bytes);
        group.shapes = std::move(collect.shapes);
    }

    const aabb local = mesh->get_bounds();
    const vec3 local_min = local.min();
    const vec3 local_max = local.max();
    const BvhItemBounds new_bounds{{local_min.x, local_min.y, local_min.z}, {local_max.x, local_max.y, local_max.z}};
    if (std::memcmp(&group.local_bounds, &new_bounds, sizeof(new_bounds)) != 0) {
        group.local_bounds = new_bounds;
        rebuild = true;
    }

    vector<uint32_t> changed;
    bool all_changed = false;
    visual.take_changed_slots(changed, all_changed);
    rebuild = rebuild || all_changed;

    // A translated element keeps every record relative to its partition's origin, so
    // only the origins move. Anything else changes the records.
    const mat4 old_world = group.world;
    const mat4& new_world = element_state->world_matrix;
    group.world = new_world;
    if (!new_group && std::memcmp(old_world.m, new_world.m, sizeof(new_world.m)) != 0) {
        bool translated_only = true;
        for (int row = 0; row < 3 && translated_only; ++row) {
            for (int col = 0; col < 3; ++col) {
                translated_only = translated_only && old_world(row, col) == new_world(row, col);
            }
        }
        if (!translated_only) {
            rebuild = true;
        } else if (!rebuild) {
            float delta[3];
            for (int k = 0; k < 3; ++k) {
                delta[k] = new_world(k, 3) - old_world(k, 3);
            }
            for (uint32_t p : group.partitions) {
                GpuBvhNode& ref = partitions_[p].ref;
                float z = 0.f;
                std::memcpy(&z, &ref.first_shape, sizeof(float));
                for (int k = 0; k < 3; ++k) {
                    ref.aabb_min[k] += delta[k];
                    ref.aabb_max[k] += delta[k];
                }
                ref.aabb_min[3] += delta[0];
                ref.aabb_max[3] += delta[1];
                z += delta[2];
                std::memcpy(&ref.first_shape, &z, sizeof(float));
            }
            top_dirty_ = true;
        }
    }

    if (rebuild) {
        cut_instances(group, emit.frame);
        return;
    }

    const auto slot_count = static_cast<uint32_t>(visual.get_transforms().size());
    if (group.slot_partition.size() < slot_count) {
        group.slot_partition.resize(slot_count, kUnplaced);
    }
    for (uint32_t slot : changed) {
        const uint32_t current = group.slot_partition[slot];
        if (!visual.is_live(slot)) {
            // Removed.
            if (current != kUnplaced) {
                partitions_[current].dirty = true;
                auto& members = partitions_[current].members;
                members.erase(std::remove(members.begin(), members.end(), slot), members.end());
                group.slot_partition[slot] = kUnplaced;
                group.movers.erase(slot);
            }
        } else if (current != kUnplaced && current == group.dynamic_partition) {
            partitions_[current].dirty = true;
            group.movers[slot].moved_frame = update_frame_;
        } else if (current != kUnplaced) {
            promote_instance(group, slot);
        } else {
            // Added.
            if (group.open_partition == kUnplaced) {
                group.open_partition = alloc_partition();
                partitions_[group.open_partition].group = &group;
                group.partitions.push_back(group.open_partition);
            }
            Partition& open = partitions_[group.open_partition];
            open.members.push_back(slot);
            open.dirty = true;
            group.slot_partition[slot] = group.open_partition;
            if (open.members.size() >= kPartitionRecords) {
                group.open_partition = kUnplaced;
            }
        }
    }
}

void SceneBvh::update_element(IElement* element, const EmitContext& emit)
{
    if (auto instanced = ::velk::find_attachment<IInstancedMeshInternal>(element)) {
        update_instances(element, *instanced, emit);
        return;
    }

    struct CollectState
    {
        SceneBvh& self;
        const EmitContext& emit;
        vector<EmittedRecord> records;
        const void* open_group = nullptr;
    };
    CollectState collect{*this, emit};
    emit_unresolved_ = false;
    emit_shapes_for_element(element, emit.frame.render_ctx, +[](void* u, ShapeSite& site) {
        auto& s = *static_cast<CollectState*>(u);
        s.emit.cb(s.emit.user, site);
        if (site.draw_entry && site.draw_entry->texture_key != 0 && site.geometry.texture_id == 0) {
            s.self.emit_unresolved_ = true;
        }
        if (site.record_group && site.record_group == s.open_group) {
            auto& open = s.records.back();
            open.shapes.push_back(site.geometry);
            merge_bounds(open.bounds.min, open.bounds.max, site.aabb_min, site.aabb_max);
            return;
        }
        s.open_group = site.record_group;
        s.records.emplace_back();
        auto& fresh = s.records.back();
        fresh.gpu = make_rt_record(*site.world);
        fresh.shapes.push_back(site.geometry);
        for (int k = 0; k < 3; ++k) {
            fresh.bounds.min[k] = site.aabb_min[k];
            fresh.bounds.max[k] = site.aabb_max[k];
        }
    }, &collect);
    if (emit_unresolved_ && std::find(unresolved_.begin(), unresolved_.end(), element) == unresolved_.end()) {
        unresolved_.push_back(element);
    }

    for (auto& emitted : collect.records) {
        if (emitted.shapes.size() > kMaxShapesWithoutTree) {
            build_local_tree(emitted.shapes, emitted.tree);
        }
    }

    auto found = element_records_.find(element);
    if (found == element_records_.end() && collect.records.empty()) {
        return;
    }

    // Records stay when only their placement or materials changed.
    bool same_structure = found != element_records_.end() && found->second.size() == collect.records.size();
    for (size_t i = 0; same_structure && i < collect.records.size(); ++i) {
        const Record& record = records_[found->second[i]];
        const auto& emitted = collect.records[i];
        same_structure = record.shapes.size() == emitted.shapes.size();
        for (size_t s = 0; same_structure && s < emitted.shapes.size(); ++s) {
            same_structure = same_geometry(record.shapes[s], emitted.shapes[s]);
        }
    }

    if (same_structure) {
        for (size_t i = 0; i < collect.records.size(); ++i) {
            const uint32_t slot = found->second[i];
            Record& record = records_[slot];
            const auto& emitted = collect.records[i];
            const bool moved = std::memcmp(record.gpu.inv_row, emitted.gpu.inv_row, sizeof(record.gpu.inv_row)) != 0
                || std::memcmp(&record.bounds, &emitted.bounds, sizeof(record.bounds)) != 0;
            if (moved) {
                std::memcpy(record.gpu.inv_row, emitted.gpu.inv_row, sizeof(record.gpu.inv_row));
                record.bounds = emitted.bounds;
                record.quiet_frames = 0;
                if (record.partition == kDynamic) {
                    dynamic_transforms_dirty_ = true;
                } else if (record.partition != kUnplaced) {
                    promote(slot);
                }
            }
            const size_t shape_bytes = emitted.shapes.size() * sizeof(RtShape);
            if (std::memcmp(record.shapes.data(), emitted.shapes.data(), shape_bytes) != 0) {
                std::memcpy(record.shapes.data(), emitted.shapes.data(), shape_bytes);
                if (record.partition != kUnplaced) {
                    shape_updates_.push_back(slot);
                }
            }
        }
        return;
    }

    if (found != element_records_.end()) {
        for (uint32_t slot : found->second) {
            free_record(slot);
        }
        found->second.clear();
    }
    auto& slots = element_records_[element];
    for (auto& emitted : collect.records) {
        uint32_t slot;
        if (free_records_.empty()) {
            slot = static_cast<uint32_t>(records_.size());
            records_.emplace_back();
        } else {
            slot = free_records_.back();
            free_records_.pop_back();
        }
        Record& record = records_[slot];
        record.owner = element;
        record.gpu = emitted.gpu;
        record.gpu.shape_count = static_cast<uint32_t>(emitted.shapes.size());
        record.shapes = std::move(emitted.shapes);
        record.tree = std::move(emitted.tree);
        record.bounds = emitted.bounds;
        record.partition = kUnplaced;
        record.quiet_frames = 0;
        slots.push_back(slot);
        unplaced_.push_back(slot);
        ++live_records_;
    }
}

void SceneBvh::free_record(uint32_t slot)
{
    Record& record = records_[slot];
    if (record.partition == kDynamic) {
        dynamic_.dirty = true;
    } else if (record.partition != kUnplaced) {
        partitions_[record.partition].dirty = true;
    }
    record.owner = nullptr;
    record.partition = kUnplaced;
    record.shapes.clear();
    record.tree.clear();
    freed_records_.push_back(slot);
    --live_records_;
}

void SceneBvh::remove_element(IElement* element)
{
    auto found = element_records_.find(element);
    if (found != element_records_.end()) {
        for (uint32_t slot : found->second) {
            free_record(slot);
        }
        element_records_.erase(found);
    }
    auto group = instance_groups_.find(element);
    if (group != instance_groups_.end()) {
        free_instance_partitions(group->second);
        instance_groups_.erase(group);
    }
    unresolved_.erase(std::remove(unresolved_.begin(), unresolved_.end(), element), unresolved_.end());
}

void SceneBvh::promote(uint32_t slot)
{
    Record& record = records_[slot];
    partitions_[record.partition].dirty = true;
    record.partition = kDynamic;
    record.quiet_frames = 0;
    dynamic_.members.push_back(slot);
    dynamic_.dirty = true;
}

uint32_t SceneBvh::alloc_partition()
{
    uint32_t index;
    if (free_partitions_.empty()) {
        index = static_cast<uint32_t>(partitions_.size());
        partitions_.emplace_back();
    } else {
        index = free_partitions_.back();
        free_partitions_.pop_back();
    }
    partitions_[index].active = true;
    partitions_[index].dirty = true;
    partitions_[index].members.clear();
    return index;
}

void SceneBvh::demote_quiet_records()
{
    vector<uint32_t> quiet;
    for (uint32_t slot : dynamic_.members) {
        Record& record = records_[slot];
        if (record.partition == kDynamic && ++record.quiet_frames >= kQuietFrames) {
            quiet.push_back(slot);
        }
    }
    for (uint32_t slot : quiet) {
        Record& record = records_[slot];
        // The partition whose bounds grow least.
        uint32_t best = kUnplaced;
        float best_growth = 0.f;
        for (uint32_t p = 0; p < partitions_.size(); ++p) {
            const Partition& partition = partitions_[p];
            if (!partition.active || partition.group) {
                continue;
            }
            float lo[3] = {partition.ref.aabb_min[0], partition.ref.aabb_min[1], partition.ref.aabb_min[2]};
            float hi[3] = {partition.ref.aabb_max[0], partition.ref.aabb_max[1], partition.ref.aabb_max[2]};
            const float before = surface_area(lo, hi);
            merge_bounds(lo, hi, record.bounds.min, record.bounds.max);
            const float growth = surface_area(lo, hi) - before;
            if (best == kUnplaced || growth < best_growth) {
                best = p;
                best_growth = growth;
            }
        }
        if (best == kUnplaced) {
            best = alloc_partition();
        }
        partitions_[best].members.push_back(slot);
        partitions_[best].dirty = true;
        record.partition = best;
        dynamic_.dirty = true;
    }
}

void SceneBvh::recut(FrameContext& ctx)
{
    vector<uint32_t> slots;
    vector<BvhItemBounds> items;
    for (uint32_t slot = 0; slot < records_.size(); ++slot) {
        const Record& record = records_[slot];
        if (record.owner && record.partition != kDynamic) {
            slots.push_back(slot);
            items.push_back(record.bounds);
        }
    }
    unplaced_.clear();

    for (uint32_t p = 0; p < partitions_.size(); ++p) {
        if (partitions_[p].active && !partitions_[p].group) {
            partitions_[p] = Partition{};
            free_partitions_.push_back(p);
        }
    }

    const BvhTree cut = build_bvh_tree({items.data(), items.size()}, kPartitionRecords, true);
    for (const auto& node : cut.nodes) {
        if (node.child_count != 0 || node.shape_count == 0) {
            continue;
        }
        const uint32_t index = alloc_partition();
        for (uint32_t i = 0; i < node.shape_count; ++i) {
            const uint32_t slot = slots[cut.order[node.first_shape + i]];
            partitions_[index].members.push_back(slot);
            records_[slot].partition = index;
        }
    }
    top_dirty_ = true;
    cut_cost_ = 0.f;
    flush_partitions(ctx);
}

void SceneBvh::flush_partitions(FrameContext& ctx)
{
    for (uint32_t p = 0; p < partitions_.size(); ++p) {
        if (!partitions_[p].active || !partitions_[p].dirty || partitions_[p].group) {
            continue;
        }
        auto& members = partitions_[p].members;
        members.erase(std::remove_if(members.begin(), members.end(),
                                     [&](uint32_t slot) { return records_[slot].partition != p; }),
                      members.end());
        if (members.size() > kPartitionRecords * 2u) {
            vector<BvhItemBounds> items;
            for (uint32_t slot : members) {
                items.push_back(records_[slot].bounds);
            }
            const vector<uint32_t> all = members;
            const BvhTree cut = build_bvh_tree({items.data(), items.size()}, kPartitionRecords, true);
            bool first = true;
            for (const auto& node : cut.nodes) {
                if (node.child_count != 0 || node.shape_count == 0) {
                    continue;
                }
                // alloc_partition may reallocate partitions_.
                const uint32_t index = first ? p : alloc_partition();
                if (first) {
                    partitions_[p].members.clear();
                    first = false;
                }
                for (uint32_t i = 0; i < node.shape_count; ++i) {
                    const uint32_t slot = all[cut.order[node.first_shape + i]];
                    partitions_[index].members.push_back(slot);
                    records_[slot].partition = index;
                }
            }
        }
    }
    for (uint32_t p = 0; p < partitions_.size(); ++p) {
        Partition& partition = partitions_[p];
        if (!partition.active || !partition.dirty) {
            continue;
        }
        partition.dirty = false;
        if (is_instance_dynamic(p)) {
            root_dirty_ = true;
        } else {
            top_dirty_ = true;
        }
        if (partition.members.empty()) {
            if (partition.group) {
                auto& owned = partition.group->partitions;
                owned.erase(std::remove(owned.begin(), owned.end(), p), owned.end());
                if (partition.group->open_partition == p) {
                    partition.group->open_partition = kUnplaced;
                }
                if (partition.group->dynamic_partition == p) {
                    partition.group->dynamic_partition = kUnplaced;
                }
            }
            partition = Partition{};
            free_partitions_.push_back(p);
            continue;
        }
        upload_partition(partition, ctx, true);
    }
}

void SceneBvh::upload_partition(Partition& partition, FrameContext& ctx, bool geometry)
{
    const InstanceGroup* group = partition.group;
    vector<BvhItemBounds> items;
    items.reserve(partition.members.size());
    for (uint32_t slot : partition.members) {
        items.push_back(group ? instance_bounds(*group, slot) : records_[slot].bounds);
    }
    BvhTree tree = build_bvh_tree({items.data(), items.size()});

    if (geometry && !group) {
        // Shapes in leaf order, so a ray that reaches neighbouring records finds
        // their shapes nearby in memory.
        size_t shape_count = 0;
        size_t tree_node_count = 0;
        for (uint32_t slot : partition.members) {
            shape_count += records_[slot].shapes.size();
            tree_node_count += records_[slot].tree.size();
        }

        uint32_t shape_base = 0;
        alloc_elements(ctx.bvh_shapes_arena, partition.shapes, shape_count, sizeof(RtShape), shape_base);
        uint32_t tree_base = 0;
        IGpuArena::Ptr tree_arena;
        if (tree_node_count != 0) {
            tree_arena = ctx.resources->shared_arena(IRenderBackend::kGlobalBlasNodes, sizeof(BlasNode));
        }
        alloc_elements(tree_arena.get(), partition.tree_nodes, tree_node_count, sizeof(BlasNode), tree_base);

        vector<RtShape> shapes;
        vector<BlasNode> tree_nodes;
        shapes.reserve(shape_count);
        tree_nodes.reserve(tree_node_count);
        for (uint32_t item : tree.order) {
            Record& record = records_[partition.members[item]];
            const auto first_shape = shape_base + static_cast<uint32_t>(shapes.size());
            record.gpu.first_shape = first_shape;
            record.gpu.shape_count = static_cast<uint32_t>(record.shapes.size());
            record.gpu.tree_root = kRtRecordNoTree;
            if (!record.tree.empty() && partition.tree_nodes.valid()) {
                const auto first_node = tree_base + static_cast<uint32_t>(tree_nodes.size());
                record.gpu.tree_root = first_node;
                for (BlasNode node : record.tree) {
                    if (node.shape_count > 0) {
                        node.first_shape += first_shape;
                    } else {
                        node.first_child += first_node;
                    }
                    tree_nodes.push_back(node);
                }
            }
            for (const auto& shape : record.shapes) {
                shapes.push_back(shape);
            }
        }
        if (partition.shapes.valid()) {
            ctx.bvh_shapes_arena->write_at(partition.shapes.offset(), shapes.data(), shapes.size() * sizeof(RtShape));
        }
        if (partition.tree_nodes.valid()) {
            tree_arena->write_at(partition.tree_nodes.offset(), tree_nodes.data(),
                                 tree_nodes.size() * sizeof(BlasNode));
        }
    }

    float origin[3];
    for (int k = 0; k < 3; ++k) {
        origin[k] = 0.5f * (tree.nodes[0].aabb_min[k] + tree.nodes[0].aabb_max[k]);
    }

    uint32_t record_base = 0;
    alloc_elements(ctx.records_arena, partition.records, tree.order.size(), sizeof(RtRecord), record_base);
    if (partition.records.valid()) {
        vector<RtRecord> gpu_records;
        gpu_records.reserve(tree.order.size());
        for (uint32_t item : tree.order) {
            const uint32_t member = partition.members[item];
            RtRecord record = group ? instance_record(*group, member) : records_[member].gpu;
            for (int k = 0; k < 3; ++k) {
                record.inv_row[k][3] -= origin[k];
            }
            gpu_records.push_back(record);
        }
        ctx.records_arena->write_at(partition.records.offset(), gpu_records.data(),
                                    gpu_records.size() * sizeof(RtRecord));
    }

    uint32_t node_base = 0;
    alloc_elements(ctx.bvh_nodes_arena, partition.nodes, tree.nodes.size(), sizeof(GpuBvhNode), node_base);
    // Indices stamped onto a failed region would point into other trees, where a
    // cycle would hang the walk. Rays skip the partition instead.
    if (!partition.nodes.valid() || !partition.records.valid()
        || (geometry && !group && !partition.shapes.valid())) {
        VELK_LOG(E, "SceneBvh: partition allocation failed");
        set_inverted_bounds(partition.ref);
        partition.node_count = 0;
        return;
    }

    partition.ref = {};
    for (int k = 0; k < 3; ++k) {
        partition.ref.aabb_min[k] = tree.nodes[0].aabb_min[k];
        partition.ref.aabb_max[k] = tree.nodes[0].aabb_max[k];
    }
    partition.ref.aabb_min[3] = origin[0];
    partition.ref.aabb_max[3] = origin[1];
    std::memcpy(&partition.ref.first_shape, &origin[2], sizeof(float));
    partition.ref.first_child = node_base;
    partition.ref.child_count = kBvhPartitionRef;

    for (auto& node : tree.nodes) {
        if (node.child_count != 0) {
            node.first_child += node_base;
        } else {
            node.first_shape += record_base;
        }
        // Rounds outward, so subtracting the origin never shrinks a box.
        for (int k = 0; k < 3; ++k) {
            node.aabb_min[k] = std::nextafter(node.aabb_min[k] - origin[k], -std::numeric_limits<float>::infinity());
            node.aabb_max[k] = std::nextafter(node.aabb_max[k] - origin[k], std::numeric_limits<float>::infinity());
        }
    }
    ctx.bvh_nodes_arena->write_at(partition.nodes.offset(), tree.nodes.data(),
                                  tree.nodes.size() * sizeof(GpuBvhNode));
    partition.node_count = static_cast<uint32_t>(tree.nodes.size());
}

void SceneBvh::rebuild_top_tree(FrameContext& ctx)
{
    vector<uint32_t> active;
    vector<BvhItemBounds> items;
    for (uint32_t p = 0; p < partitions_.size(); ++p) {
        if (partitions_[p].active && !is_instance_dynamic(p)) {
            const auto& ref = partitions_[p].ref;
            BvhItemBounds bounds{};
            for (int k = 0; k < 3; ++k) {
                bounds.min[k] = ref.aabb_min[k];
                bounds.max[k] = ref.aabb_max[k];
            }
            active.push_back(p);
            items.push_back(bounds);
        }
    }
    top_dirty_ = false;
    root_dirty_ = true;
    has_top_ = !active.empty();
    top_node_count_ = 0;
    if (!has_top_) {
        top_nodes_ = {};
        return;
    }

    BvhTree tree = build_bvh_tree({items.data(), items.size()}, 1, true);
    uint32_t base = 0;
    if (!alloc_elements(ctx.bvh_nodes_arena, top_nodes_, tree.nodes.size(), sizeof(GpuBvhNode), base)) {
        VELK_LOG(E, "SceneBvh: top tree allocation failed");
        has_top_ = false;
        top_node_count_ = 0;
        return;
    }

    for (auto& node : tree.nodes) {
        if (node.child_count != 0) {
            node.first_child += base;
        } else {
            node = partitions_[active[tree.order[node.first_shape]]].ref;
        }
    }
    ctx.bvh_nodes_arena->write_at(top_nodes_.offset(), tree.nodes.data(),
                                  tree.nodes.size() * sizeof(GpuBvhNode));
    top_root_ = tree.nodes[0];

    top_node_count_ = static_cast<uint32_t>(tree.nodes.size());
    for (uint32_t p : active) {
        top_node_count_ += partitions_[p].node_count;
    }


    update_probe_volume(top_root_.aabb_min, top_root_.aabb_max, static_cast<uint32_t>(ctx.present_counter));

    // The partitions' area against the root's, weighted by records: how much a
    // ray pays. It grows as partitions change without a new cut.
    float cost = 0.f;
    const float root_area = std::max(surface_area(top_root_.aabb_min, top_root_.aabb_max), 1e-30f);
    for (uint32_t i = 0; i < active.size(); ++i) {
        const auto& partition = partitions_[active[i]];
        cost += surface_area(partition.ref.aabb_min, partition.ref.aabb_max) * partition.members.size();
    }
    cost /= root_area;
    if (cut_cost_ == 0.f) {
        cut_cost_ = cost;
    } else if (cost > cut_cost_ * kRecutDrift) {
        recut(ctx);
        rebuild_top_tree(ctx);
    }
}

void SceneBvh::upload_root(FrameContext& ctx)
{
    root_dirty_ = false;
    const bool has_dynamic = !dynamic_.members.empty();

    // The probe volume follows the static partitions, so movers do not reset it.
    // It only follows movers in a scene with nothing else.
    if (!has_top_ && has_dynamic) {
        update_probe_volume(dynamic_.ref.aabb_min, dynamic_.ref.aabb_max,
                            static_cast<uint32_t>(ctx.present_counter));
    }

    // What the root joins: the static top tree, the dynamic partition, and each instance
    // group's dynamic partition.
    vector<GpuBvhNode> parts;
    uint32_t part_node_count = 0;
    if (has_top_) {
        parts.push_back(top_root_);
        part_node_count += top_node_count_;
    }
    if (has_dynamic) {
        parts.push_back(dynamic_.ref);
        part_node_count += dynamic_.node_count;
    }
    for (const auto& [element, group] : instance_groups_) {
        if (group.dynamic_partition != kUnplaced && partitions_[group.dynamic_partition].node_count != 0) {
            parts.push_back(partitions_[group.dynamic_partition].ref);
            part_node_count += partitions_[group.dynamic_partition].node_count;
        }
    }

    BvhTree tree;
    if (parts.size() > 1) {
        vector<BvhItemBounds> items;
        for (const auto& part : parts) {
            BvhItemBounds bounds{};
            for (int k = 0; k < 3; ++k) {
                bounds.min[k] = part.aabb_min[k];
                bounds.max[k] = part.aabb_max[k];
            }
            items.push_back(bounds);
        }
        tree = build_bvh_tree({items.data(), items.size()}, 1, true);
    }

    vector<GpuBvhNode> nodes;
    nodes.resize(parts.size() > 1 ? tree.nodes.size() : size_t(1));
    uint32_t base = 0;
    if (!alloc_elements(ctx.bvh_nodes_arena, root_nodes_, nodes.size(), sizeof(GpuBvhNode), base)) {
        VELK_LOG(E, "SceneBvh: root allocation failed");
        root_ = 0;
        node_count_ = 0;
        return;
    }
    if (parts.empty()) {
        // Empty BVH: a single inverted-AABB root, so node_count > 0 and every ray
        // rejects.
        set_inverted_bounds(nodes[0]);
    } else if (parts.size() == 1) {
        nodes[0] = parts[0];
    } else {
        for (size_t i = 0; i < nodes.size(); ++i) {
            nodes[i] = tree.nodes[i];
            if (nodes[i].child_count != 0) {
                nodes[i].first_child += base;
            } else {
                nodes[i] = parts[tree.order[tree.nodes[i].first_shape]];
            }
        }
    }
    ctx.bvh_nodes_arena->write_at(root_nodes_.offset(), nodes.data(), nodes.size() * sizeof(GpuBvhNode));
    root_ = base;
    node_count_ = static_cast<uint32_t>(nodes.size()) + part_node_count;
}

void SceneBvh::full_build(IScene* scene, const EmitContext& emit)
{
    for (uint32_t slot = 0; slot < records_.size(); ++slot) {
        if (records_[slot].owner) {
            free_record(slot);
        }
    }
    element_records_.clear();
    for (auto& [element, group] : instance_groups_) {
        free_instance_partitions(group);
    }
    instance_groups_.clear();
    unresolved_.clear();
    dynamic_ = Partition{};
    dynamic_transforms_dirty_ = false;
    shape_updates_.clear();

    if (scene) {
        walk_scene_preorder(scene, scene->root(), [&](IElement* element) { update_element(element, emit); });
    }
    recut(emit.frame);
    needs_full_build_ = false;
}

void SceneBvh::update(const SceneState& state, FrameContext& ctx, ShapeCb shape_cb, void* shape_user)
{
    VELK_PERF_SCOPE("renderer.bvh_rebuild");
    const EmitContext emit{ctx, shape_cb, shape_user};

    {
        VELK_PERF_SCOPE("renderer.bvh_emit");
        if (needs_full_build_) {
            full_build(state.scene, emit);
        } else {
            for (const auto& removed : state.removed_list) {
                remove_element(removed.get());
            }

            vector<IElement*> retry = std::move(unresolved_);
            unresolved_.clear();
            for (auto* element : retry) {
                update_element(element, emit);
            }

            auto* hierarchy = interface_cast<IHierarchy>(state.scene);
            for (size_t i = 0; i < state.redraw_list.size(); ++i) {
                IElement* element = state.redraw_list[i];
                if (element_records_.find(element) == element_records_.end()
                    && !::velk::has_attachment<IVisual>(element)) {
                    continue;
                }
                // Removed after it was marked, and not added back.
                if (!state.removed_list.empty() && hierarchy) {
                    auto* object = interface_cast<IObject>(element);
                    if (!object || !hierarchy->contains(object->get_self<IObject>())) {
                        continue;
                    }
                }
                update_element(element, emit);
            }
        }
    }

    // A batch that is large against the scene is a load, so it goes straight to
    // static partitions rather than through the dynamic partition.
    if (unplaced_.size() > std::max<size_t>(kMaxUnplacedToDynamic, live_records_ / 4u)) {
        recut(ctx);
    } else if (!unplaced_.empty()) {
        for (uint32_t slot : unplaced_) {
            if (records_[slot].owner) {
                records_[slot].partition = kDynamic;
                dynamic_.members.push_back(slot);
                dynamic_.dirty = true;
            }
        }
        unplaced_.clear();
    }

    demote_quiet_records();
    ++update_frame_;
    for (auto& [element, group] : instance_groups_) {
        demote_quiet_instances(group);
    }

    bool any_partition_dirty = dynamic_.dirty || dynamic_transforms_dirty_ || top_dirty_;
    for (uint32_t p = 0; !any_partition_dirty && p < partitions_.size(); ++p) {
        any_partition_dirty = partitions_[p].active && partitions_[p].dirty;
    }
    if (any_partition_dirty) {
        VELK_PERF_SCOPE("renderer.bvh_build");
        flush_partitions(ctx);
        if (top_dirty_) {
            rebuild_top_tree(ctx);
        }
        if (dynamic_.dirty) {
            auto& members = dynamic_.members;
            members.erase(std::remove_if(members.begin(), members.end(),
                                         [&](uint32_t slot) { return records_[slot].partition != kDynamic; }),
                          members.end());
        }
        if (dynamic_.dirty || dynamic_transforms_dirty_) {
            if (dynamic_.members.empty()) {
                dynamic_.nodes = {};
                dynamic_.records = {};
                dynamic_.shapes = {};
                dynamic_.tree_nodes = {};
                dynamic_.node_count = 0;
            } else {
                upload_partition(dynamic_, ctx, dynamic_.dirty);
            }
            dynamic_.dirty = false;
            dynamic_transforms_dirty_ = false;
            root_dirty_ = true;
        }
    }

    for (uint32_t slot : shape_updates_) {
        const Record& record = records_[slot];
        if (record.owner && !record.shapes.empty()) {
            ctx.bvh_shapes_arena->update_at(record.gpu.first_shape * sizeof(RtShape), record.shapes.data(),
                                            record.shapes.size() * sizeof(RtShape));
        }
    }
    shape_updates_.clear();

    for (uint32_t slot : freed_records_) {
        free_records_.push_back(slot);
    }
    freed_records_.clear();

    if (root_dirty_) {
        upload_root(ctx);
    }
}

} // namespace velk::impl
