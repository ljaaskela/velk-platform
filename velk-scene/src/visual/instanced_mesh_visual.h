#ifndef VELK_UI_INSTANCED_MESH_VISUAL_H
#define VELK_UI_INSTANCED_MESH_VISUAL_H

#include <velk/vector.h>

#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-scene/ext/trait.h>
#include <velk-scene/plugin.h>

namespace velk::impl {

/**
 * @brief Draws IVisual3D::mesh once per instance.
 *
 * The transform array here is the only CPU copy of the instances. Raster reads a region
 * of ElementInstance by slot, and a removed slot holds a zero matrix, which draws nothing.
 */
class InstancedMeshVisual : public ext::Visual3D<InstancedMeshVisual, IInstancedMeshVisual, IInstancedMeshInternal>
{
public:
    VELK_CLASS_UID(ClassId::Visual::InstancedMesh, "InstancedMeshVisual");

    // IVisual
    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx, const ::velk::size& bounds) override;
    aabb get_local_bounds(const ::velk::size& bounds) const override;

    // IInstancedMeshVisual
    uint32_t add_instance(const mat4& transform) override;
    uint32_t add_instances(array_view<const InstanceTransform> transforms) override;
    void set_transform(uint32_t slot, const mat4& transform) override;
    void remove_instance(uint32_t slot) override;
    uint32_t get_instance_count() const override { return live_count_; }

    // IInstancedMeshInternal
    array_view<const InstanceTransform> get_transforms() const override
    {
        return {transforms_.data(), transforms_.size()};
    }
    bool is_live(uint32_t slot) const override { return slot < live_.size() && live_[slot] != 0; }
    void flush_instances(IGpuArena& arena, const mat4& world) override;
    uint64_t instance_region_offset() const override { return region_.offset(); }
    uint64_t instance_region_size() const override { return region_.size(); }
    void take_changed_slots(vector<uint32_t>& changed, bool& all) override;

private:
    uint32_t place(const InstanceTransform& transform);
    void note_changed(uint32_t slot);
    void grow_bounds(const InstanceTransform& transform);

    vector<InstanceTransform> transforms_;
    vector<uint8_t> live_;
    vector<uint32_t> free_slots_;
    uint32_t live_count_ = 0;

    /// Slots to rewrite in the raster region, and to report to the BVH.
    vector<uint32_t> flush_slots_;
    vector<uint32_t> changed_slots_;
    bool flush_all_ = true;
    bool all_changed_ = true;

    ArenaRegion region_;
    mat4 flushed_world_{};

    /// Grows only. Instance origins, and the largest scale any transform has.
    float origin_min_[3] = {0.f, 0.f, 0.f};
    float origin_max_[3] = {0.f, 0.f, 0.f};
    float max_scale_ = 0.f;
    bool has_bounds_ = false;
};

} // namespace velk::impl

#endif // VELK_UI_INSTANCED_MESH_VISUAL_H
