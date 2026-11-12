#ifndef VELK_UI_SCENE_BVH_H
#define VELK_UI_SCENE_BVH_H

#include "scene_collector.h"
#include "scene_lbvh.h"

#include <velk/api/velk.h>
#include <velk/ext/object.h>

#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_bvh.h>
#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/interface/intf_render_graph.h>
#include <velk-render/interface/intf_render_pass.h>
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

    /// Where the cameras are, for what stays resident.
    void set_view_points(array_view<const vec3> points)
    {
        view_points_.clear();
        view_points_.insert(view_points_.end(), points.begin(), points.end());
    }

    /// Adds the passes that run instanced visuals' motion functions to @p graph. Comes
    /// before every pass that reads instances or records.
    void emit_instance_motion(FrameContext& ctx, IRenderGraph& graph);

    /// Adds the pass that builds this frame's trees on the GPU to @p graph. Comes before
    /// every pass that traces rays.
    void emit_gpu_builds(FrameContext& ctx, IRenderGraph& graph);
    /// Forgets the builds of the frame just recorded.
    void end_frame_gpu_builds() { gpu_builds_.clear(); }

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
    string_view get_ray_motion(float& reach) const override;
    array_view<const WorldBox> changed_bounds(bool& all) const override
    {
        all = changed_all_;
        return {changed_.data(), changed_.size()};
    }

    uint32_t get_object_transforms(uint32_t& slots) const override
    {
        slots = object_transform_slots_;
        return object_transforms_word_;
    }

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
    /// at one run of shapes per mesh.
    struct InstanceGroup
    {
        struct MeshRun
        {
            uint32_t first_shape = 0;
            uint32_t shape_count = 0;
            BvhItemBounds bounds{};
            float far_area = 0.f;      ///< Mean projected area at scale 1. 0 without a far form.
            float far_albedo[3] = {1.f, 1.f, 1.f};
        };
        IInstancedMeshInternal* visual = nullptr;
        mat4 world{};
        vector<RtShape> shapes;  ///< All meshes' runs.
        ArenaRegion shape_region;
        vector<MeshRun> meshes;
        vector<uint32_t> partitions;
        vector<uint32_t> slot_partition;  ///< Per slot, its index in partitions_, or kUnplaced.
        uint32_t open_partition = kUnplaced;  ///< Takes new instances until it is full.

        /// With a residency budget, by what its partitions are worth keeping. What comes in
        /// goes last to first, and what makes room for it, last to first.
        vector<uint32_t> pending_load;
        vector<uint32_t> pending_unload;
        uint32_t resident_members = 0;
        uint32_t residency_age = 0;  ///< Updates since those were worked out.

        /// What culling reads, in the culling arena. See `instance_partition_cull_src`.
        ArenaRegion cull_table;
        uint32_t cull_table_capacity = 0;
        uint32_t cull_next_entry = 0;
        vector<uint32_t> cull_free_entries;
        ArenaRegion cull_list;
        uint32_t cull_list_capacity = 0;
        uint32_t cull_chunks = 0;  ///< In all its partitions, which the list has to hold.

        /// Holds instances that moved recently. It rebuilds each frame it changes, and
        /// joins the root rather than the top tree, so a mover costs only itself.
        uint32_t dynamic_partition = kUnplaced;
        struct Mover
        {
            uint32_t home;        ///< The partition it left, in partitions_.
            uint32_t moved_frame; ///< The update it last moved in.
        };
        std::unordered_map<uint32_t, Mover> movers;  ///< By slot.

        /// With a motion function: its table, then per slot its record and partition
        /// origin, in the culling arena. See `instance_motion_compute_src`.
        ArenaRegion motion_region;
        uint32_t motion_capacity = 0;  ///< Slots the region holds.
        float max_displacement = 0.f;  ///< Bounds are grown by this.
        uint32_t object_base = 0;      ///< Slot i's object id is this plus i.
        uint32_t object_count = 0;     ///< How many ids from object_base it has placed.
        bool motion_region_fresh = false;  ///< No frame has read it, so it can be written directly.
        bool motion_records_dirty = false;  ///< Records were written at rest.
        bool motion_error_logged = false;
        IRenderPass::Ptr motion_pass;
        bool motion_pass_dirty = true;
        uint64_t motion_rest_offset = 0;  ///< What the recorded pass was given.
        uint64_t motion_instances_offset = 0;
    };

    struct Partition
    {
        /// Record slots, or instance slots when `group` is set. Can hold stale slots
        /// until `dirty` is handled.
        vector<uint32_t> members;
        InstanceGroup* group = nullptr;  ///< Set when this partition holds instances.
        ArenaRegion cull_members;           ///< Its slots, in chunks.
        uint32_t cull_entry = kUnplaced;    ///< Its entry in the group's table.
        uint32_t cull_chunks = 0;
        BvhItemBounds cull_bounds{};
        ArenaRegion gpu_build_items;    ///< This frame's items, when the GPU builds the tree.
        ArenaRegion gpu_build_scratch;  ///< What the build works in. Kept while it is large enough.
        uint32_t gpu_build_capacity = 0;
        ArenaRegion nodes;
        ArenaRegion records;
        ArenaRegion shapes;
        ArenaRegion tree_nodes;
        GpuBvhNode ref{};  ///< World bounds, referring to its root. Copied into the tree above it.
        uint32_t node_count = 0;
        bool active = false;
        bool dirty = false;
        /// Whether its instances have their GPU data. Without, `nodes` is the top of its tree,
        /// which rays and raster take as a medium.
        bool resident = true;
        float score = 0.f;  ///< Its worth as `pending_load` was worked out.
        /// Only whether it is resident changed, which leaves its bounds, so the top tree can
        /// keep its shape, and only its node there is rewritten.
        bool residency_changed = false;
        uint32_t top_index = kUnplaced;  ///< Its node in the top tree.
    };

    struct EmitContext
    {
        FrameContext& frame;
        ShapeCb cb;
        void* user;
        IScene* scene;  ///< Hands out object ids. Can be null.
    };

    void full_build(IScene* scene, const EmitContext& emit);
    void update_element(IElement* element, const EmitContext& emit);
    void remove_element(IElement* element);
    /// Brings @p element's instance group up to date with its visual.
    void update_instances(IElement* element, IInstancedMeshInternal& visual, const EmitContext& emit);
    /// Cuts all of @p group's live instances into new partitions, by Morton order.
    void cut_instances(InstanceGroup& group, FrameContext& ctx);
    void free_instance_partitions(InstanceGroup& group);
    void update_residency(InstanceGroup& group);
    /// Makes the tables culling reads for @p group, empty, and tells its visual.
    void alloc_instance_cull(InstanceGroup& group, FrameContext& ctx, uint32_t table_capacity, uint32_t list_capacity);
    /// Brings culling's view of @p partition up to date with its members and cull_bounds.
    void write_partition_cull(Partition& partition, FrameContext& ctx);
    /// Takes @p partition out of what culling reads. For one about to be freed.
    void release_partition_cull(Partition& partition, FrameContext& ctx);
    void write_cull_entry(const Partition& partition, IGpuArena& arena, bool fresh_table);
    /// Moves @p slot to its group's dynamic partition.
    void promote_instance(InstanceGroup& group, uint32_t slot);
    /// Sends instances that have been still for a while back where they came from.
    void demote_quiet_instances(InstanceGroup& group);
    bool is_instance_dynamic(uint32_t partition) const;
    /// Allocates and clears @p group's motion region, if it has a motion function.
    void alloc_instance_motion(InstanceGroup& group, FrameContext& ctx);
    /// Tells the motion pass where @p slot's record is, or that it has none.
    void write_motion_slot(InstanceGroup& group, FrameContext& ctx, uint32_t slot, uint32_t record,
                           const float origin[3]);
    BvhItemBounds instance_bounds(const InstanceGroup& group, uint32_t slot) const;
    /// The world bounds of mesh @p mesh of @p group placed by @p t, with the element at @p w.
    BvhItemBounds instance_bounds(const InstanceGroup& group, const mat4& w, const InstanceTransform& t,
                                  uint32_t mesh) const;
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
    /// Queues a build of @p partition's tree on the GPU, over @p items in the order of
    /// its records, which start at @p record_base.
    bool queue_gpu_build(Partition& partition, FrameContext& ctx, array_view<const BvhItemBounds> items,
                         array_view<const float> areas, array_view<const float> albedos, const float origin[3],
                         uint32_t node_base, uint32_t record_base);
    void rebuild_top_tree(FrameContext& ctx);
    void upload_root(FrameContext& ctx);
    uint32_t alloc_partition();
    /// Moves the probe volume only if @p bounds left it or are much smaller, since
    /// moving it discards probe history.
    vector<Record> records_;
    vector<uint32_t> free_records_;
    /// Slots freed this frame. They join free_records_ once every partition has
    /// dropped them.
    vector<uint32_t> freed_records_;
    std::unordered_map<IElement*, vector<uint32_t>> element_records_;
    std::unordered_map<IElement*, InstanceGroup> instance_groups_;
    vector<vec3> view_points_;
    uint32_t next_medium_id_ = 1;
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
    vector<LbvhPush> gpu_builds_;     ///< Trees to build on the GPU this frame.
    IRenderPass::Ptr gpu_build_pass_;
    vector<IElement*> unresolved_;    ///< Elements to emit again next frame.
    vector<WorldBox> changed_;        ///< See changed_bounds. Cleared by each update.
    bool changed_all_ = false;
    void note_changed(const BvhItemBounds& bounds);
    void note_all_changed();
    bool emit_unresolved_ = false;    ///< Set while emitting if the element has an unresolved shape.

    bool needs_full_build_ = true;

    /// Each object's place, by the slot of its id: see get_object_transforms. A mirror of
    /// what is in the culling arena, and the slots changed since it was last uploaded.
    vector<uint32_t> object_transforms_;
    uint32_t transforms_dirty_first_ = 0xFFFFFFFFu;
    uint32_t transforms_dirty_last_ = 0;
    bool transforms_all_dirty_ = false;
    ArenaRegion object_transforms_region_;
    uint32_t object_transforms_word_ = 0xFFFFFFFFu;
    uint32_t object_transform_slots_ = 0;
    /// Notes where object @p object is: @p world, its object space to the world, and
    /// @p radius, the radius of its bounds in world units.
    void set_object_transform(uint32_t object, const mat4& world, float radius);
    /// Forgets where @p count objects from @p object are.
    void clear_object_transforms(uint32_t object, uint32_t count);
    /// Writes the instances' places, every slot or only @p slots.
    void set_instance_transforms(const InstanceGroup& group, const vector<uint32_t>* slots);
    /// Uploads what changed to the culling arena.
    void flush_object_transforms(FrameContext& ctx);
};

} // namespace impl

} // namespace velk

#endif // VELK_UI_SCENE_BVH_H
