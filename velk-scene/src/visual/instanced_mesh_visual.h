#ifndef VELK_UI_INSTANCED_MESH_VISUAL_H
#define VELK_UI_INSTANCED_MESH_VISUAL_H

#include <velk/string.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-scene/ext/trait.h>
#include <velk-scene/plugin.h>

namespace velk::impl {

/**
 * @brief Draws IVisual3D::mesh and any added meshes, once per instance.
 *
 * The transform array here is the only CPU copy of the instances. Raster reads a region
 * of ElementInstance, at the index a slot has there, whose params[1] is the mesh and
 * params[2] the slot, and a removed slot holds a zero matrix, which draws nothing.
 */
class InstancedMeshVisual : public ext::Visual3D<InstancedMeshVisual, IInstancedMeshVisual, IInstancedMeshInternal>
{
public:
    VELK_CLASS_UID(ClassId::Visual::InstancedMesh, "InstancedMeshVisual");

    // IVisual
    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx, const ::velk::size& bounds) override;
    aabb get_local_bounds(const ::velk::size& bounds) const override;

    // IInstancedMeshVisual
    uint32_t add_mesh(const IMesh::Ptr& mesh) override;
    uint32_t add_instance(const mat4& transform, uint32_t mesh = 0) override;
    uint32_t add_instances(array_view<const InstanceTransform> transforms, uint32_t mesh = 0) override;
    void set_transform(uint32_t slot, const mat4& transform) override;
    void remove_instance(uint32_t slot) override;
    uint32_t get_instance_count() const override { return live_count_; }
    void set_draw_distance(float distance) override;
    void set_motion(string_view glsl, float max_displacement) override;
    void set_motion_params(const vec4& params) override;
    void set_residency_budget(uint64_t bytes) override;

    // IInstancedMeshInternal
    array_view<const InstanceTransform> get_transforms() const override
    {
        return {transforms_.data(), transforms_.size()};
    }
    bool is_live(uint32_t slot) const override { return slot < live_.size() && live_[slot] != 0; }
    uint32_t get_instance_index(uint32_t slot) const override { return slot_index_[slot]; }
    uint32_t get_instance_capacity() const override { return instance_capacity_; }
    bool has_residency_budget() const override
    {
        return residency_bytes_ != 0 && far_form_ && motion_source_.empty();
    }
    bool assign_instance_index(uint32_t slot) override;
    void release_instance_index(uint32_t slot) override;
    void set_medium_id(uint32_t id) override { medium_id_ = id; }
    uint32_t get_medium_id() const override { return medium_id_; }
    void flush_instances(IGpuArena& arena, IGpuResourceManager& resources, const mat4& world) override;
    uint64_t instance_region_offset() const override { return region_.offset(); }
    uint64_t instance_region_size() const override { return region_.size(); }
    uint32_t get_mesh_count() const override { return static_cast<uint32_t>(added_meshes_.size()) + 1u; }
    IMesh::Ptr get_mesh(uint32_t index) const override;
    uint32_t get_slot_mesh(uint32_t slot) const override { return slot_mesh_[slot]; }
    uint32_t get_mesh_instance_count(uint32_t index) const override
    {
        return index < mesh_counts_.size() ? mesh_counts_[index] : 0u;
    }
    void get_cull_sphere(uint32_t index, float out[4]) const override;
    float get_draw_distance() const override { return draw_distance_; }
    void set_far_form(bool enabled) override;
    bool get_far_form() const override { return far_form_; }
    void get_far_look(uint32_t index, float& area, float albedo[3], float* direction_area) const override;
    string_view get_motion_source() const override { return motion_source_; }
    float get_max_displacement() const override
    {
        return (motion_source_.empty() ? 0.f : max_displacement_) + (ray_motion_source_.empty() ? 0.f : ray_motion_reach_);
    }
    string_view get_ray_motion_source() const override { return ray_motion_source_; }
    float get_ray_motion_reach() const override { return ray_motion_reach_; }
    void set_ray_motion(string_view glsl, float reach) override
    {
        ray_motion_source_ = string(glsl);
        ray_motion_reach_ = reach;
    }
    uint64_t rest_region_offset() const override { return rest_region_.offset(); }
    uint64_t rest_region_size() const override { return rest_region_.size(); }
    bool take_motion_params(float out[4]) override;
    bool take_rest_changed() override;
    void take_changed_slots(vector<uint32_t>& changed, bool& all, vector<InstanceBefore>& before,
                            bool& before_complete) override;
    void set_partition_cull(uint32_t table_word, uint32_t count, uint32_t list_word) override
    {
        cull_table_word_ = table_word;
        cull_count_ = count;
        cull_list_word_ = list_word;
    }
    void get_partition_cull(uint32_t& table_word, uint32_t& count, uint32_t& list_word) const override
    {
        table_word = cull_table_word_;
        count = cull_count_;
        list_word = cull_list_word_;
    }

private:
    uint32_t place(const InstanceTransform& transform, uint32_t mesh);
    void note_changed(uint32_t slot);
    /// Keeps where @p slot's instance is, before it moves or goes away.
    void note_before(uint32_t slot);
    /// Without a budget, every slot's index is the slot. With one, none has a place yet.
    void reset_instance_indices();
    void update_instance_capacity();
    void grow_bounds(const InstanceTransform& transform);

    vector<InstanceTransform> transforms_;
    vector<uint8_t> live_;
    vector<uint16_t> slot_mesh_;
    vector<uint32_t> slot_index_;      ///< Per slot, its index in the raster region.
    uint32_t instance_capacity_ = 0;
    uint64_t residency_bytes_ = 0;
    uint32_t medium_id_ = 0;
    uint32_t next_index_ = 0;          ///< Below it, indices have been handed out.
    vector<uint32_t> free_indices_;
    vector<uint32_t> cleared_indices_;  ///< Freed, to zero at the next flush.
    vector<IMesh::Ptr> added_meshes_;  ///< Meshes 1 and up.
    vector<uint32_t> mesh_counts_;     ///< Live instances per mesh.
    vector<uint32_t> free_slots_;
    uint32_t live_count_ = 0;

    /// Slots to rewrite in the raster region, and to report to the BVH.
    vector<uint32_t> flush_slots_;
    vector<uint32_t> changed_slots_;
    bool flush_all_ = true;
    bool all_changed_ = true;
    vector<InstanceBefore> before_;  ///< See take_changed_slots.
    bool before_complete_ = true;

    uint32_t cull_table_word_ = 0;
    uint32_t cull_count_ = 0;
    uint32_t cull_list_word_ = 0;

    ArenaRegion region_;
    mat4 flushed_world_{};
    float draw_distance_ = 0.f;
    bool far_form_ = true;

    string motion_source_;
    float max_displacement_ = 0.f;
    string ray_motion_source_;
    float ray_motion_reach_ = 0.f;
    ArenaRegion rest_region_;
    float motion_params_[4] = {0.f, 0.f, 0.f, 0.f};
    bool motion_params_changed_ = true;
    bool rest_changed_ = false;

    /// Grows only. Instance origins, and the largest scale any transform has.
    float origin_min_[3] = {0.f, 0.f, 0.f};
    float origin_max_[3] = {0.f, 0.f, 0.f};
    float max_scale_ = 0.f;
    bool has_bounds_ = false;
};

} // namespace velk::impl

#endif // VELK_UI_INSTANCED_MESH_VISUAL_H
