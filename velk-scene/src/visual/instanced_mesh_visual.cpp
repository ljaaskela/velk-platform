#include "instanced_mesh_visual.h"

#include "primitive_shaders.h"

#include <velk/api/attachment.h>
#include <velk/api/perf.h>
#include <velk/api/state.h>

#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/material/intf_material_options.h>
#include <velk-render/plugin.h>
#include <velk-scene/instance_types.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace velk::impl {

namespace {

/// Past this share of the slots, all raster instances are rewritten.
constexpr uint32_t kChangedListDivisor = 4;
/// What an instance with a place uses on the GPU: its raster data, its record and two BVH
/// nodes of 48 bytes.
constexpr uint64_t kResidentInstanceBytes = sizeof(ElementInstance) + 64u + 96u;

ElementInstance make_instance(const mat4& world, const InstanceTransform& transform, uint32_t mesh, uint32_t slot)
{
    ElementInstance instance{};
    instance.world_matrix = world * transform.to_mat4();
    instance.size = {1.f, 1.f, 1.f, 0.f};
    instance.col = ::velk::color::white();
    instance.params[1] = mesh;
    instance.params[2] = slot;
    return instance;
}

} // namespace

vector<DrawEntry> InstancedMeshVisual::get_draw_entries(::velk::IRenderContext& /*ctx*/,
                                                        const ::velk::size& /*bounds*/)
{
    vector<DrawEntry> out;
    for (uint32_t m = 0; m < get_mesh_count(); ++m) {
        auto mesh = get_mesh(m);
        if (!mesh) {
            continue;
        }
        ::velk::find_or_create_attachment<::velk::IMaterialOptions>(this, ::velk::ClassId::MaterialOptions);
        for (auto& primitive : mesh->get_primitives()) {
            if (!primitive) {
                continue;
            }
            DrawEntry entry{};
            entry.pipeline_key = kPrimitive3DPipelineKey;
            entry.primitive = primitive;
            if (auto ps = read_state<IMeshPrimitive>(primitive.get()); ps && ps->material) {
                entry.material = ps->material.get<IProgram>();
            }
            entry.instance_size = sizeof(ElementInstance);
            entry.instance_source = get_self<IInterface>();
            entry.instance_source_mesh = m;
            out.push_back(std::move(entry));
        }
    }
    return out;
}

IMesh::Ptr InstancedMeshVisual::get_mesh(uint32_t index) const
{
    if (index == 0) {
        auto state = read_state<IVisual3D>(this);
        return state ? state->mesh.get<IMesh>() : nullptr;
    }
    return index <= added_meshes_.size() ? added_meshes_[index - 1] : nullptr;
}

uint32_t InstancedMeshVisual::add_mesh(const IMesh::Ptr& mesh)
{
    added_meshes_.push_back(mesh);
    invoke_visual_changed();
    return static_cast<uint32_t>(added_meshes_.size());
}

aabb InstancedMeshVisual::get_local_bounds(const ::velk::size& /*bounds*/) const
{
    if (!has_bounds_) {
        return {};
    }
    // A sphere around each mesh's origin that holds its bounds, so any rotation fits.
    float reach2 = 0.f;
    for (uint32_t m = 0; m < get_mesh_count(); ++m) {
        if (auto mesh = get_mesh(m)) {
            const aabb local = mesh->get_bounds();
            const vec3 lo = local.min();
            const vec3 hi = local.max();
            reach2 = std::max({reach2, vec3::dot(lo, lo), vec3::dot(hi, hi)});
        }
    }
    const float reach = max_scale_ * std::sqrt(reach2);
    aabb out;
    out.position = {origin_min_[0] - reach, origin_min_[1] - reach, origin_min_[2] - reach};
    out.extent = {origin_max_[0] - origin_min_[0] + reach * 2.f, origin_max_[1] - origin_min_[1] + reach * 2.f,
                  origin_max_[2] - origin_min_[2] + reach * 2.f};
    return out;
}

void InstancedMeshVisual::grow_bounds(const InstanceTransform& transform)
{
    for (int row = 0; row < 3; ++row) {
        const float origin = transform.rows[row][3];
        origin_min_[row] = has_bounds_ ? std::min(origin_min_[row], origin) : origin;
        origin_max_[row] = has_bounds_ ? std::max(origin_max_[row], origin) : origin;
    }
    has_bounds_ = true;
    for (int col = 0; col < 3; ++col) {
        const float x = transform.rows[0][col];
        const float y = transform.rows[1][col];
        const float z = transform.rows[2][col];
        max_scale_ = std::max(max_scale_, std::sqrt(x * x + y * y + z * z));
    }
}

uint32_t InstancedMeshVisual::place(const InstanceTransform& transform, uint32_t mesh)
{
    uint32_t slot;
    if (free_slots_.empty()) {
        slot = static_cast<uint32_t>(transforms_.size());
        transforms_.push_back(transform);
        live_.push_back(1);
        slot_mesh_.push_back(static_cast<uint16_t>(mesh));
        slot_index_.push_back(has_residency_budget() ? kNoInstanceIndex : next_index_++);
        update_instance_capacity();
    } else {
        slot = free_slots_.back();
        free_slots_.pop_back();
        transforms_[slot] = transform;
        live_[slot] = 1;
        slot_mesh_[slot] = static_cast<uint16_t>(mesh);
    }
    if (mesh_counts_.size() <= mesh) {
        mesh_counts_.resize(mesh + 1u, 0u);
    }
    ++mesh_counts_[mesh];
    ++live_count_;
    grow_bounds(transform);
    note_changed(slot);
    return slot;
}

void InstancedMeshVisual::note_changed(uint32_t slot)
{
    const size_t limit = transforms_.size() / kChangedListDivisor + 16u;
    if (!flush_all_) {
        flush_slots_.push_back(slot);
        if (flush_slots_.size() > limit) {
            flush_all_ = true;
            flush_slots_.clear();
        }
    }
    // The BVH wants every slot, however many, or it cuts all its partitions again.
    if (!all_changed_) {
        changed_slots_.push_back(slot);
    }
}

void InstancedMeshVisual::note_before(uint32_t slot)
{
    // Past this many, those that read where things changed treat everything as changed.
    constexpr size_t kMaxBefore = 256;
    if (!before_complete_) {
        return;
    }
    if (before_.size() >= kMaxBefore) {
        before_complete_ = false;
        before_.clear();
        return;
    }
    before_.push_back({slot, slot_mesh_[slot], transforms_[slot]});
}

uint32_t InstancedMeshVisual::add_instance(const mat4& transform, uint32_t mesh)
{
    const uint32_t slot = place(InstanceTransform::from(transform), mesh);
    invoke_visual_changed();
    return slot;
}

uint32_t InstancedMeshVisual::add_instances(array_view<const InstanceTransform> transforms, uint32_t mesh)
{
    if (transforms.empty()) {
        return 0;
    }
    transforms_.reserve(transforms_.size() + transforms.size());
    live_.reserve(live_.size() + transforms.size());
    slot_mesh_.reserve(slot_mesh_.size() + transforms.size());
    slot_index_.reserve(slot_index_.size() + transforms.size());
    uint32_t first = 0;
    for (size_t i = 0; i < transforms.size(); ++i) {
        const uint32_t slot = place(transforms[i], mesh);
        first = i == 0 ? slot : first;
    }
    invoke_visual_changed();
    return first;
}

void InstancedMeshVisual::set_transform(uint32_t slot, const mat4& transform)
{
    if (!is_live(slot)) {
        return;
    }
    note_before(slot);
    transforms_[slot] = InstanceTransform::from(transform);
    grow_bounds(transforms_[slot]);
    note_changed(slot);
    invoke_trait_dirty(DirtyFlags::Transform);
}

void InstancedMeshVisual::remove_instance(uint32_t slot)
{
    if (!is_live(slot)) {
        return;
    }
    note_before(slot);
    live_[slot] = 0;
    free_slots_.push_back(slot);
    --mesh_counts_[slot_mesh_[slot]];
    --live_count_;
    if (has_residency_budget()) {
        release_instance_index(slot);
    }
    note_changed(slot);
    invoke_trait_dirty(DirtyFlags::Transform);
}

void InstancedMeshVisual::update_instance_capacity()
{
    const auto slots = static_cast<uint32_t>(transforms_.size());
    instance_capacity_ = has_residency_budget()
        ? static_cast<uint32_t>(std::min<uint64_t>(slots, residency_bytes_ / kResidentInstanceBytes))
        : slots;
}

void InstancedMeshVisual::reset_instance_indices()
{
    const bool budget = has_residency_budget();
    free_indices_.clear();
    cleared_indices_.clear();
    next_index_ = budget ? 0u : static_cast<uint32_t>(transforms_.size());
    for (uint32_t slot = 0; slot < slot_index_.size(); ++slot) {
        slot_index_[slot] = budget ? kNoInstanceIndex : slot;
    }
    update_instance_capacity();
}

void InstancedMeshVisual::set_residency_budget(uint64_t bytes)
{
    residency_bytes_ = bytes;
    reset_instance_indices();
    flush_all_ = true;
    all_changed_ = true;
    invoke_visual_changed();
}

bool InstancedMeshVisual::assign_instance_index(uint32_t slot)
{
    if (slot_index_[slot] != kNoInstanceIndex) {
        return true;
    }
    if (!free_indices_.empty()) {
        slot_index_[slot] = free_indices_.back();
        free_indices_.pop_back();
    } else if (next_index_ < instance_capacity_) {
        slot_index_[slot] = next_index_++;
    } else {
        return false;
    }
    if (!flush_all_) {
        flush_slots_.push_back(slot);
    }
    return true;
}

void InstancedMeshVisual::release_instance_index(uint32_t slot)
{
    const uint32_t index = slot_index_[slot];
    if (index == kNoInstanceIndex) {
        return;
    }
    slot_index_[slot] = kNoInstanceIndex;
    free_indices_.push_back(index);
    if (!flush_all_) {
        cleared_indices_.push_back(index);
    }
}

void InstancedMeshVisual::set_draw_distance(float distance)
{
    draw_distance_ = distance;
    invoke_visual_changed();
}

void InstancedMeshVisual::set_far_form(bool enabled)
{
    far_form_ = enabled;
    reset_instance_indices();
    flush_all_ = true;
    // The BVH holds what wide rays use, and it comes from a new cut.
    all_changed_ = true;
    invoke_visual_changed();
}

void InstancedMeshVisual::get_far_look(uint32_t index, float& area, float albedo[3], float* direction_area) const
{
    area = 0.f;
    albedo[0] = albedo[1] = albedo[2] = 1.f;
    if (direction_area) {
        std::fill(direction_area, direction_area + kSurfaceDirectionCount, 0.f);
    }
    auto mesh = get_mesh(index);
    if (!mesh || !far_form_) {
        return;
    }
    float total = 0.f;
    float sum[3] = {0.f, 0.f, 0.f};
    for (const auto& primitive : mesh->get_primitives()) {
        const float primitive_area = primitive->get_surface_area();
        color tint = color::white();
        if (auto state = read_state<IMeshPrimitive>(primitive.get()); state && state->material) {
            if (auto material = state->material.template get<IMaterial>()) {
                tint = material->get_average_albedo();
            }
        }
        if (direction_area) {
            const float* facing = primitive->get_direction_area();
            for (int d = 0; d < kSurfaceDirectionCount; ++d) {
                direction_area[d] += facing[d];
            }
        }
        total += primitive_area;
        sum[0] += tint.r * primitive_area;
        sum[1] += tint.g * primitive_area;
        sum[2] += tint.b * primitive_area;
    }
    if (total > 0.f) {
        // A closed shape's mean projected area is a quarter of its surface.
        area = total * 0.25f;
        for (int k = 0; k < 3; ++k) {
            albedo[k] = sum[k] / total;
        }
    }
}

void InstancedMeshVisual::get_cull_sphere(uint32_t index, float out[4]) const
{
    auto mesh = get_mesh(index);
    const aabb local = mesh ? mesh->get_bounds() : aabb{};
    const vec3 lo = local.min();
    const vec3 hi = local.max();
    const vec3 half = (hi - lo) * 0.5f;
    out[0] = lo.x + half.x;
    out[1] = lo.y + half.y;
    out[2] = lo.z + half.z;
    out[3] = vec3::length(half);
}

void InstancedMeshVisual::set_motion(string_view glsl, float max_displacement)
{
    motion_source_ = string(glsl);
    max_displacement_ = max_displacement;
    motion_params_changed_ = true;
    reset_instance_indices();
    flush_all_ = true;
    all_changed_ = true;
    invoke_visual_changed();
}

void InstancedMeshVisual::set_motion_params(const vec4& params)
{
    const float values[4] = {params.x, params.y, params.z, params.w};
    if (std::memcmp(values, motion_params_, sizeof(values)) != 0) {
        std::memcpy(motion_params_, values, sizeof(values));
        motion_params_changed_ = true;
    }
}

bool InstancedMeshVisual::take_motion_params(float out[4])
{
    std::memcpy(out, motion_params_, sizeof(motion_params_));
    const bool changed = motion_params_changed_;
    motion_params_changed_ = false;
    return changed;
}

bool InstancedMeshVisual::take_rest_changed()
{
    const bool changed = rest_changed_;
    rest_changed_ = false;
    return changed;
}

void InstancedMeshVisual::flush_instances(IGpuArena& arena, IGpuResourceManager& resources, const mat4& world)
{
    VELK_PERF_SCOPE("renderer.instanced_flush");
    const auto slot_count = static_cast<uint32_t>(transforms_.size());
    const uint64_t need = uint64_t(instance_capacity_) * sizeof(ElementInstance);
    if (need == 0) {
        region_ = {};
        rest_region_ = {};
        flush_slots_.clear();
        cleared_indices_.clear();
        return;
    }

    // With a motion function, the matrices it starts from are kept beside the instances.
    if (motion_source_.empty()) {
        rest_region_ = {};
    } else if (auto rest_arena = resources.shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t))) {
        const uint64_t rest_need = uint64_t(instance_capacity_) * sizeof(InstanceTransform);
        const auto rest_of = [&](uint32_t slot) {
            return live_[slot] != 0 ? InstanceTransform::from(world * transforms_[slot].to_mat4()) : InstanceTransform{};
        };
        const bool all = flush_all_ || std::memcmp(flushed_world_.m, world.m, sizeof(world.m)) != 0;
        if (rest_region_.size() < rest_need) {
            constexpr uint64_t kMaxSpareRest = uint64_t(16) << 20;  // 16 MiB
            rest_region_ = rest_arena->alloc(rest_need + std::min(rest_need / 4u, kMaxSpareRest), 16);
            auto* out = static_cast<InstanceTransform*>(
                rest_region_.valid() ? rest_arena->mapped_at(rest_region_.offset()) : nullptr);
            for (uint32_t slot = 0; out && slot < slot_count; ++slot) {
                out[slot_index_[slot]] = rest_of(slot);
            }
            rest_changed_ = true;
        } else if (all) {
            vector<InstanceTransform> rest;
            rest.resize(instance_capacity_);
            for (uint32_t slot = 0; slot < slot_count; ++slot) {
                rest[slot_index_[slot]] = rest_of(slot);
            }
            rest_arena->update_at(rest_region_.offset(), rest.data(), rest_need);
            rest_changed_ = true;
        } else {
            for (uint32_t slot : flush_slots_) {
                const InstanceTransform rest = rest_of(slot);
                rest_arena->update_at(rest_region_.offset() + uint64_t(slot_index_[slot]) * sizeof(InstanceTransform), &rest,
                                      sizeof(rest));
                rest_changed_ = true;
            }
        }
    }

    const auto write_slot = [&](uint32_t slot, ElementInstance* out) {
        if (live_[slot] != 0) {
            *out = make_instance(world, transforms_[slot], slot_mesh_[slot], slot);
        } else {
            *out = ElementInstance{};
        }
    };

    // A region no frame has read can be filled directly.
    if (region_.size() < need) {
        constexpr uint64_t kMaxSpare = uint64_t(16) << 20;  // 16 MiB
        region_ = arena.alloc(need + std::min(need / 4u, kMaxSpare), sizeof(ElementInstance));
        auto* out = static_cast<ElementInstance*>(region_.valid() ? arena.mapped_at(region_.offset()) : nullptr);
        if (!out) {
            VELK_LOG(E, "InstancedMeshVisual: no room for %u instances", instance_capacity_);
            return;
        }
        std::memset(static_cast<void*>(out), 0, need);
        for (uint32_t slot = 0; slot < slot_count; ++slot) {
            if (slot_index_[slot] != kNoInstanceIndex) {
                write_slot(slot, out + slot_index_[slot]);
            }
        }
    } else if (flush_all_ || std::memcmp(flushed_world_.m, world.m, sizeof(world.m)) != 0) {
        vector<ElementInstance> instances;
        instances.resize(instance_capacity_);
        std::memset(static_cast<void*>(instances.data()), 0, need);
        for (uint32_t slot = 0; slot < slot_count; ++slot) {
            if (slot_index_[slot] != kNoInstanceIndex) {
                write_slot(slot, &instances[slot_index_[slot]]);
            }
        }
        arena.update_at(region_.offset(), instances.data(), need);
    } else {
        for (uint32_t index : cleared_indices_) {
            const ElementInstance empty{};
            arena.update_at(region_.offset() + uint64_t(index) * sizeof(ElementInstance), &empty, sizeof(empty));
        }
        for (uint32_t slot : flush_slots_) {
            if (slot_index_[slot] == kNoInstanceIndex) {
                continue;
            }
            ElementInstance instance;
            write_slot(slot, &instance);
            arena.update_at(region_.offset() + uint64_t(slot_index_[slot]) * sizeof(ElementInstance), &instance, sizeof(instance));
        }
    }
    cleared_indices_.clear();
    flush_slots_.clear();
    flush_all_ = false;
    flushed_world_ = world;
}

void InstancedMeshVisual::take_changed_slots(vector<uint32_t>& changed, bool& all, vector<InstanceBefore>& before,
                                             bool& before_complete)
{
    all = all_changed_;
    before_complete = before_complete_;
    if (!all) {
        changed.insert(changed.end(), changed_slots_.begin(), changed_slots_.end());
        before.insert(before.end(), before_.begin(), before_.end());
    }
    changed_slots_.clear();
    all_changed_ = false;
    before_.clear();
    before_complete_ = true;
}

} // namespace velk::impl
