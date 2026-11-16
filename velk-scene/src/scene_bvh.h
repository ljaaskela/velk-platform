#ifndef VELK_UI_SCENE_BVH_H
#define VELK_UI_SCENE_BVH_H

#include "aggregate.h"
#include "scene_collector.h"

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

    /// Brings the BVH up to date with the changes in @p state.
    void update(const SceneState& state, FrameContext& ctx);

    /// A view the scene is seen from: where it is, and how many pixels a unit covers at a unit
    /// from it, which residency weighs what partitions need by.
    struct ViewPoint
    {
        vec3 at{};
        float pixels_per_unit = 0.f;
    };
    /// Where the cameras are, for what stays resident.
    void set_view_points(array_view<const ViewPoint> points)
    {
        view_points_.clear();
        view_points_.insert(view_points_.end(), points.begin(), points.end());
    }

    /// Adds the passes that run instanced visuals' motion functions to @p graph. Comes
    /// before every pass that reads instances or records.
    void emit_instance_motion(FrameContext& ctx, IRenderGraph& graph);

    /// Adds the pass that builds the quad trees of the partitions that changed (kQuadNodeWords)
    /// to @p graph. Comes after the instance motion, whose resting matrices it reads.
    void emit_quad_trees(FrameContext& ctx, IRenderGraph& graph);


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

    uint32_t get_plain_partitions(uint32_t& count) const override
    {
        count = plain_cull_.valid() ? plain_cull_capacity_ : 0u;
        return plain_cull_.valid() ? static_cast<uint32_t>(plain_cull_.offset() / sizeof(uint32_t)) : 0xFFFFFFFFu;
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
        mat4 world{};          ///< Where it is: a change moves it.
        uint32_t object = 0;   ///< Its element's object id, 0 for none.
        uint32_t color = 0;    ///< Its shapes' colour (RGBA8), for its object's quad.
        vector<RtShape> shapes;  ///< What it is: a change in kind makes it again.
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
        };
        IInstancedMeshInternal* visual = nullptr;
        mat4 world{};
        vector<RtShape> shapes;  ///< All meshes' runs, kept to tell when they change.
        vector<MeshRun> meshes;
        vector<uint32_t> partitions;
        vector<uint32_t> slot_partition;  ///< Per slot, its index in partitions_, or kUnplaced.
        uint32_t open_partition = kUnplaced;  ///< Takes new instances until it is full.

        /// Loaded by need, the partitions that need their instances, the largest on screen
        /// last, which come in from the back.
        vector<uint32_t> pending_load;
        uint32_t resident_members = 0;
        uint32_t residency_age = 0;  ///< Updates since those were worked out.

        /// What culling reads, in the culling arena. See `instance_partition_cull_src`.
        ArenaRegion cull_table;
        uint32_t cull_table_capacity = 0;
        uint32_t cull_next_entry = 0;
        vector<uint32_t> cull_free_entries;

        /// Holds instances that moved recently. It is written again each frame it changes,
        /// so a mover costs only itself.
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
        bool motion_dirty = false;  ///< Instances were written at rest, so the motion runs again.
        bool motion_error_logged = false;
        /// Its meshes' quads (MeshQuad, kMeshQuadWords each) in the culling arena, which its
        /// partitions' quad trees are built from. Made with each cut.
        ArenaRegion quad_table;
        uint32_t quad_meshes = 0;
        vector<MeshQuad> mesh_quads;  ///< What quad_table holds, for partitions without their instances.
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
        /// Its slots in chunks, or for plain records their objects' ids.
        ArenaRegion cull_members;
        /// Its entry in the group's table, or for plain records in the plain table.
        uint32_t cull_entry = kUnplaced;
        uint32_t cull_chunks = 0;
        BvhItemBounds cull_bounds{};  ///< Its members' bounds in the world.
        bool active = false;
        bool dirty = false;
        /// Whether its instances have their GPU data. Without, it is its quad tree down to
        /// `stored_level`.
        bool resident = true;
        float score = 0.f;  ///< How large its members are on screen, as `pending_load` was worked out.
        /// Only whether it is resident changed, which leaves its bounds, so the partitions'
        /// layout stays.
        bool residency_changed = false;
        /// Its members as a tree of quads (kQuadNodeWords), in the culling arena, built on the
        /// GPU when its members change. None for a dynamic partition or one without its instances.
        ArenaRegion quad_tree;
        uint32_t quad_leaves = 0;
        bool quad_tree_dirty = false;
        /// For plain records, its objects' own quads in the world (MeshQuad each), in the
        /// order of `cull_members`, which the tree is built from. For instances without their
        /// instances, one quad per chunk of members, and the tree's leaves are the finest it has.
        ArenaRegion quad_objects;
        uint32_t quad_object_count = 0;
        bool quad_leaves_only = false;
        /// Without its instances: the level of its tree it keeps (0 its members' own quads),
        /// the coarsest the views allow. And each level's largest quad side, 0 its members, up
        /// to the root, which that is worked out from; empty until measured.
        uint32_t stored_level = 0;
        vector<float> level_sides;
    };

    struct EmitContext
    {
        FrameContext& frame;
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
    void alloc_instance_cull(InstanceGroup& group, FrameContext& ctx, uint32_t table_capacity);
    /// Brings culling's view of @p partition up to date with its members and cull_bounds.
    void write_partition_cull(Partition& partition, FrameContext& ctx);
    /// Takes @p partition out of what culling reads. For one about to be freed.
    void release_partition_cull(Partition& partition, FrameContext& ctx);
    void write_cull_entry(const Partition& partition, IGpuArena& arena, bool fresh_table);
    /// Lists a plain partition's objects for culling, in the plain table: see
    /// IBvh::get_plain_partitions.
    void write_plain_cull(Partition& partition, FrameContext& ctx, const float* lo, const float* hi);
    /// Takes a plain partition out of the plain table.
    void release_plain_cull(Partition& partition, FrameContext& ctx);
    /// Moves @p slot to its group's dynamic partition.
    void promote_instance(InstanceGroup& group, uint32_t slot);
    /// Sends instances that have been still for a while back where they came from.
    void demote_quiet_instances(InstanceGroup& group);
    bool is_instance_dynamic(uint32_t partition) const;
    /// Allocates and clears @p group's motion region, if it has a motion function.
    void alloc_instance_motion(InstanceGroup& group, FrameContext& ctx);
    BvhItemBounds instance_bounds(const InstanceGroup& group, uint32_t slot) const;
    /// @p slot's quad in the world, as the GPU build makes its leaf; a colour of 0 for none.
    MeshQuad member_quad(const InstanceGroup& group, uint32_t slot) const;
    /// The quads at @p level (0 its members) of the tree @p partition has over its members, as
    /// the GPU builds it, and with @p sides each level's largest quad side up to the root.
    vector<MeshQuad> partition_level(const InstanceGroup& group, const Partition& partition, uint32_t level,
                                     vector<float>* sides) const;
    /// The world bounds of mesh @p mesh of @p group placed by @p t, with the element at @p w.
    BvhItemBounds instance_bounds(const InstanceGroup& group, const mat4& w, const InstanceTransform& t,
                                  uint32_t mesh) const;
    void free_record(uint32_t slot);
    /// Moves a record to the dynamic partition, leaving its static partition if any.
    void promote(uint32_t slot);
    /// Sends quiet records of the dynamic partition back to static partitions.
    void demote_quiet_records();
    /// Rebuilds all static partitions from the static and unplaced records.
    void recut(FrameContext& ctx);
    /// Splits partitions grown past their limit, then uploads the changed ones.
    void flush_partitions(FrameContext& ctx);
    /// Writes @p partition's bounds and culling table entry.
    void upload_partition(Partition& partition, FrameContext& ctx);
    /// Cuts the static partitions again once they hold what they hold much more loosely than
    /// when last cut.
    void check_recut(FrameContext& ctx);
    /// Writes @p group's table of mesh quads.
    void write_quad_table(InstanceGroup& group, FrameContext& ctx);
    /// The quad tree pass, its jobs, and what it built since that was last logged.
    IRenderPass::Ptr quad_pass_;
    ArenaRegion quad_jobs_;
    uint32_t quad_jobs_capacity_ = 0;
    uint32_t quad_trees_built_ = 0;
    uint64_t quad_nodes_built_ = 0;
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
    vector<ViewPoint> view_points_;
    /// Instances all loaded-by-need visuals may have places for together: the device-wide cap,
    /// from the device's memory. And how many have places, summed over them each update.
    uint64_t resident_cap_ = 0;
    uint64_t resident_total_ = 0;
    uint32_t update_frame_ = 0;
    uint32_t live_records_ = 0;

    vector<Partition> partitions_;
    /// Plain partitions for culling, kCullEntryWords each: see IBvh::get_plain_partitions.
    ArenaRegion plain_cull_;
    uint32_t plain_cull_capacity_ = 0;
    uint32_t plain_cull_next_ = 0;
    vector<uint32_t> plain_cull_free_;
    vector<uint32_t> free_partitions_;
    Partition dynamic_;
    bool dynamic_transforms_dirty_ = false;

    /// Partitions changed their bounds, so whether to cut again is checked.
    bool layout_dirty_ = false;
    float cut_cost_ = 0.f;  ///< How loosely the partitions held their members when last cut.

    vector<uint32_t> unplaced_;       ///< Records waiting for a partition.
    vector<IElement*> unresolved_;    ///< Elements to emit again next frame.
    vector<WorldBox> changed_;        ///< See changed_bounds. Cleared by each update.
    bool changed_all_ = false;
    void note_changed(const BvhItemBounds& bounds);
    void note_all_changed();

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
