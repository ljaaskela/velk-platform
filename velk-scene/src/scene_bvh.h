#ifndef VELK_UI_SCENE_BVH_H
#define VELK_UI_SCENE_BVH_H

#include "scene_collector.h"

#include <velk/api/velk.h>
#include <velk/ext/object.h>

#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_bvh.h>
#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-scene/plugin.h>

#include <unordered_map>

namespace velk {

/**
 * @brief IBvh implementation over an IScene's records.
 *
 * Attached to the scene root on first use. Each group of shapes is a record, kept
 * in a static partition or in the dynamic partition. A static partition owns its
 * tree and its record, shape and local tree regions, all relative to its own origin,
 * and a top tree over the partitions joins them in world space. Records that change
 * move to the dynamic partition, which
 * rebuilds each frame it changes, and return to a static partition once they have
 * been still for a while. With a stable set of movers, the per-frame cost depends
 * only on the movers.
 *
 * An instanced visual's instances skip the records and sit in partitions of their
 * own, which join the same top tree. A moved instance goes to a dynamic partition of
 * its element, as records do, and a translated element only moves its partitions'
 * origins.
 *
 * All node, record and shape indices on the GPU are absolute in their arenas.
 */
namespace impl {

class SceneBvh : public ::velk::ext::ObjectCore<SceneBvh, ::velk::IBvh>
{
public:
    VELK_CLASS_UID(::velk::ClassId::SceneBvh, "SceneBvh");

    SceneBvh() = default;

    /**
     * @brief Brings the BVH up to date with the changes in @p state.
     *
     * The callback fires per emitted shape of each changed element so the caller
     * can fill material / texture / intersect fields. A shape whose texture is not
     * resolved yet is emitted again each frame until it is.
     */
    void update(const SceneState& state, FrameContext& ctx, ShapeCb shape_cb, void* shape_user);

    // IBvh
    bool any_hit(vec3 /*origin*/, vec3 /*dir*/, float /*t_max*/) const override
    {
        // TODO(m-later): wire CPU-side traversal so app-side pickers / collision
        // consumers can query without going through GPU. Stub until then.
        return false;
    }

    bool closest_hit(vec3 /*origin*/, vec3 /*dir*/, BvhHit& /*out*/) const override
    {
        return false;
    }

    uint32_t get_root_index() const override { return root_; }
    uint32_t get_node_count() const override { return node_count_; }
    ProbeVolume get_probe_volume() const override { return probe_volume_; }

    void invalidate() override { needs_full_build_ = true; }

private:
    static constexpr uint32_t kUnplaced = 0xFFFFFFFFu;
    static constexpr uint32_t kDynamic = 0xFFFFFFFEu;

    /// One group of shapes in one local space. A slot in records_ is stable for the
    /// record's lifetime.
    struct Record
    {
        IElement* owner = nullptr;  ///< Null when the slot is free. Never dereferenced.
        /// Its first_shape and tree_root are absolute once its partition uploads. Its
        /// translation is in world space; the upload makes it relative to the partition.
        RtRecord gpu{};
        vector<RtShape> shapes;
        /// Local tree, when the record has many shapes. Indices relative to this
        /// vector and to `shapes`.
        vector<BlasNode> tree;
        BvhItemBounds bounds{};
        uint32_t partition = kUnplaced;  ///< Index into partitions_, or kDynamic / kUnplaced.
        uint32_t quiet_frames = 0;       ///< Frames since it last moved, in the dynamic partition.
    };

    /// An element's instances. They have no Records: their partitions hold instance
    /// slots, and every GPU record is computed from the visual's transforms and points
    /// at this one run of shapes.
    struct InstanceGroup
    {
        IInstancedMeshInternal* visual = nullptr;
        mat4 world{};
        vector<RtShape> shapes;
        ArenaRegion shape_region;
        uint32_t first_shape = 0;
        BvhItemBounds local_bounds{};  ///< Of the mesh.
        vector<uint32_t> partitions;
        vector<uint32_t> slot_partition;  ///< Per slot, its index in partitions_, or kUnplaced.
        uint32_t open_partition = kUnplaced;  ///< Takes new instances until it is full.

        /// Holds instances that moved recently. It rebuilds each frame it changes, and
        /// joins the root rather than the top tree, so a mover costs only itself.
        uint32_t dynamic_partition = kUnplaced;
        struct Mover
        {
            uint32_t home;        ///< The partition it left, in partitions_.
            uint32_t moved_frame; ///< The update it last moved in.
        };
        std::unordered_map<uint32_t, Mover> movers;  ///< By slot.
    };

    struct Partition
    {
        /// Record slots, or instance slots when `group` is set. Can hold stale slots
        /// until `dirty` is handled.
        vector<uint32_t> members;
        InstanceGroup* group = nullptr;  ///< Set when this partition holds instances.
        ArenaRegion nodes;
        ArenaRegion records;
        ArenaRegion shapes;
        ArenaRegion tree_nodes;
        GpuBvhNode ref{};  ///< World bounds, referring to its root. Copied into the tree above it.
        uint32_t node_count = 0;
        bool active = false;
        bool dirty = false;
    };

    struct EmitContext
    {
        FrameContext& frame;
        ShapeCb cb;
        void* user;
    };

    void full_build(IScene* scene, const EmitContext& emit);
    void update_element(IElement* element, const EmitContext& emit);
    void remove_element(IElement* element);
    /// Brings @p element's instance group up to date with its visual.
    void update_instances(IElement* element, IInstancedMeshInternal& visual, const EmitContext& emit);
    /// Cuts all of @p group's live instances into new partitions, by Morton order.
    void cut_instances(InstanceGroup& group, FrameContext& ctx);
    void free_instance_partitions(InstanceGroup& group);
    /// Moves @p slot to its group's dynamic partition.
    void promote_instance(InstanceGroup& group, uint32_t slot);
    /// Sends instances that have been still for a while back where they came from.
    void demote_quiet_instances(InstanceGroup& group);
    bool is_instance_dynamic(uint32_t partition) const;
    BvhItemBounds instance_bounds(const InstanceGroup& group, uint32_t slot) const;
    RtRecord instance_record(const InstanceGroup& group, uint32_t slot) const;
    void free_record(uint32_t slot);
    /// Moves a record to the dynamic partition, leaving its static partition if any.
    void promote(uint32_t slot);
    /// Sends quiet records of the dynamic partition back to static partitions.
    void demote_quiet_records();
    /// Rebuilds all static partitions from the static and unplaced records.
    void recut(FrameContext& ctx);
    /// Splits partitions grown past their limit, then uploads the changed ones.
    void flush_partitions(FrameContext& ctx);
    /// Uploads @p partition. With @p geometry, also its shapes and local trees.
    void upload_partition(Partition& partition, FrameContext& ctx, bool geometry);
    void rebuild_top_tree(FrameContext& ctx);
    void upload_root(FrameContext& ctx);
    uint32_t alloc_partition();
    /// Moves the probe volume only if @p bounds left it or are much smaller, since
    /// moving it discards probe history.
    void update_probe_volume(const float bounds_min[3], const float bounds_max[3], uint32_t frame);

    vector<Record> records_;
    vector<uint32_t> free_records_;
    /// Slots freed this frame. They join free_records_ once every partition has
    /// dropped them.
    vector<uint32_t> freed_records_;
    std::unordered_map<IElement*, vector<uint32_t>> element_records_;
    std::unordered_map<IElement*, InstanceGroup> instance_groups_;
    uint32_t update_frame_ = 0;
    uint32_t live_records_ = 0;

    vector<Partition> partitions_;
    vector<uint32_t> free_partitions_;
    Partition dynamic_;
    bool dynamic_transforms_dirty_ = false;

    ArenaRegion top_nodes_;
    GpuBvhNode top_root_{};
    uint32_t top_node_count_ = 0;
    bool has_top_ = false;
    bool top_dirty_ = false;
    float cut_cost_ = 0.f;  ///< The top tree's SAH cost when last cut.

    ArenaRegion root_nodes_;
    uint32_t root_ = 0;
    uint32_t node_count_ = 0;
    bool root_dirty_ = true;

    vector<uint32_t> unplaced_;       ///< Records waiting for a partition.
    vector<uint32_t> shape_updates_;  ///< Records whose shapes changed but not their geometry.
    vector<IElement*> unresolved_;    ///< Elements to emit again next frame.
    bool emit_unresolved_ = false;    ///< Set while emitting if the element has an unresolved shape.

    ProbeVolume probe_volume_;
    bool probe_volume_placed_ = false;
    bool needs_full_build_ = true;
};

} // namespace impl

} // namespace velk

#endif // VELK_UI_SCENE_BVH_H
