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
 * An instanced visual's instances skip the records but are members of the same partitions,
 * each by itself, so that a partition holds whatever is near, of any visual. A moved
 * instance goes to the dynamic partition as records do, and so do all of a moved
 * element's instances, placed again past a limit.
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

    /// An element's instances. They have no Records: each instance is a member of the scene's
    /// partitions by itself (see kMemberInstance), and every GPU record is computed from the
    /// visual's transforms and points at one run of shapes per mesh.
    struct InstanceGroup
    {
        struct MeshRun
        {
            uint32_t first_shape = 0;
            uint32_t shape_count = 0;
            BvhItemBounds bounds{};
        };
        IInstancedMeshInternal* visual = nullptr;
        uint32_t id = kUnplaced;  ///< Its index in groups_, which its members carry.
        mat4 world{};
        vector<RtShape> shapes;  ///< All meshes' runs, kept to tell when they change.
        vector<MeshRun> meshes;
        /// Per slot, its index in partitions_, kDynamic or kUnplaced.
        vector<uint32_t> slot_partition;
        /// Loaded by need, how many of its instances have places in the raster region.
        uint32_t resident_members = 0;

        /// Its slot table for culling (see IInstancedMeshInternal::set_slot_table): each slot's
        /// place in the raster region, written again when places change: all of it, or the
        /// slots from slot_dirty_from up to slot_dirty_to.
        ArenaRegion slot_table;
        uint32_t slot_table_slots = 0;
        bool slot_table_dirty = true;
        uint32_t slot_dirty_from = 0xFFFFFFFFu;
        uint32_t slot_dirty_to = 0;
        vector<uint32_t> slot_table_copy;  ///< What slot_table holds, kept to upload a range from.
        vector<uint32_t> dirty_slots;      ///< Slots whose places may have changed since it was written.
        /// @p slot's place may have changed: its part of the slot table is written again.
        void mark_slot(uint32_t slot)
        {
            slot_dirty_from = slot < slot_dirty_from ? slot : slot_dirty_from;
            slot_dirty_to = slot + 1u > slot_dirty_to ? slot + 1u : slot_dirty_to;
            dirty_slots.push_back(slot);
        }

        /// The update each instance in the dynamic partition last moved in, by slot.
        std::unordered_map<uint32_t, uint32_t> movers;

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
        /// Its meshes' quads, which its instances' quads are placed from (member_quad).
        vector<MeshQuad> mesh_quads;
        IRenderPass::Ptr motion_pass;
        bool motion_pass_dirty = true;
        uint64_t motion_rest_offset = 0;  ///< What the recorded pass was given.
        uint64_t motion_instances_offset = 0;
    };

    /// A member of a partition: a plain record's slot, or with kMemberInstance an instance,
    /// its group's id above kMemberSlotBits and its slot below.
    static constexpr uint32_t kMemberInstance = 0x80000000u;
    static constexpr uint32_t kMemberSlotBits = 21;
    static constexpr uint32_t kMemberSlotMask = (1u << kMemberSlotBits) - 1u;
    static constexpr uint32_t kMaxGroups = 1u << (31 - kMemberSlotBits);

    /// A part of the scene in which every member is near the others, plain objects and
    /// instances of any visual alike, which culling keeps a cut of and rays walk as one.
    struct Partition
    {
        /// Its members (see kMemberInstance). Can hold stale ones until `dirty` is handled.
        vector<uint32_t> members;
        /// Its objects' ids in chunks of kCullChunkSlots, or without its instances the indices
        /// of its quads.
        ArenaRegion cull_members;
        /// Its entry in the scene's table (plain_cull_).
        uint32_t cull_entry = kUnplaced;
        uint32_t cull_chunks = 0;
        BvhItemBounds cull_bounds{};  ///< Its members' bounds in the world.
        /// The surface area of its bounds when its members were last cut, against which drift is
        /// measured.
        float cut_area = 0.f;
        bool active = false;
        bool dirty = false;
        /// Whether its members are drawn as themselves. Without, it is its quad tree down to
        /// `stored_level`, plain objects and instances alike, and its instances of visuals
        /// loaded by need give up their places.
        bool resident = true;
        float score = 0.f;  ///< How large its members are on screen, as residency last worked out.
        /// Only whether it is resident changed, which leaves its bounds, so the partitions'
        /// layout stays.
        bool residency_changed = false;
        /// Its members as a tree of quads (kQuadNodeWords), in the culling arena, built on the
        /// GPU when its members change. None for the dynamic partition.
        ArenaRegion quad_tree;
        uint32_t quad_leaves = 0;
        bool quad_tree_dirty = false;
        /// Its objects' own quads in the world (MeshQuad each), in the order of `cull_members`,
        /// which the tree is built from: a plain object's across its bounds, an instance's its
        /// mesh's placed by its resting matrix. Without its instances, its tree's quads at
        /// `stored_level`, and the tree's leaves are the finest it has.
        ArenaRegion quad_objects;
        uint32_t quad_object_count = 0;
        bool quad_leaves_only = false;
        /// Without its instances: the level of its tree it keeps (0 its members' own quads),
        /// the coarsest the views allow. And each level's largest quad side, 0 its members, up
        /// to the root, which that is worked out from; empty until measured.
        uint32_t stored_level = 0;
        vector<float> level_sides;
        /// Its instances of visuals loaded by need, a count per group id, as last uploaded: the
        /// places it takes when it keeps its members.
        struct GroupCount
        {
            uint32_t group;
            uint32_t count;
        };
        vector<GroupCount> by_need;
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
    /// Takes every instance of @p group out of the partitions.
    void remove_group_members(InstanceGroup& group);
    /// Places every live instance of @p group in the partitions: a few by insertion, many cut
    /// into partitions of their own.
    void place_group(InstanceGroup& group);
    /// Works out, now and then, which partitions keep their members and which only their quads,
    /// and loads a few of those that need their members back.
    void update_residency();

    /// What a member is: its group, or null for a plain record, and its slot.
    InstanceGroup* member_group(uint32_t member) const;
    static uint32_t member_slot(uint32_t member) { return member & ((member & kMemberInstance) != 0u ? kMemberSlotMask : ~0u); }
    /// The member of instance @p slot of @p group.
    static uint32_t instance_member(const InstanceGroup& group, uint32_t slot)
    {
        return kMemberInstance | (group.id << kMemberSlotBits) | slot;
    }
    /// Where @p member is: its partition's index in partitions_, kDynamic or kUnplaced.
    uint32_t member_partition(uint32_t member) const;
    void set_member_partition(uint32_t member, uint32_t partition);
    /// Whether @p member is still something: a live record or a live instance.
    bool member_live(uint32_t member) const;
    BvhItemBounds member_bounds(uint32_t member) const;
    /// Its object id, 0 for none.
    uint32_t member_object(uint32_t member) const;

    /// Brings culling's view of @p partition up to date with its members and cull_bounds.
    void write_cull(Partition& partition, FrameContext& ctx);
    /// Takes @p partition out of what culling reads. For one about to be freed.
    void release_cull(Partition& partition, FrameContext& ctx);
    /// Writes @p partition's entry in the scene's table, @p fresh when nothing has read the table.
    void write_cull_entry(const Partition& partition, IGpuArena& arena, bool fresh);
    /// Writes each group's slot table where places changed.
    void write_slot_tables(FrameContext& ctx);
    /// Allocates and clears @p group's motion region, if it has a motion function.
    void alloc_instance_motion(InstanceGroup& group, FrameContext& ctx);
    BvhItemBounds instance_bounds(const InstanceGroup& group, uint32_t slot) const;
    /// @p slot's quad in the world, as the GPU build makes its leaf; a colour of 0 for none.
    MeshQuad member_quad(const InstanceGroup& group, uint32_t slot) const;
    /// The quads at @p level (0 its objects') of the tree @p partition has over its objects, as
    /// the GPU builds it, and with @p sides each level's largest quad side up to the root.
    vector<MeshQuad> partition_level(const Partition& partition, uint32_t level, vector<float>* sides) const;
    /// @p partition's objects, near to near in the world, and each one's quad: an instance by
    /// itself, a plain object over all its records.
    void partition_objects(const Partition& partition, vector<uint32_t>& objects, vector<MeshQuad>& quads) const;
    /// The world bounds of mesh @p mesh of @p group placed by @p t, with the element at @p w.
    BvhItemBounds instance_bounds(const InstanceGroup& group, const mat4& w, const InstanceTransform& t,
                                  uint32_t mesh) const;
    void free_record(uint32_t slot);
    /// Moves @p member to the dynamic partition, leaving its partition if any.
    void promote(uint32_t member);
    /// Sends the dynamic partition's members that have been still for a while to partitions.
    void demote_quiet();
    /// Puts @p member in the partition whose bounds grow least by it.
    void insert_member(uint32_t member);
    /// Cuts @p members into new partitions by a tree over their bounds.
    void cut_members(const vector<uint32_t>& members);
    /// Cuts all members again, at a full build or a load large against the scene.
    void recut(FrameContext& ctx);
    /// Splits partitions grown past their limit, then uploads the changed ones.
    void flush_partitions(FrameContext& ctx);
    /// Writes @p partition's bounds and culling table entry.
    void upload_partition(Partition& partition, FrameContext& ctx);
    /// Cuts again the partitions whose bounds have drifted far from where they were cut, their
    /// members together.
    void check_recut();
    /// Writes @p group's table of mesh quads.
    void write_quad_table(InstanceGroup& group);
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
    /// The groups by id (InstanceGroup::id), null where free. An id freed in an update is given
    /// out again only after it, once no partition can still hold a member that carries it.
    vector<InstanceGroup*> groups_;
    vector<uint32_t> retired_groups_;
    vector<ViewPoint> view_points_;
    /// The partitions whose members come in ahead of need, the largest on screen last, which
    /// come in from the back, a few an update.
    vector<uint32_t> pending_load_;
    /// Instances all loaded-by-need visuals may have places for together: the device-wide cap,
    /// from the device's memory. And how many have places, summed over them each update.
    uint64_t resident_cap_ = 0;
    uint64_t resident_total_ = 0;
    uint32_t update_frame_ = 0;
    uint32_t live_records_ = 0;

    vector<Partition> partitions_;
    /// The partitions for culling, kCullEntryWords each: see IBvh::get_plain_partitions.
    ArenaRegion plain_cull_;
    uint32_t plain_cull_capacity_ = 0;
    uint32_t plain_cull_next_ = 0;
    vector<uint32_t> plain_cull_free_;
    vector<uint32_t> free_partitions_;
    Partition dynamic_;
    bool dynamic_transforms_dirty_ = false;

    /// Partitions changed their bounds, so whether to cut again is checked.
    bool layout_dirty_ = false;

    vector<uint32_t> unplaced_;       ///< Members waiting for a partition.
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
