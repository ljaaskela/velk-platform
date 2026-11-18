#include "scene_bvh.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-scene/interface/intf_element.h>
#include <velk-scene/interface/intf_visual.h>

#include <velk-render/api/cached_view_pass.h>
#include <velk-render/frame/instance_cull.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/plugin.h>
#include <velk-scene/instance_types.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace velk::impl {

namespace {

/// Records in a static partition when it is cut. A partition rebuild is bounded by
/// twice this.
constexpr uint32_t kPartitionRecords = 1024;
/// A partition's instances come in ahead of need once its members' quads are over this share of
/// kLodElementPixels on screen, a few an update, so that they are in before they are needed.
constexpr float kResidencyAheadShare = 0.5f;
/// Most partitions coming in ahead of need per update for each visual loaded by need, since each
/// builds a tree. Those already needed come in at once, past it.
constexpr uint32_t kMaxResidencyChanges = 8;
/// How much smaller on screen than the size that brings them in a resident partition's members
/// have to be before its instances go, so that one at the edge does not come and go.
constexpr float kResidencySwapGain = 1.25f;
/// What an instance with a place uses on the GPU: its raster data and what culling keeps for
/// it. The device-wide cap counts by it.
constexpr uint64_t kResidentInstanceBytes = sizeof(ElementInstance) + 64u;
/// Frames a record of the dynamic partition stays still before it returns to a
/// static partition.
constexpr uint32_t kQuietFrames = 120;

/// Where @p center is along a curve that keeps near points near, within the box @p lo to
/// @p hi: sorting by it orders things near to near. One grid scale on all axes, so that a flat
/// spread still gets cells that are square.
uint64_t morton_key(const float* center, const float* lo, const float* hi)
{
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
    const auto cell = [&](int k) {
        return static_cast<uint64_t>(std::clamp((center[k] - lo[k]) * scale, 0.f, 2097151.f));
    };
    return spread(cell(0)) | spread(cell(1)) << 1 | spread(cell(2)) << 2;
}
/// A partition is cut again, with the others drifted as far, once its bounds' surface area grows
/// past this factor of what it was when its members were cut.
constexpr float kRecutDrift = 1.5f;
/// A partition with at least this many items gets its tree built on the GPU.
constexpr size_t kGpuBuildMinItems = 4096;
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

/// Whether two shapes describe the same geometry, ignoring color and material.
bool same_geometry(const RtShape& a, const RtShape& b)
{
    // A mesh shape's geometry fields are only its bounds, which the record's bounds follow,
    // and which a deforming mesh changes without becoming other geometry.
    if (a.shape_kind == kRtShapeKindMesh && b.shape_kind == kRtShapeKindMesh) {
        return true;
    }
    return std::memcmp(a.origin, b.origin, sizeof(a.origin)) == 0
        && std::memcmp(a.u_axis, b.u_axis, sizeof(a.u_axis)) == 0
        && std::memcmp(a.v_axis, b.v_axis, sizeof(a.v_axis)) == 0
        && std::memcmp(a.w_axis, b.w_axis, sizeof(a.w_axis)) == 0
        && std::memcmp(a.params, b.params, sizeof(a.params)) == 0
        && a.shape_kind == b.shape_kind;
}

/// Shapes emitted for one record, before they are compared with what the slot holds.
struct EmittedRecord
{
    mat4 world{};
    uint32_t object = 0;
    vector<RtShape> shapes;
    BvhItemBounds bounds{};
    float color_sum[3] = {0.f, 0.f, 0.f};  ///< Its shapes' colours added up, for its quad.
};

/// What a shape adds to its record's colour: a mesh's materials' colour, or the shape's own,
/// times its tint.
void add_shape_color(EmittedRecord& record, const ShapeSite& site)
{
    float rgb[3] = {site.geometry.color[0], site.geometry.color[1], site.geometry.color[2]};
    if (site.visual) {
        if (auto state = read_state<IVisual3D>(site.visual)) {
            if (auto mesh = state->mesh.template get<IMesh>()) {
                const uint32_t c = mesh_color(*mesh);
                for (int k = 0; k < 3; ++k) {
                    rgb[k] *= static_cast<float>((c >> (k * 8)) & 0xFFu) / 255.f;
                }
            }
        }
    }
    for (int k = 0; k < 3; ++k) {
        record.color_sum[k] += rgb[k];
    }
}

uint32_t record_color(const EmittedRecord& record)
{
    const float count = static_cast<float>(std::max<size_t>(record.shapes.size(), 1u));
    uint32_t packed = 255u << 24;
    for (int k = 0; k < 3; ++k) {
        const float unit = std::min(std::max(record.color_sum[k] / count, 0.f), 1.f);
        packed |= static_cast<uint32_t>(unit * 255.f + 0.5f) << (k * 8);
    }
    return packed;
}

float bounds_radius(const BvhItemBounds& b)
{
    float r2 = 0.f;
    for (int k = 0; k < 3; ++k) {
        const float half = 0.5f * (b.max[k] - b.min[k]);
        r2 += half * half;
    }
    return std::sqrt(r2);
}

// The radius of the box around all of an element's records, in world units.
float records_radius(const vector<EmittedRecord>& records)
{
    if (records.empty()) {
        return 0.f;
    }
    BvhItemBounds all = records[0].bounds;
    for (const auto& record : records) {
        for (int k = 0; k < 3; ++k) {
            all.min[k] = std::min(all.min[k], record.bounds.min[k]);
            all.max[k] = std::max(all.max[k], record.bounds.max[k]);
        }
    }
    return bounds_radius(all);
}

} // namespace

namespace {

/// The quads at @p level (0 the leaves') of the tree the GPU builds over @p leaves (padded to
/// its leaf count), merged in pairs as it merges them, and with @p sides each level's largest
/// quad side up to the root.
vector<MeshQuad> quad_levels(vector<MeshQuad> quads, uint32_t level, vector<float>* sides)
{
    const auto largest_side = [](const vector<MeshQuad>& level_quads) {
        float side = 0.f;
        for (const MeshQuad& q : level_quads) {
            if (q.color != 0u) {
                const float lu = std::sqrt(q.u[0] * q.u[0] + q.u[1] * q.u[1] + q.u[2] * q.u[2]);
                const float lv = std::sqrt(q.v[0] * q.v[0] + q.v[1] * q.v[1] + q.v[2] * q.v[2]);
                side = std::max(side, 2.f * std::max(lu, lv));
            }
        }
        return side;
    };
    vector<MeshQuad> wanted;
    if (sides) {
        sides->clear();
    }
    for (uint32_t at = 0;; ++at) {
        if (sides) {
            sides->push_back(largest_side(quads));
        }
        if (at == level) {
            wanted = quads;
        }
        if (quads.size() <= 1 || (at >= level && !sides)) {
            break;
        }
        quads = merge_pairs(quads);
    }
    return wanted;
}

} // namespace

vector<MeshQuad> SceneBvh::partition_level(const Partition& partition, uint32_t level, vector<float>* sides) const
{
    // Its objects' quads in the world as the GPU build makes its leaves, padded as its tree is.
    vector<uint32_t> objects;
    vector<MeshQuad> quads;
    partition_objects(partition, objects, quads);
    const auto chunks = static_cast<uint32_t>((quads.size() + kCullChunkSlots - 1u) / kCullChunkSlots);
    quads.resize(quad_tree_leaves(std::max(chunks, 1u) * kCullChunkSlots));
    return quad_levels(std::move(quads), level, sides);
}

void SceneBvh::partition_objects(const Partition& partition, vector<uint32_t>& objects, vector<MeshQuad>& quads) const
{
    objects.clear();
    quads.clear();
    const float* lo = partition.cull_bounds.min;
    const float* hi = partition.cull_bounds.max;
    // Its objects, each once (an element's records are one object), near to near in the world,
    // which the tree rays walk over the records chosen for them is built in.
    struct Keyed
    {
        uint64_t key;
        uint32_t object;
        uint32_t member;
    };
    vector<Keyed> keyed;
    keyed.reserve(partition.members.size());
    for (uint32_t member : partition.members) {
        const uint32_t object = member_object(member);
        if (object == 0u || !member_live(member)) {
            continue;
        }
        const BvhItemBounds b = member_bounds(member);
        const float center[3] = {0.5f * (b.min[0] + b.max[0]), 0.5f * (b.min[1] + b.max[1]), 0.5f * (b.min[2] + b.max[2])};
        keyed.push_back({morton_key(center, lo, hi), object, member});
    }
    // Each object where its first member is along the curve.
    std::sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
        return a.object != b.object ? a.object < b.object : a.key < b.key;
    });
    // A plain object's quad across all its records' bounds, in their colours; an instance's its
    // mesh's, placed.
    struct ObjectLook
    {
        uint32_t object = 0;
        uint64_t key = 0;
        float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        float hi[3] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                       -std::numeric_limits<float>::max()};
        float color[3] = {0.f, 0.f, 0.f};
        float weight = 0.f;
        uint32_t instance = kUnplaced;  ///< The member, for an instance.
    };
    vector<ObjectLook> looks;
    for (const Keyed& k : keyed) {
        if (looks.empty() || looks.back().object != k.object) {
            looks.emplace_back();
            looks.back().object = k.object;
            looks.back().key = k.key;
        }
        ObjectLook& look = looks.back();
        if ((k.member & kMemberInstance) != 0u) {
            look.instance = k.member;
            continue;
        }
        const Record& record = records_[k.member];
        merge_bounds(look.lo, look.hi, record.bounds.min, record.bounds.max);
        for (int c = 0; c < 3; ++c) {
            look.color[c] += static_cast<float>((record.color >> (c * 8)) & 0xFFu) / 255.f;
        }
        look.weight += 1.f;
    }
    std::sort(looks.begin(), looks.end(), [](const ObjectLook& a, const ObjectLook& b) {
        return a.key != b.key ? a.key < b.key : a.object < b.object;
    });
    objects.reserve(looks.size());
    quads.reserve(looks.size());
    for (const ObjectLook& look : looks) {
        objects.push_back(look.object);
        if (look.instance != kUnplaced) {
            const InstanceGroup* group = member_group(look.instance);
            quads.push_back(group ? member_quad(*group, member_slot(look.instance)) : MeshQuad{});
            continue;
        }
        uint32_t color = 255u << 24;
        for (int c = 0; c < 3; ++c) {
            const float unit = look.weight > 0.f ? look.color[c] / look.weight : 1.f;
            color |= static_cast<uint32_t>(std::min(std::max(unit, 0.f), 1.f) * 255.f + 0.5f) << (c * 8);
        }
        quads.push_back(bounds_quad(look.lo, look.hi, color));
    }
}

SceneBvh::InstanceGroup* SceneBvh::member_group(uint32_t member) const
{
    if ((member & kMemberInstance) == 0u) {
        return nullptr;
    }
    const uint32_t id = (member & ~kMemberInstance) >> kMemberSlotBits;
    return id < groups_.size() ? groups_[id] : nullptr;
}

uint32_t SceneBvh::member_partition(uint32_t member) const
{
    if ((member & kMemberInstance) == 0u) {
        return member < records_.size() ? records_[member].partition : kUnplaced;
    }
    const InstanceGroup* group = member_group(member);
    const uint32_t slot = member_slot(member);
    return group && slot < group->slot_partition.size() ? group->slot_partition[slot] : kUnplaced;
}

void SceneBvh::set_member_partition(uint32_t member, uint32_t partition)
{
    if ((member & kMemberInstance) == 0u) {
        records_[member].partition = partition;
        return;
    }
    InstanceGroup* group = member_group(member);
    const uint32_t slot = member_slot(member);
    if (group && slot < group->slot_partition.size()) {
        group->slot_partition[slot] = partition;
    }
}

bool SceneBvh::member_live(uint32_t member) const
{
    if ((member & kMemberInstance) == 0u) {
        return member < records_.size() && records_[member].owner != nullptr;
    }
    const InstanceGroup* group = member_group(member);
    const uint32_t slot = member_slot(member);
    return group && slot < group->visual->get_transforms().size() && group->visual->is_live(slot);
}

BvhItemBounds SceneBvh::member_bounds(uint32_t member) const
{
    if ((member & kMemberInstance) == 0u) {
        return records_[member].bounds;
    }
    const InstanceGroup* group = member_group(member);
    return group ? instance_bounds(*group, member_slot(member)) : BvhItemBounds{};
}

uint32_t SceneBvh::member_object(uint32_t member) const
{
    if ((member & kMemberInstance) == 0u) {
        return records_[member].object;
    }
    const InstanceGroup* group = member_group(member);
    return group && group->object_base != 0u ? group->object_base + member_slot(member) : 0u;
}

MeshQuad SceneBvh::member_quad(const InstanceGroup& group, uint32_t slot) const
{
    MeshQuad leaf;
    const auto transforms = group.visual->get_transforms();
    const mat4& w = group.world;
    {
        const uint32_t mesh = group.visual->get_slot_mesh(slot);
        if (slot >= transforms.size() || !group.visual->is_live(slot) || mesh >= group.mesh_quads.size()
            || group.mesh_quads[mesh].color == 0u) {
            return leaf;
        }
        const MeshQuad& local = group.mesh_quads[mesh];
        const InstanceTransform& t = transforms[slot];
        // The instance's matrix, then the element's: a point and two directions.
        float c[3];
        float u[3];
        float v[3];
        for (int row = 0; row < 3; ++row) {
            float pc = t.rows[row][3];
            float pu = 0.f;
            float pv = 0.f;
            for (int col = 0; col < 3; ++col) {
                pc += t.rows[row][col] * local.center[col];
                pu += t.rows[row][col] * local.u[col];
                pv += t.rows[row][col] * local.v[col];
            }
            c[row] = pc;
            u[row] = pu;
            v[row] = pv;
        }
        float wc[3];
        float wu[3];
        float wv[3];
        for (int row = 0; row < 3; ++row) {
            wc[row] = w(row, 3);
            wu[row] = 0.f;
            wv[row] = 0.f;
            for (int col = 0; col < 3; ++col) {
                wc[row] += w(row, col) * c[col];
                wu[row] += w(row, col) * u[col];
                wv[row] += w(row, col) * v[col];
            }
        }
        std::memcpy(leaf.center, wc, sizeof(wc));
        std::memcpy(leaf.u, wu, sizeof(wu));
        std::memcpy(leaf.v, wv, sizeof(wv));
        leaf.color = local.color;
    }
    return leaf;
}

BvhItemBounds SceneBvh::instance_bounds(const InstanceGroup& group, uint32_t slot) const
{
    return instance_bounds(group, group.world, group.visual->get_transforms()[slot], group.visual->get_slot_mesh(slot));
}

BvhItemBounds SceneBvh::instance_bounds(const InstanceGroup& group, const mat4& w, const InstanceTransform& t,
                                        uint32_t mesh) const
{
    const BvhItemBounds& local = group.meshes[mesh].bounds;
    // The instance's matrix applied to the mesh bounds' center and half extents.
    float center[3];
    float half[3];
    for (int k = 0; k < 3; ++k) {
        center[k] = 0.5f * (local.min[k] + local.max[k]);
        half[k] = 0.5f * (local.max[k] - local.min[k]);
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
        out.min[row] = c - h - group.max_displacement;
        out.max[row] = c + h + group.max_displacement;
    }
    return out;
}

namespace {

/**
 * Runs a visual's motion function over its instances, writing each one's world matrix,
 * which raster and rays both read. The table at table_word holds params (vec4), the
 * largest allowed displacement, how far the visual's meshes reach from their origins,
 * and a count of instances that moved too far.
 */
constexpr string_view instance_motion_prefix_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct MotionInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) buffer MotionInstances { MotionInstance data[]; } motion_instances;
layout(set = 1, binding = 16, std430) buffer MotionWords { uint data[]; } motion_words;
layout(set = 1, binding = 16, std430) readonly buffer MotionRows { vec4 data[]; } motion_rows;

layout(push_constant) uniform PC {
    uint instances_base;
    uint slot_count;
    uint table_word;
    uint rest_row;
} pc;

)";

constexpr string_view instance_motion_main_src = R"(

void main()
{
    uint slot = gl_GlobalInvocationID.x;
    if (slot >= pc.slot_count) return;

    vec4 row0 = motion_rows.data[pc.rest_row + slot * 3u];
    vec4 row1 = motion_rows.data[pc.rest_row + slot * 3u + 1u];
    vec4 row2 = motion_rows.data[pc.rest_row + slot * 3u + 2u];
    if (row0.xyz == vec3(0.0) && row1.xyz == vec3(0.0) && row2.xyz == vec3(0.0)) return;
    mat4 rest = transpose(mat4(row0, row1, row2, vec4(0.0, 0.0, 0.0, 1.0)));

    uint t = pc.table_word;
    vec4 params = vec4(uintBitsToFloat(motion_words.data[t]), uintBitsToFloat(motion_words.data[t + 1u]),
                       uintBitsToFloat(motion_words.data[t + 2u]), uintBitsToFloat(motion_words.data[t + 3u]));
    mat4 world = velk_instance_motion(rest, slot, params);

    // How far a point of the mesh can have moved: its origin's move, plus its reach
    // times the most the rotation and scale can have changed it.
    mat3 change = mat3(world) - mat3(rest);
    float change2 = dot(change[0], change[0]) + dot(change[1], change[1]) + dot(change[2], change[2]);
    float moved = length(world[3].xyz - rest[3].xyz) + uintBitsToFloat(motion_words.data[t + 5u]) * sqrt(change2);
    if (moved > uintBitsToFloat(motion_words.data[t + 4u])) {
        atomicAdd(motion_words.data[t + 6u], 1u);
    }

    motion_instances.data[pc.instances_base + slot].world_matrix = world;
}
)";

constexpr uint32_t kMotionTableWords = 8;
constexpr uint32_t kMotionMaxDisplacementWord = 4;
constexpr uint32_t kMotionReachWord = 5;
constexpr uint32_t kMotionOverrunWord = 6;
/// How often, in updates, the count of instances that moved too far is read back.
constexpr uint32_t kMotionCheckInterval = 120;

} // namespace

void SceneBvh::alloc_instance_motion(InstanceGroup& group, FrameContext& ctx)
{
    group.motion_region = {};
    group.motion_capacity = 0;
    if (group.visual->get_motion_source().empty()) {
        return;
    }
    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) {
        return;
    }
    const uint32_t slot_count = group.visual->get_instance_capacity();
    const uint32_t capacity = slot_count + slot_count / 8u + 16u;
    group.motion_region = arena->alloc(uint64_t(kMotionTableWords) * sizeof(uint32_t), 16);
    auto* words = static_cast<uint32_t*>(group.motion_region.valid() ? arena->mapped_at(group.motion_region.offset())
                                                                     : nullptr);
    if (!words) {
        VELK_LOG(E, "SceneBvh: no room for the motion of %u instances", slot_count);
        group.motion_region = {};
        return;
    }
    group.motion_capacity = capacity;
    group.motion_region_fresh = true;
    group.motion_pass_dirty = true;
    group.motion_dirty = true;

    float reach2 = 0.f;
    for (uint32_t m = 0; m < group.visual->get_mesh_count(); ++m) {
        if (auto mesh = group.visual->get_mesh(m)) {
            const aabb local = mesh->get_bounds();
            const vec3 lo = local.min();
            const vec3 hi = local.max();
            for (const float x : {lo.x, hi.x}) {
                for (const float y : {lo.y, hi.y}) {
                    for (const float z : {lo.z, hi.z}) {
                        reach2 = std::max(reach2, x * x + y * y + z * z);
                    }
                }
            }
        }
    }
    uint32_t table[kMotionTableWords] = {};
    const float max_displacement = group.visual->get_max_displacement();
    const float reach = std::sqrt(reach2);
    std::memcpy(&table[kMotionMaxDisplacementWord], &max_displacement, sizeof(float));
    std::memcpy(&table[kMotionReachWord], &reach, sizeof(float));
    std::memcpy(words, table, sizeof(table));
}

void SceneBvh::emit_instance_motion(FrameContext& ctx, IRenderGraph& graph)
{
    for (auto& [element, group] : instance_groups_) {
        if (!group.motion_region.valid() || group.visual->rest_region_size() == 0
            || group.visual->instance_region_size() == 0) {
            continue;
        }
        auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
        if (!arena) {
            continue;
        }

        float params[4];
        const bool params_changed = group.visual->take_motion_params(params);
        if (params_changed) {
            arena->update_at(group.motion_region.offset(), params, sizeof(params));
        }
        const bool rest_changed = group.visual->take_rest_changed();
        const bool needed = params_changed || rest_changed || group.motion_dirty;
        group.motion_dirty = false;
        group.motion_region_fresh = false;

        if (update_frame_ % kMotionCheckInterval == 0 && !group.motion_error_logged) {
            uint32_t overruns = 0;
            arena->read_at(group.motion_region.offset() + kMotionOverrunWord * sizeof(uint32_t), &overruns,
                           sizeof(overruns));
            if (overruns != 0) {
                VELK_LOG(E, "SceneBvh: an instance motion moved past its maximum displacement of %f, %u times",
                         double(group.max_displacement), overruns);
                group.motion_error_logged = true;
            }
        }

        if (!needed) {
            continue;
        }

        const uint64_t rest_offset = group.visual->rest_region_offset();
        const uint64_t instances_offset = group.visual->instance_region_offset();
        if (rest_offset != group.motion_rest_offset || instances_offset != group.motion_instances_offset) {
            group.motion_rest_offset = rest_offset;
            group.motion_instances_offset = instances_offset;
            group.motion_pass_dirty = true;
        }

        InstanceGroup* motion_group = &group;
        emit_cached_view_pass(group.motion_pass, group.motion_pass_dirty, "instance.motion", graph,
                              [&](CachedPassRecording& rec) {
            string_view source = motion_group->visual->get_motion_source();
            uint64_t key = 0xcbf29ce484222325ULL;
            for (size_t i = 0; i < source.size(); ++i) {
                key = (key ^ static_cast<uint8_t>(source[i])) * 0x100000001b3ULL;
            }
            key |= 0x4000000000000000ULL;
            auto pipeline = ctx.render_ctx->pipelines().find(
                PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0});
            if (!pipeline) {
                string src(instance_motion_prefix_src);
                src += source;
                src += instance_motion_main_src;
                pipeline = ctx.render_ctx->pipelines().compile_compute(string_view(src), key);
            }
            auto cmd = pipeline ? ctx.backend->create_command_buffer() : nullptr;
            if (!cmd) {
                return;
            }
            struct PushC
            {
                uint32_t instances_base;
                uint32_t slot_count;
                uint32_t table_word;
                uint32_t rest_row;
            };
            const uint32_t slot_count = motion_group->visual->get_instance_capacity();
            const PushC pc{static_cast<uint32_t>(instances_offset / sizeof(ElementInstance)), slot_count,
                           static_cast<uint32_t>(motion_group->motion_region.offset() / sizeof(uint32_t)),
                           static_cast<uint32_t>(rest_offset / (sizeof(float) * 4))};
            DispatchCall dc{};
            dc.pipeline = pipeline.get();
            dc.groups_x = (slot_count + 63u) / 64u;
            dc.groups_y = 1;
            dc.groups_z = 1;
            dc.root_constants_size = sizeof(pc);
            std::memcpy(dc.root_constants, &pc, sizeof(pc));

            cmd->begin_recording();
            cmd->push_label("Instance motion");
            // Earlier frames may still be reading what this writes.
            cmd->record_global_barrier();
            cmd->record_dispatch(dc);
            cmd->record_global_barrier();
            cmd->pop_label();
            cmd->end_recording();
            rec.cmd = std::move(cmd);
            rec.held.push_back(std::move(pipeline));
        });
    }
}

void SceneBvh::write_quad_table(InstanceGroup& group)
{
    const uint32_t meshes = group.visual->get_mesh_count();
    vector<MeshQuad> table;
    table.resize(meshes);
    for (uint32_t m = 0; m < meshes; ++m) {
        if (auto mesh = group.visual->get_mesh(m)) {
            mesh_quad(*mesh, table[m]);
        }
    }
    group.mesh_quads = std::move(table);
}

void SceneBvh::emit_quad_trees(FrameContext& ctx, IRenderGraph& graph)
{
    auto arena = ctx.resources ? ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t)) : nullptr;
    if (!arena || !ctx.render_ctx || !ctx.backend) {
        return;
    }
    // The partitions that changed: each tree from its objects' quads, which it wrote on the CPU
    // (partition_objects), or without its instances its quads at the level it keeps.
    vector<uint32_t> jobs;
    uint32_t trees = 0;
    uint64_t nodes = 0;
    for (Partition& partition : partitions_) {
        if (!partition.active || !partition.quad_tree_dirty) {
            continue;
        }
        if (!partition.quad_tree.valid() || !partition.quad_objects.valid() || !plain_cull_.valid()) {
            continue;
        }
        jobs.push_back(0u);
        jobs.push_back(partition.quad_object_count);
        jobs.push_back(static_cast<uint32_t>(partition.quad_tree.offset() / sizeof(uint32_t)));
        jobs.push_back(partition.quad_leaves);
        jobs.push_back(static_cast<uint32_t>(partition.quad_objects.offset() / sizeof(uint32_t)));
        jobs.push_back(kCullNoSlot);
        jobs.push_back(kCullNoSlot);
        jobs.push_back(0u);
        partition.quad_tree_dirty = false;
        if (partition.cull_entry != kUnplaced) {
            // Its list and chunks with its tree, which a list of chunk quads waited for.
            const uint32_t words[4] = {static_cast<uint32_t>(partition.cull_members.offset() / sizeof(uint32_t)),
                                       partition.cull_chunks,
                                       static_cast<uint32_t>(partition.quad_tree.offset() / sizeof(uint32_t)),
                                       partition.quad_leaves | (partition.quad_leaves_only ? kQuadLeavesOnly : 0u)};
            arena->update_at(plain_cull_.offset() + (uint64_t(partition.cull_entry) * kCullEntryWords + 6u) * sizeof(uint32_t),
                             words, sizeof(words));
        }
        ++trees;
        nodes += uint64_t(partition.quad_leaves) * 2u - 1u;
    }
    if (jobs.empty()) {
        // What a round of builds made, once it is over.
        if (quad_trees_built_ != 0) {
            VELK_LOG(I, "quad trees: %u built, %llu nodes", quad_trees_built_, static_cast<unsigned long long>(quad_nodes_built_));
            VELK_LOG(I, "quad trees: %.1f MB", static_cast<double>(quad_nodes_built_) * kQuadNodeWords * 4.0 / (1024.0 * 1024.0));
            quad_trees_built_ = 0;
            quad_nodes_built_ = 0;
        }
        return;
    }
    quad_trees_built_ += trees;
    quad_nodes_built_ += nodes;

    const auto job_count = static_cast<uint32_t>(jobs.size() / kQuadJobWords);
    if (job_count > quad_jobs_capacity_) {
        quad_jobs_capacity_ = job_count + job_count / 2u + 16u;
        quad_jobs_ = arena->alloc(uint64_t(quad_jobs_capacity_) * kQuadJobWords * sizeof(uint32_t));
        if (!quad_jobs_.valid()) {
            quad_jobs_capacity_ = 0;
            VELK_LOG(E, "SceneBvh: no room for the quad tree jobs");
            return;
        }
    }
    // A frame on the GPU may still read the list from before, so it is written in order with
    // this frame's passes.
    arena->update_at(quad_jobs_.offset(), jobs.data(), jobs.size() * sizeof(uint32_t));

    bool dirty = true;
    const auto jobs_word = static_cast<uint32_t>(quad_jobs_.offset() / sizeof(uint32_t));
    emit_cached_view_pass(quad_pass_, dirty, "instance.quad_trees", graph, [&](CachedPassRecording& rec) {
        constexpr uint64_t kKey = 0x5175616454726565ULL | 0x4000000000000000ULL;
        auto pipeline = ctx.render_ctx->pipelines().find(PipelineCacheKey{kKey, PixelFormat::RGBA8, DepthFormat::None, 0});
        if (!pipeline) {
            pipeline = ctx.render_ctx->pipelines().compile_compute(quad_tree_build_src, kKey);
        }
        auto cmd = pipeline ? ctx.backend->create_command_buffer() : nullptr;
        if (!cmd) {
            return;
        }
        DispatchCall dc{};
        dc.pipeline = pipeline.get();
        dc.groups_x = job_count;
        dc.groups_y = 1;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(jobs_word);
        std::memcpy(dc.root_constants, &jobs_word, sizeof(jobs_word));
        cmd->begin_recording();
        cmd->push_label("Quad trees");
        cmd->record_global_barrier();
        cmd->record_dispatch(dc);
        cmd->record_global_barrier();
        cmd->pop_label();
        cmd->end_recording();
        rec.cmd = std::move(cmd);
        rec.held.push_back(std::move(pipeline));
    });
}

void SceneBvh::update_residency()
{
    if (view_points_.empty()) {
        return;
    }

    // Each update, what each partition needs: its members where their quads are over
    // kLodElementPixels in some view, else its tree down to the coarsest level whose quads are all
    // within it, whatever it holds. Members whose quads are over kResidencyAheadShare of that come
    // in ahead of need, a few an update, the largest on screen first; those over it and still out
    // come in now, so that no partition shows as quads while its members are larger than that.
    const float ahead_pixels = kLodElementPixels * kResidencyAheadShare;
    vector<uint32_t> needed;
    pending_load_.clear();
    // The places each group's partitions that keep or want their members take.
    vector<uint32_t> wanted(groups_.size(), 0u);
    for (uint32_t p = 0; p < partitions_.size(); ++p) {
        Partition& partition = partitions_[p];
        if (!partition.active || partition.cull_entry == kUnplaced || partition.members.empty()) {
            continue;
        }
        if (partition.level_sides.empty()) {
            partition_level(partition, 0u, &partition.level_sides);
        }
        // The most pixels a unit covers anywhere in it, over the views.
        float pixels_per_unit = 0.f;
        for (const ViewPoint& view : view_points_) {
            const float at[3] = {view.at.x, view.at.y, view.at.z};
            float dist2 = 0.f;
            for (int k = 0; k < 3; ++k) {
                const float away = std::max({partition.cull_bounds.min[k] - at[k], at[k] - partition.cull_bounds.max[k], 0.f});
                dist2 += away * away;
            }
            pixels_per_unit = std::max(pixels_per_unit, view.pixels_per_unit / std::max(std::sqrt(dist2), 1e-3f));
        }
        const float allowed = kLodElementPixels / std::max(pixels_per_unit, 1e-12f);
        const float members_pixels = partition.level_sides[0] * pixels_per_unit;
        partition.score = members_pixels;
        // Its members in, or out once clearly small enough, so that one at the edge does not
        // come and go.
        const bool wants_members = partition.resident ? members_pixels * kResidencySwapGain > ahead_pixels
                                                      : members_pixels > ahead_pixels;
        if (wants_members) {
            for (const Partition::GroupCount& n : partition.by_need) {
                if (n.group < wanted.size()) {
                    wanted[n.group] += n.count;
                }
            }
            if (!partition.resident) {
                (members_pixels > kLodElementPixels ? needed : pending_load_).push_back(p);
            }
            continue;
        }
        // Level 0 is its objects' own quads, which need no instances.
        uint32_t level = 0;
        while (level + 1u < partition.level_sides.size() && partition.level_sides[level + 1u] <= allowed) {
            ++level;
        }
        if (partition.resident || level != partition.stored_level) {
            // Its instances give up their places when it is uploaded.
            partition.resident = false;
            partition.stored_level = level;
            partition.dirty = true;
            partition.residency_changed = true;
        }
    }
    // The largest on screen first, loaded last from the back.
    std::sort(pending_load_.begin(), pending_load_.end(),
              [&](uint32_t a, uint32_t b) { return partitions_[a].score < partitions_[b].score; });

    // Room for every instance the partitions that keep or want their members hold, reserved at
    // once. A region that has to grow moves, and nothing comes in until what names it has
    // followed, on the next frame.
    bool room = true;
    for (uint32_t id = 0; id < groups_.size(); ++id) {
        InstanceGroup* group = groups_[id];
        if (group && group->visual->loads_by_need() && wanted[id] != 0u) {
            room = group->visual->reserve_instances(wanted[id]) && room;
        }
    }
    if (!room) {
        return;
    }

    // Brings @p p's members in, within the device-wide cap.
    const auto load = [&](uint32_t p) {
        Partition& partition = partitions_[p];
        if (!partition.active || partition.resident) {
            return;
        }
        uint64_t need = 0;
        for (const Partition::GroupCount& n : partition.by_need) {
            need += n.count;
        }
        if (resident_total_ + need > resident_cap_) {
            return;
        }
        partition.resident = true;
        partition.dirty = true;
        partition.residency_changed = true;
        resident_total_ += need;
    };
    // Those needed now, all of them; then those ahead of need, a few an update for each visual
    // loaded by need, since each builds a tree.
    for (uint32_t p : needed) {
        load(p);
    }
    uint32_t by_need_visuals = 0;
    for (const auto& [element, group] : instance_groups_) {
        by_need_visuals += group.visual->loads_by_need() ? 1u : 0u;
    }
    const uint32_t max_changes = kMaxResidencyChanges * std::max(by_need_visuals, 1u);
    for (uint32_t changes = 0; changes < max_changes && !pending_load_.empty(); ++changes) {
        load(pending_load_.back());
        pending_load_.pop_back();
    }
}

void SceneBvh::write_cull_entry(const Partition& partition, IGpuArena& arena, bool fresh)
{
    uint32_t entry[kCullEntryWords];
    std::memcpy(entry, partition.cull_bounds.min, sizeof(float) * 3);
    std::memcpy(entry + 3, partition.cull_bounds.max, sizeof(float) * 3);
    // Its quad tree once a build of it is under way, which emit_quad_trees writes. A list of
    // quads is nothing to choose from without its tree, so it waits for it too.
    const bool tree = partition.quad_leaves != 0 && !partition.quad_tree_dirty;
    entry[6] = partition.cull_members.valid() ? static_cast<uint32_t>(partition.cull_members.offset() / sizeof(uint32_t)) : 0u;
    entry[7] = partition.quad_leaves_only && !tree ? 0u : partition.cull_chunks;
    entry[8] = tree ? static_cast<uint32_t>(partition.quad_tree.offset() / sizeof(uint32_t)) : kCullNoSlot;
    entry[9] = tree ? partition.quad_leaves | (partition.quad_leaves_only ? kQuadLeavesOnly : 0u) : 0u;
    const uint64_t at = plain_cull_.offset() + uint64_t(partition.cull_entry) * sizeof(entry);
    if (fresh) {
        arena.write_at(at, entry, sizeof(entry));
    } else {
        arena.update_at(at, entry, sizeof(entry));
    }
}

void SceneBvh::write_cull(Partition& partition, FrameContext& ctx)
{
    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) {
        return;
    }
    const bool dynamic = &partition == &dynamic_;

    // Its objects and their quads; without its members, its tree's quads at the level it keeps,
    // which its list names instead. Its sizes per level, for what it needs, measured again.
    vector<uint32_t> objects;
    vector<MeshQuad> quads;
    partition_objects(partition, objects, quads);
    partition.quad_objects = {};
    partition.quad_object_count = 0;
    partition.quad_leaves_only = false;
    vector<uint32_t> list;
    if (partition.resident || dynamic) {
        list = objects;
        if (!dynamic && !quads.empty()) {
            vector<MeshQuad> padded = quads;
            const auto chunks = static_cast<uint32_t>((padded.size() + kCullChunkSlots - 1u) / kCullChunkSlots);
            padded.resize(quad_tree_leaves(chunks * kCullChunkSlots));
            quad_levels(std::move(padded), 0u, &partition.level_sides);
        }
    } else if (!quads.empty()) {
        const size_t count = quads.size();
        const auto chunks = static_cast<uint32_t>((count + kCullChunkSlots - 1u) / kCullChunkSlots);
        quads.resize(quad_tree_leaves(chunks * kCullChunkSlots));
        quads = quad_levels(std::move(quads), partition.stored_level, &partition.level_sides);
        // Only the nodes over its objects: those after are none, and the GPU merges the same
        // pairs without them.
        const size_t covering = (count + (size_t(1) << partition.stored_level) - 1u) >> partition.stored_level;
        quads.resize(std::min(quads.size(), covering));
        partition.quad_leaves_only = true;
        for (uint32_t i = 0; i < quads.size(); ++i) {
            list.push_back(quads[i].color != 0u ? i : kCullNoSlot);
        }
    }

    // A new list each time, so that culling sees its entry change.
    const auto chunks = static_cast<uint32_t>((list.size() + kCullChunkSlots - 1u) / kCullChunkSlots);
    partition.cull_members = {};
    partition.cull_chunks = 0;
    if (chunks != 0) {
        partition.cull_members = arena->alloc(uint64_t(chunks) * kCullChunkSlots * sizeof(uint32_t));
        if (partition.cull_members.valid()) {
            list.resize(size_t(chunks) * kCullChunkSlots, kCullNoSlot);
            arena->write_at(partition.cull_members.offset(), list.data(), list.size() * sizeof(uint32_t));
            partition.cull_chunks = chunks;
        } else {
            VELK_LOG(E, "SceneBvh: no room for a partition's culling list");
        }
    }

    // Its quads and the tree over them, built again on the GPU. Not for the dynamic partition.
    const uint32_t leaves = partition.cull_chunks != 0 && !dynamic ? quad_tree_leaves(partition.cull_chunks * kCullChunkSlots) : 0u;
    if (leaves != 0 && !quads.empty()) {
        partition.quad_objects = arena->alloc(quads.size() * sizeof(MeshQuad));
        if (partition.quad_objects.valid()) {
            arena->write_at(partition.quad_objects.offset(), quads.data(), quads.size() * sizeof(MeshQuad));
            partition.quad_object_count = static_cast<uint32_t>(quads.size());
        }
    }
    const uint32_t tree_leaves = partition.quad_objects.valid() ? leaves : 0u;
    if (tree_leaves != partition.quad_leaves) {
        partition.quad_tree = tree_leaves != 0
            ? arena->alloc((uint64_t(tree_leaves) * 2u - 1u) * kQuadNodeWords * sizeof(uint32_t))
            : ArenaRegion{};
        partition.quad_leaves = partition.quad_tree.valid() ? tree_leaves : 0u;
    }
    partition.quad_tree_dirty = partition.quad_leaves != 0;

    if (partition.cull_entry == kUnplaced) {
        if (!plain_cull_free_.empty()) {
            partition.cull_entry = plain_cull_free_.back();
            plain_cull_free_.pop_back();
        } else {
            if (plain_cull_next_ == plain_cull_capacity_) {
                // A larger table, fresh, so nothing has read it: every entry written again.
                const uint32_t capacity = plain_cull_capacity_ * 2u + 16u;
                ArenaRegion table = arena->alloc(uint64_t(capacity) * kCullEntryWords * sizeof(uint32_t));
                if (!table.valid()) {
                    VELK_LOG(E, "SceneBvh: no room for the partitions' culling table");
                    return;
                }
                vector<uint32_t> empty(size_t(capacity) * kCullEntryWords, 0u);
                arena->write_at(table.offset(), empty.data(), empty.size() * sizeof(uint32_t));
                plain_cull_ = std::move(table);
                plain_cull_capacity_ = capacity;
                for (const Partition& p : partitions_) {
                    if (p.active && p.cull_entry != kUnplaced && &p != &partition) {
                        write_cull_entry(p, *arena, true);
                    }
                }
                if (dynamic_.cull_entry != kUnplaced && &dynamic_ != &partition) {
                    write_cull_entry(dynamic_, *arena, true);
                }
            }
            partition.cull_entry = plain_cull_next_++;
        }
    }
    write_cull_entry(partition, *arena, false);
}

void SceneBvh::release_cull(Partition& partition, FrameContext& ctx)
{
    if (partition.cull_entry == kUnplaced) {
        return;
    }
    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (arena && plain_cull_.valid()) {
        const uint32_t empty[kCullEntryWords] = {};
        arena->update_at(plain_cull_.offset() + uint64_t(partition.cull_entry) * sizeof(empty), empty, sizeof(empty));
    }
    plain_cull_free_.push_back(partition.cull_entry);
    partition.cull_entry = kUnplaced;
    partition.cull_members = {};
    partition.cull_chunks = 0;
    partition.quad_objects = {};
    partition.quad_object_count = 0;
    partition.quad_tree = {};
    partition.quad_leaves = 0;
    partition.quad_tree_dirty = false;
}

void SceneBvh::write_slot_tables(FrameContext& ctx)
{
    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) {
        return;
    }
    for (auto& [element, group] : instance_groups_) {
        if (!group.slot_table_dirty && group.slot_dirty_from >= group.slot_dirty_to) {
            continue;
        }
        const bool all = group.slot_table_dirty;
        const uint32_t from = group.slot_dirty_from;
        const uint32_t to = group.slot_dirty_to;
        group.slot_table_dirty = false;
        group.slot_dirty_from = 0xFFFFFFFFu;
        group.slot_dirty_to = 0;
        if (all) group.dirty_slots.clear();
        const auto slots = static_cast<uint32_t>(group.visual->get_transforms().size());
        if (slots == 0) {
            group.dirty_slots.clear();
            group.visual->set_slot_table(0, 0);
            continue;
        }
        // Room for more, so that a growing visual seldom moves it; a new place is fresh, so
        // nothing has read it.
        const bool fresh = !group.slot_table.valid() || group.slot_table_slots < slots;
        if (fresh) {
            group.slot_table_slots = slots + slots / 4u + 16u;
            group.slot_table = arena->alloc(uint64_t(group.slot_table_slots) * sizeof(uint32_t));
            if (!group.slot_table.valid()) {
                group.slot_table_slots = 0;
                group.visual->set_slot_table(0, 0);
                group.dirty_slots.clear();
                group.slot_table_copy.clear();
                VELK_LOG(E, "SceneBvh: no room for a visual's slot table");
                continue;
            }
        }
        // Each live slot's place in the raster region where its partition keeps its members, kept
        // in a copy: every slot when the table is new or all changed, else only the slots that
        // did, and the range they span uploaded from the copy.
        const auto place = [&](uint32_t slot) {
            return slot < group.slot_partition.size() && group.slot_partition[slot] != kUnplaced && group.visual->is_live(slot)
                     ? group.visual->get_instance_index(slot)
                     : kCullNoSlot;
        };
        auto& copy = group.slot_table_copy;
        uint32_t first = 0;
        uint32_t end = 0;
        if (all || fresh || copy.size() != slots) {
            copy.resize(slots);
            for (uint32_t slot = 0; slot < slots; ++slot) copy[slot] = place(slot);
            end = slots;
        } else {
            for (uint32_t slot : group.dirty_slots) {
                if (slot < slots) copy[slot] = place(slot);
            }
            first = std::min(from, slots);
            end = std::min(to, slots);
        }
        group.dirty_slots.clear();
        if (first < end) {
            const uint64_t at = group.slot_table.offset() + uint64_t(first) * sizeof(uint32_t);
            if (fresh) {
                arena->write_at(at, copy.data() + first, (end - first) * sizeof(uint32_t));
            } else {
                arena->update_at(at, copy.data() + first, (end - first) * sizeof(uint32_t));
            }
        }
        group.visual->set_slot_table(static_cast<uint32_t>(group.slot_table.offset() / sizeof(uint32_t)), slots);
    }
}

void SceneBvh::remove_group_members(InstanceGroup& group)
{
    for (uint32_t slot = 0; slot < group.slot_partition.size(); ++slot) {
        const uint32_t p = group.slot_partition[slot];
        if (p == kDynamic) {
            dynamic_.dirty = true;
        } else if (p != kUnplaced && p < partitions_.size()) {
            partitions_[p].dirty = true;
        }
        group.slot_partition[slot] = kUnplaced;
        if (group.visual->loads_by_need() && slot < group.visual->get_transforms().size()
            && group.visual->get_instance_index(slot) != kNoInstanceIndex) {
            group.visual->release_instance_index(slot);
            group.resident_members -= std::min(group.resident_members, 1u);
        }
    }
    group.movers.clear();
    group.slot_table_dirty = true;
    layout_dirty_ = true;
}

void SceneBvh::place_group(InstanceGroup& group)
{
    const auto slots = static_cast<uint32_t>(group.visual->get_transforms().size());
    group.slot_partition.resize(slots, kUnplaced);
    for (uint32_t slot = 0; slot < slots; ++slot) {
        if (group.visual->is_live(slot) && group.slot_partition[slot] == kUnplaced) {
            unplaced_.push_back(instance_member(group, slot));
        }
    }
    group.slot_table_dirty = true;
}

void SceneBvh::update_instances(IElement* element, IInstancedMeshInternal& visual, const EmitContext& emit)
{
    auto element_state = read_state<IElement>(element);
    if (!element_state) {
        return;
    }

    // One run of shapes per mesh, local to each instance.
    struct CollectShapes
    {
        vector<RtShape> shapes;
    };
    CollectShapes collect;
    const mat4 identity = mat4::identity();
    vector<InstanceGroup::MeshRun> runs;
    bool unresolved = false;
    for (uint32_t m = 0; m < visual.get_mesh_count(); ++m) {
        InstanceGroup::MeshRun run;
        run.first_shape = static_cast<uint32_t>(collect.shapes.size());
        if (auto mesh = visual.get_mesh(m)) {
            emit_mesh_shapes(interface_cast<IVisual>(&visual), *mesh, identity, ::velk::color::white(), nullptr,
                             +[](void* u, ShapeSite& site) {
                                 static_cast<CollectShapes*>(u)->shapes.push_back(site.geometry);
                             }, &collect);
            const aabb local = mesh->get_bounds();
            const vec3 lo = local.min();
            const vec3 hi = local.max();
            run.bounds = {{lo.x, lo.y, lo.z}, {hi.x, hi.y, hi.z}};
        }
        run.shape_count = static_cast<uint32_t>(collect.shapes.size()) - run.first_shape;
        unresolved = unresolved || (run.shape_count == 0 && visual.get_mesh_instance_count(m) != 0);
        runs.push_back(run);
    }
    if (unresolved || collect.shapes.empty()) {
        if (std::find(unresolved_.begin(), unresolved_.end(), element) == unresolved_.end()) {
            unresolved_.push_back(element);
        }
        return;
    }

    auto& group = instance_groups_[element];
    const bool new_group = group.visual == nullptr;
    group.visual = &visual;
    if (new_group) {
        // An id its members carry: the first free one not freed in this update.
        uint32_t id = 0;
        while (id < groups_.size()
               && (groups_[id] != nullptr
                   || std::find(retired_groups_.begin(), retired_groups_.end(), id) != retired_groups_.end())) {
            ++id;
        }
        if (id >= kMaxGroups) {
            VELK_LOG(E, "SceneBvh: more than %u instanced visuals", kMaxGroups);
            instance_groups_.erase(element);
            return;
        }
        if (id == groups_.size()) {
            groups_.push_back(nullptr);
        }
        groups_[id] = &group;
        group.id = id;
    }
    if (visual.get_transforms().size() > kMemberSlotMask + 1u) {
        VELK_LOG(E, "SceneBvh: an instanced visual with more than %u instances", kMemberSlotMask + 1u);
    }

    // The meshes' shapes, kept to tell when they change. Why its partitions are cut again, for
    // the log.
    bool rebuild = new_group;
    const char* why = "";
    if (group.shapes.size() != collect.shapes.size()) {
        group.shapes = std::move(collect.shapes);
        rebuild = true;
        why = "shapes";
    } else if (std::memcmp(group.shapes.data(), collect.shapes.data(), collect.shapes.size() * sizeof(RtShape)) != 0) {
        group.shapes = std::move(collect.shapes);
    }
    if (group.meshes.size() != runs.size()
        || std::memcmp(group.meshes.data(), runs.data(), runs.size() * sizeof(InstanceGroup::MeshRun)) != 0) {
        group.meshes = std::move(runs);
        rebuild = true;
        why = "meshes";
    }

    vector<uint32_t> changed;
    bool all_changed = false;
    vector<InstanceBefore> before;
    bool before_complete = true;
    visual.take_changed_slots(changed, all_changed, before, before_complete);
    if (all_changed) {
        rebuild = true;
        why = "all instances changed";
    }

    const float max_displacement = visual.get_max_displacement();
    if (group.max_displacement != max_displacement) {
        group.max_displacement = max_displacement;
        rebuild = true;
        why = "displacement";
    }
    const bool has_motion = !visual.get_motion_source().empty();
    if (has_motion != group.motion_region.valid()
        || (has_motion && visual.get_instance_capacity() > group.motion_capacity)) {
        rebuild = true;
        why = "motion";
    }

    // An object id per slot. A new range renames every instance, in its records and in
    // what is drawn.
    const uint32_t object_base =
        emit.scene ? emit.scene->get_object_ids(*element, static_cast<uint32_t>(visual.get_transforms().size())) : 0u;
    if (object_base != group.object_base) {
        clear_object_transforms(group.object_base, group.object_count);
        group.object_base = object_base;
        visual.set_object_id_base(object_base);
        rebuild = true;
        why = "object ids";
    }
    const auto object_count = static_cast<uint32_t>(visual.get_transforms().size());
    if (object_count < group.object_count) {
        clear_object_transforms(group.object_base + object_count, group.object_count - object_count);
    }
    group.object_count = object_count;

    const mat4 old_world = group.world;
    const mat4& new_world = element_state->world_matrix;
    group.world = new_world;
    const bool element_moved = !new_group && std::memcmp(old_world.m, new_world.m, sizeof(new_world.m)) != 0;

    // Where each instance is, for whatever keeps data by object: all of them when the
    // element moved or the group is new, else those that changed.
    if (rebuild || element_moved) {
        set_instance_transforms(group, nullptr);
    } else if (!changed.empty()) {
        set_instance_transforms(group, &changed);
    }

    if (rebuild) {
        if (!new_group) {
            VELK_LOG(I, "instances placed in partitions again: %s", why);
        }
        note_all_changed();
        remove_group_members(group);
        alloc_instance_motion(group, emit.frame);
        write_quad_table(group);
        place_group(group);
        return;
    }

    // A moved element moves its instances: to the dynamic partition, or past a limit placed
    // again.
    if (element_moved) {
        note_all_changed();
        uint32_t live = 0;
        for (uint32_t slot = 0; slot < group.slot_partition.size(); ++slot) {
            live += group.slot_partition[slot] != kUnplaced ? 1u : 0u;
        }
        if (live > kPartitionRecords) {
            remove_group_members(group);
            place_group(group);
        } else {
            for (uint32_t slot = 0; slot < group.slot_partition.size(); ++slot) {
                const uint32_t current = group.slot_partition[slot];
                if (current == kDynamic) {
                    group.movers[slot] = update_frame_;
                    dynamic_transforms_dirty_ = true;
                } else if (current != kUnplaced) {
                    promote(instance_member(group, slot));
                }
            }
        }
    }

    // Where instances were and are, for whatever keeps results of rays.
    if (!before_complete) {
        note_all_changed();
    }
    for (const InstanceBefore& b : before) {
        note_changed(instance_bounds(group, old_world, b.transform, b.mesh));
    }
    for (uint32_t slot : changed) {
        if (visual.is_live(slot)) {
            note_changed(instance_bounds(group, slot));
        }
    }

    const auto slot_count = static_cast<uint32_t>(visual.get_transforms().size());
    if (group.slot_partition.size() < slot_count) {
        group.slot_partition.resize(slot_count, kUnplaced);
    }
    for (uint32_t slot : changed) {
        const uint32_t current = group.slot_partition[slot];
        if (!visual.is_live(slot)) {
            // Removed: its partition drops it when written again, and it gives up its place.
            if (current == kDynamic) {
                dynamic_.dirty = true;
            } else if (current != kUnplaced) {
                partitions_[current].dirty = true;
            }
            group.slot_partition[slot] = kUnplaced;
            group.movers.erase(slot);
            group.mark_slot(slot);
        } else if (current == kDynamic) {
            dynamic_transforms_dirty_ = true;
            group.movers[slot] = update_frame_;
        } else if (current != kUnplaced) {
            promote(instance_member(group, slot));
        } else {
            // Added: placed with the other new members.
            unplaced_.push_back(instance_member(group, slot));
            group.slot_partition[slot] = kUnplaced;
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
        vector<EmittedRecord> records;
        const void* open_group = nullptr;
        mat4 world{};  ///< Its object space to the world: every site of an element has the same.
    };
    CollectState collect;
    emit_shapes_for_element(element, emit.frame.render_ctx, +[](void* u, ShapeSite& site) {
        auto& s = *static_cast<CollectState*>(u);
        if (site.record_group && site.record_group == s.open_group) {
            auto& open = s.records.back();
            open.shapes.push_back(site.geometry);
            add_shape_color(open, site);
            merge_bounds(open.bounds.min, open.bounds.max, site.aabb_min, site.aabb_max);
            return;
        }
        s.open_group = site.record_group;
        s.world = *site.world;
        s.records.emplace_back();
        auto& fresh = s.records.back();
        fresh.world = *site.world;
        fresh.shapes.push_back(site.geometry);
        add_shape_color(fresh, site);
        for (int k = 0; k < 3; ++k) {
            fresh.bounds.min[k] = site.aabb_min[k];
            fresh.bounds.max[k] = site.aabb_max[k];
        }
    }, &collect);

    // All of the element's records are one object.
    const uint32_t object = emit.scene && !collect.records.empty() ? emit.scene->get_object_ids(*element, 1) : 0u;
    for (auto& emitted : collect.records) {
        emitted.object = object;
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
            const bool moved = std::memcmp(&record.world, &emitted.world, sizeof(record.world)) != 0
                || std::memcmp(&record.bounds, &emitted.bounds, sizeof(record.bounds)) != 0;
            if (moved) {
                note_changed(record.bounds);
                note_changed(emitted.bounds);
                record.world = emitted.world;
                record.bounds = emitted.bounds;
                record.quiet_frames = 0;
                if (record.partition == kDynamic) {
                    dynamic_transforms_dirty_ = true;
                } else if (record.partition != kUnplaced) {
                    promote(slot);
                }
            }
            std::memcpy(record.shapes.data(), emitted.shapes.data(), emitted.shapes.size() * sizeof(RtShape));
            // A new colour: its partition's quads again.
            const uint32_t color = record_color(emitted);
            if (color != record.color) {
                record.color = color;
                if (!moved && record.partition < partitions_.size()) {
                    partitions_[record.partition].dirty = true;
                }
            }
        }
        set_object_transform(object, collect.world, records_radius(collect.records));
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
        record.world = emitted.world;
        record.object = emitted.object;
        record.color = record_color(emitted);
        record.shapes = std::move(emitted.shapes);
        record.bounds = emitted.bounds;
        note_changed(record.bounds);
        record.partition = kUnplaced;
        record.quiet_frames = 0;
        slots.push_back(slot);
        unplaced_.push_back(slot);
        ++live_records_;
    }
    if (!collect.records.empty()) {
        set_object_transform(object, collect.world, records_radius(collect.records));
    }
}

namespace {
// See IBvh::get_object_transforms.
constexpr uint32_t kObjectTransformWords = 14;
constexpr uint32_t kObjectSlotMask = 0x3FFFFFFu;
constexpr uint32_t kObjectSlotBits = 26;
} // namespace

void SceneBvh::set_object_transform(uint32_t object, const mat4& world, float radius)
{
    if (object == 0) {
        return;
    }
    const uint32_t slot = object & kObjectSlotMask;
    if (object_transforms_.size() < size_t(slot + 1) * kObjectTransformWords) {
        object_transforms_.resize(size_t(slot + 1) * kObjectTransformWords, 0u);
    }
    uint32_t* entry = object_transforms_.data() + size_t(slot) * kObjectTransformWords;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            const float v = world(row, col);
            std::memcpy(entry + row * 4 + col, &v, sizeof(float));
        }
    }
    entry[12] = (object >> kObjectSlotBits) + 1u;
    std::memcpy(entry + 13, &radius, sizeof(float));
    transforms_dirty_first_ = std::min(transforms_dirty_first_, slot);
    transforms_dirty_last_ = std::max(transforms_dirty_last_, slot);
}

void SceneBvh::clear_object_transforms(uint32_t object, uint32_t count)
{
    const uint32_t first = object & kObjectSlotMask;
    if (object == 0 || count == 0 || size_t(first) * kObjectTransformWords >= object_transforms_.size()) {
        return;
    }
    const uint32_t last = std::min(first + count, static_cast<uint32_t>(object_transforms_.size() / kObjectTransformWords)) - 1u;
    for (uint32_t slot = first; slot <= last; ++slot) {
        object_transforms_[size_t(slot) * kObjectTransformWords + 12u] = 0u;
    }
    transforms_dirty_first_ = std::min(transforms_dirty_first_, first);
    transforms_dirty_last_ = std::max(transforms_dirty_last_, last);
}

void SceneBvh::set_instance_transforms(const InstanceGroup& group, const vector<uint32_t>* slots)
{
    const auto& transforms = group.visual->get_transforms();
    const auto place = [&](uint32_t slot) {
        if (slot >= transforms.size()) {
            return;
        }
        if (group.visual->is_live(slot)) {
            set_object_transform(group.object_base + slot, group.world * transforms[slot].to_mat4(),
                                 bounds_radius(instance_bounds(group, slot)));
        } else {
            clear_object_transforms(group.object_base + slot, 1);
        }
    };
    if (slots) {
        for (uint32_t slot : *slots) {
            place(slot);
        }
    } else {
        for (uint32_t slot = 0; slot < transforms.size(); ++slot) {
            place(slot);
        }
    }
}

void SceneBvh::flush_object_transforms(FrameContext& ctx)
{
    if (transforms_dirty_first_ > transforms_dirty_last_ && !transforms_all_dirty_) {
        return;
    }
    auto arena = ctx.resources ? ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t)) : nullptr;
    if (!arena || object_transforms_.empty()) {
        return;
    }
    const uint64_t bytes = object_transforms_.size() * sizeof(uint32_t);
    if (!object_transforms_region_.valid() || object_transforms_region_.size() < bytes) {
        // Room for twice as many, so that a growing scene seldom moves it.
        object_transforms_region_ = arena->alloc(bytes * 2u);
        if (!object_transforms_region_.valid()) {
            VELK_LOG(E, "SceneBvh: object transform allocation failed");
            object_transforms_word_ = 0xFFFFFFFFu;
            object_transform_slots_ = 0;
            return;
        }
        transforms_all_dirty_ = true;
    }
    if (transforms_all_dirty_) {
        arena->update_at(object_transforms_region_.offset(), object_transforms_.data(), bytes);
    } else {
        const uint32_t last = std::min(transforms_dirty_last_, static_cast<uint32_t>(object_transforms_.size() / kObjectTransformWords) - 1u);
        const uint64_t from = uint64_t(transforms_dirty_first_) * kObjectTransformWords;
        const uint64_t words = uint64_t(last - transforms_dirty_first_ + 1u) * kObjectTransformWords;
        arena->update_at(object_transforms_region_.offset() + from * sizeof(uint32_t), object_transforms_.data() + from,
                         words * sizeof(uint32_t));
    }
    object_transforms_word_ = static_cast<uint32_t>(object_transforms_region_.offset() / sizeof(uint32_t));
    object_transform_slots_ = static_cast<uint32_t>(object_transforms_.size() / kObjectTransformWords);
    transforms_dirty_first_ = 0xFFFFFFFFu;
    transforms_dirty_last_ = 0;
    transforms_all_dirty_ = false;
}

void SceneBvh::note_changed(const BvhItemBounds& bounds)
{
    // Past this many, saying that everything changed costs readers less than the list.
    constexpr size_t kMaxChanged = 256;
    if (changed_all_) return;
    if (changed_.size() >= kMaxChanged) {
        changed_all_ = true;
        changed_.clear();
        return;
    }
    WorldBox box;
    for (int k = 0; k < 3; ++k) {
        box.min[k] = bounds.min[k];
        box.max[k] = bounds.max[k];
    }
    changed_.push_back(box);
}

void SceneBvh::note_all_changed()
{
    changed_all_ = true;
    changed_.clear();
}

void SceneBvh::free_record(uint32_t slot)
{
    Record& record = records_[slot];
    note_changed(record.bounds);
    if (record.partition == kDynamic) {
        dynamic_.dirty = true;
    } else if (record.partition != kUnplaced) {
        partitions_[record.partition].dirty = true;
    }
    clear_object_transforms(record.object, 1);
    record.owner = nullptr;
    record.partition = kUnplaced;
    record.shapes.clear();
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
        clear_object_transforms(group->second.object_base, group->second.object_count);
        remove_group_members(group->second);
        // Its members drop out of their partitions when those are written again; its id waits
        // until then.
        if (group->second.id < groups_.size()) {
            groups_[group->second.id] = nullptr;
            retired_groups_.push_back(group->second.id);
        }
        instance_groups_.erase(group);
    }
    unresolved_.erase(std::remove(unresolved_.begin(), unresolved_.end(), element), unresolved_.end());
}

void SceneBvh::promote(uint32_t member)
{
    const uint32_t from = member_partition(member);
    if (from == kDynamic) {
        return;
    }
    if (from != kUnplaced) {
        partitions_[from].dirty = true;
    }
    set_member_partition(member, kDynamic);
    if (InstanceGroup* group = member_group(member)) {
        group->movers[member_slot(member)] = update_frame_;
    } else {
        records_[member].quiet_frames = 0;
    }
    dynamic_.members.push_back(member);
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

void SceneBvh::demote_quiet()
{
    vector<uint32_t> quiet;
    for (uint32_t member : dynamic_.members) {
        if (member_partition(member) != kDynamic || !member_live(member)) {
            continue;
        }
        if (InstanceGroup* group = member_group(member)) {
            const auto found = group->movers.find(member_slot(member));
            if (found == group->movers.end() || update_frame_ - found->second >= kQuietFrames) {
                quiet.push_back(member);
            }
        } else if (++records_[member].quiet_frames >= kQuietFrames) {
            quiet.push_back(member);
        }
    }
    for (uint32_t member : quiet) {
        if (InstanceGroup* group = member_group(member)) {
            group->movers.erase(member_slot(member));
        }
        insert_member(member);
        dynamic_.dirty = true;
    }
}

void SceneBvh::insert_member(uint32_t member)
{
    const BvhItemBounds bounds = member_bounds(member);
    // The partition whose bounds grow least.
    uint32_t best = kUnplaced;
    float best_growth = 0.f;
    for (uint32_t p = 0; p < partitions_.size(); ++p) {
        const Partition& partition = partitions_[p];
        if (!partition.active) {
            continue;
        }
        float lo[3] = {partition.cull_bounds.min[0], partition.cull_bounds.min[1], partition.cull_bounds.min[2]};
        float hi[3] = {partition.cull_bounds.max[0], partition.cull_bounds.max[1], partition.cull_bounds.max[2]};
        const float before = surface_area(lo, hi);
        merge_bounds(lo, hi, bounds.min, bounds.max);
        const float growth = surface_area(lo, hi) - before;
        if (best == kUnplaced || growth < best_growth) {
            best = p;
            best_growth = growth;
        }
    }
    if (best == kUnplaced) {
        best = alloc_partition();
        partitions_[best].cut_area = surface_area(bounds.min, bounds.max);
    }
    partitions_[best].members.push_back(member);
    partitions_[best].dirty = true;
    set_member_partition(member, best);
    // Its bounds take it in at once, so that the next one goes by them too.
    merge_bounds(partitions_[best].cull_bounds.min, partitions_[best].cull_bounds.max, bounds.min, bounds.max);
}

void SceneBvh::cut_members(const vector<uint32_t>& members)
{
    if (members.empty()) {
        return;
    }
    vector<BvhItemBounds> items;
    items.reserve(members.size());
    for (uint32_t member : members) {
        items.push_back(member_bounds(member));
    }
    const BvhTree cut = build_bvh_tree({items.data(), items.size()}, kPartitionRecords, true);
    for (const auto& node : cut.nodes) {
        if (node.child_count != 0 || node.shape_count == 0) {
            continue;
        }
        // alloc_partition may reallocate partitions_.
        const uint32_t index = alloc_partition();
        float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        float hi[3] = {-lo[0], -lo[0], -lo[0]};
        bool by_need = false;
        bool all_kept = true;
        for (uint32_t i = 0; i < node.shape_count; ++i) {
            const uint32_t at = cut.order[node.first_shape + i];
            const uint32_t member = members[at];
            const uint32_t from = member_partition(member);
            if (from < partitions_.size()) {
                partitions_[from].dirty = true;
                all_kept = all_kept && partitions_[from].resident;
            } else if (from == kDynamic) {
                dynamic_.dirty = true;
            } else {
                all_kept = false;
            }
            partitions_[index].members.push_back(member);
            set_member_partition(member, index);
            merge_bounds(lo, hi, items[at].min, items[at].max);
            const InstanceGroup* group = member_group(member);
            by_need = by_need || (group && group->visual->loads_by_need());
        }
        partitions_[index].cut_area = surface_area(lo, hi);
        // With instances loaded by need, it comes in from its quads when it is needed, unless all
        // its members come from partitions that had theirs in (or the dynamic partition, which
        // keeps them), so that cutting again does not turn what was in into quads.
        partitions_[index].resident = !by_need || all_kept;
    }
    layout_dirty_ = true;
}

void SceneBvh::recut(FrameContext& ctx)
{
    vector<uint32_t> members;
    for (uint32_t slot = 0; slot < records_.size(); ++slot) {
        const Record& record = records_[slot];
        if (record.owner && record.partition != kDynamic) {
            members.push_back(slot);
        }
    }
    for (auto& [element, group] : instance_groups_) {
        for (uint32_t slot = 0; slot < group.slot_partition.size(); ++slot) {
            if (group.slot_partition[slot] != kDynamic && group.visual->is_live(slot)) {
                members.push_back(instance_member(group, slot));
            }
        }
    }
    unplaced_.clear();

    for (uint32_t p = 0; p < partitions_.size(); ++p) {
        if (partitions_[p].active) {
            release_cull(partitions_[p], ctx);
            partitions_[p] = Partition{};
            free_partitions_.push_back(p);
        }
    }
    for (uint32_t member : members) {
        set_member_partition(member, kUnplaced);
    }
    cut_members(members);
    flush_partitions(ctx);
}

void SceneBvh::flush_partitions(FrameContext& ctx)
{
    for (uint32_t p = 0; p < partitions_.size(); ++p) {
        if (!partitions_[p].active || !partitions_[p].dirty) {
            continue;
        }
        auto& members = partitions_[p].members;
        members.erase(std::remove_if(members.begin(), members.end(),
                                     [&](uint32_t member) { return member_partition(member) != p || !member_live(member); }),
                      members.end());
        std::sort(members.begin(), members.end());
        members.erase(std::unique(members.begin(), members.end()), members.end());
        // Grown past its limit: split by a tree over its members.
        if (members.size() > kPartitionRecords * 2u) {
            const vector<uint32_t> all = members;
            partitions_[p].members.clear();
            partitions_[p].dirty = true;
            for (uint32_t member : all) {
                set_member_partition(member, kUnplaced);
            }
            cut_members(all);
        }
    }
    for (uint32_t p = 0; p < partitions_.size(); ++p) {
        Partition& partition = partitions_[p];
        if (!partition.active || !partition.dirty) {
            continue;
        }
        partition.dirty = false;
        // Only whether its members are in changed: its bounds stay, so the layout does too.
        if (!partition.residency_changed) {
            layout_dirty_ = true;
        }
        partition.residency_changed = false;
        if (partition.members.empty()) {
            release_cull(partition, ctx);
            partition = Partition{};
            free_partitions_.push_back(p);
            continue;
        }
        upload_partition(partition, ctx);
    }
}

void SceneBvh::upload_partition(Partition& partition, FrameContext& ctx)
{
    float bounds_lo[3];
    float bounds_hi[3];
    for (int k = 0; k < 3; ++k) {
        bounds_lo[k] = std::numeric_limits<float>::max();
        bounds_hi[k] = -std::numeric_limits<float>::max();
    }
    for (uint32_t member : partition.members) {
        const BvhItemBounds b = member_bounds(member);
        merge_bounds(bounds_lo, bounds_hi, b.min, b.max);
    }
    for (int k = 0; k < 3; ++k) {
        partition.cull_bounds.min[k] = bounds_lo[k];
        partition.cull_bounds.max[k] = bounds_hi[k];
    }
    // Its instances of visuals loaded by need have places in the raster region while it keeps its
    // members, and none while only its quads; the dynamic partition keeps them always.
    const bool keeps = partition.resident || &partition == &dynamic_;
    partition.by_need.clear();
    for (uint32_t member : partition.members) {
        InstanceGroup* group = member_group(member);
        if (!group) {
            continue;
        }
        const uint32_t slot = member_slot(member);
        if (group->visual->loads_by_need()) {
            // The places it takes when it keeps its members, which residency reserves by.
            auto counted = std::find_if(partition.by_need.begin(), partition.by_need.end(),
                                        [&](const Partition::GroupCount& n) { return n.group == group->id; });
            if (counted == partition.by_need.end()) {
                partition.by_need.push_back({group->id, 1u});
            } else {
                ++counted->count;
            }
            const bool had = group->visual->get_instance_index(slot) != kNoInstanceIndex;
            if (keeps && !had && group->visual->assign_instance_index(slot)) {
                ++group->resident_members;
            } else if (!keeps && had) {
                group->visual->release_instance_index(slot);
                group->resident_members -= std::min(group->resident_members, 1u);
            }
        }
        group->mark_slot(slot);
        // Its instances are written at rest, so the motion runs over them again.
        if (keeps && group->motion_region.valid()) {
            group->motion_dirty = true;
        }
    }
    write_cull(partition, ctx);
}

void SceneBvh::check_recut()
{
    layout_dirty_ = false;
    // The partitions whose bounds have grown well past what they were when their members were
    // cut, which hold what they hold much more loosely: their members cut again together.
    vector<uint32_t> drifted;
    for (uint32_t p = 0; p < partitions_.size(); ++p) {
        const Partition& partition = partitions_[p];
        if (!partition.active || partition.members.empty() || partition.cut_area <= 0.f) {
            continue;
        }
        if (surface_area(partition.cull_bounds.min, partition.cull_bounds.max) > partition.cut_area * kRecutDrift) {
            drifted.push_back(p);
        }
    }
    if (drifted.empty()) {
        return;
    }
    vector<uint32_t> members;
    for (uint32_t p : drifted) {
        for (uint32_t member : partitions_[p].members) {
            if (member_partition(member) == p && member_live(member)) {
                members.push_back(member);
            }
        }
        partitions_[p].dirty = true;
    }
    VELK_LOG(I, "partitions cut again by region: %u partitions, %u members", static_cast<unsigned>(drifted.size()),
             static_cast<unsigned>(members.size()));
    cut_members(members);
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
        remove_group_members(group);
    }
    instance_groups_.clear();
    groups_.clear();
    retired_groups_.clear();
    pending_load_.clear();
    for (uint32_t p = 0; p < partitions_.size(); ++p) {
        if (partitions_[p].active) {
            release_cull(partitions_[p], emit.frame);
            partitions_[p] = Partition{};
            free_partitions_.push_back(p);
        }
    }
    std::fill(object_transforms_.begin(), object_transforms_.end(), 0u);
    transforms_all_dirty_ = true;
    unresolved_.clear();
    unplaced_.clear();
    release_cull(dynamic_, emit.frame);
    dynamic_ = Partition{};
    dynamic_transforms_dirty_ = false;

    if (scene) {
        walk_scene_preorder(scene, scene->root(), [&](IElement* element) { update_element(element, emit); });
    }
    recut(emit.frame);
    needs_full_build_ = false;
}

void SceneBvh::update(const SceneState& state, FrameContext& ctx)
{
    VELK_PERF_SCOPE("renderer.bvh_rebuild");
    const EmitContext emit{ctx, state.scene};
    changed_.clear();
    changed_all_ = false;

    {
        VELK_PERF_SCOPE("renderer.bvh_emit");
        if (needs_full_build_) {
            changed_all_ = true;
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

    // New members: a load large against the scene is cut together with the partitions it lands
    // among, so that what is in one place stays in one partition; a few start in the dynamic
    // partition, and go to the partition they grow least once still.
    size_t live_members = live_records_;
    for (const auto& [element, group] : instance_groups_) {
        live_members += group.visual->get_transforms().size();
    }
    vector<uint32_t> placing;
    for (uint32_t member : unplaced_) {
        if (member_live(member) && member_partition(member) == kUnplaced) {
            placing.push_back(member);
            set_member_partition(member, kDynamic);
        }
    }
    unplaced_.clear();
    if (placing.size() > std::max<size_t>(kMaxUnplacedToDynamic, live_members / 4u)) {
        float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        float hi[3] = {-lo[0], -lo[0], -lo[0]};
        for (uint32_t member : placing) {
            set_member_partition(member, kUnplaced);
            const BvhItemBounds b = member_bounds(member);
            merge_bounds(lo, hi, b.min, b.max);
        }
        // With the members of every partition whose box overlaps the load's.
        vector<uint32_t> cutting = placing;
        for (uint32_t p = 0; p < partitions_.size(); ++p) {
            const Partition& partition = partitions_[p];
            if (!partition.active || partition.members.empty()) {
                continue;
            }
            bool overlaps = true;
            for (int k = 0; k < 3; ++k) {
                overlaps = overlaps && partition.cull_bounds.min[k] <= hi[k] && partition.cull_bounds.max[k] >= lo[k];
            }
            if (!overlaps) {
                continue;
            }
            for (uint32_t member : partition.members) {
                if (member_partition(member) == p && member_live(member)) {
                    cutting.push_back(member);
                }
            }
        }
        if (cutting.size() > placing.size()) {
            VELK_LOG(I, "partitions cut again by region for a load of %u members: %u members in all",
                     static_cast<unsigned>(placing.size()), static_cast<unsigned>(cutting.size()));
        }
        cut_members(cutting);
        for (uint32_t member : placing) {
            if (InstanceGroup* group = member_group(member)) {
                group->mark_slot(member_slot(member));
            }
        }
    } else {
        for (uint32_t member : placing) {
            dynamic_.members.push_back(member);
            dynamic_.dirty = true;
            if (InstanceGroup* group = member_group(member)) {
                group->movers[member_slot(member)] = update_frame_;
                group->mark_slot(member_slot(member));
            } else {
                records_[member].quiet_frames = 0;
            }
        }
    }

    {
        VELK_PERF_SCOPE("bvh.demote_quiet");
        demote_quiet();
    }
    ++update_frame_;
    // The device-wide cap on instances with places, a quarter of the device's memory, and how
    // many have places now over all visuals loaded by need.
    if (ctx.backend) {
        resident_cap_ = ctx.backend->device_memory_bytes() / 4u / kResidentInstanceBytes;
    }
    resident_total_ = 0;
    uint64_t by_need_total = 0;
    for (const auto& [element, group] : instance_groups_) {
        if (group.visual->loads_by_need()) {
            resident_total_ += group.resident_members;
            by_need_total += group.visual->get_transforms().size();
        }
    }
    // Now and then, how much of what loads by need has its instances in.
    constexpr uint32_t kResidencyLogUpdates = 60;
    if (by_need_total != 0 && update_frame_ % kResidencyLogUpdates == 0) {
        // And the partitions that keep only quads: how many, how many of those hold plain
        // objects, the most and the mean of the levels they keep.
        uint32_t quads_only = 0;
        uint32_t with_plain = 0;
        uint32_t deepest = 0;
        uint64_t levels = 0;
        for (const Partition& partition : partitions_) {
            if (!partition.active || partition.resident) continue;
            ++quads_only;
            levels += partition.stored_level;
            deepest = std::max(deepest, partition.stored_level);
            for (uint32_t member : partition.members) {
                if ((member & kMemberInstance) == 0u && member_partition(member) != kUnplaced) {
                    ++with_plain;
                    break;
                }
            }
        }
        VELK_LOG(I, "residency: %llu of %llu instances in; %u of %u partitions only quads, %u of them with plain objects, "
                    "levels kept up to %u, mean %.1f",
                 static_cast<unsigned long long>(resident_total_), static_cast<unsigned long long>(by_need_total), quads_only,
                 static_cast<unsigned>(partitions_.size()), with_plain, deepest,
                 quads_only != 0 ? double(levels) / double(quads_only) : 0.0);
    }
    // Partitions made in this update are uploaded first, with their sizes and counts, so that
    // residency decides for them now and what they need comes in before the frame is drawn.
    bool any_new = false;
    for (uint32_t p = 0; !any_new && p < partitions_.size(); ++p) {
        any_new = partitions_[p].active && partitions_[p].dirty && partitions_[p].cull_entry == kUnplaced;
    }
    if (any_new) {
        VELK_PERF_SCOPE("renderer.bvh_build");
        flush_partitions(ctx);
    }
    {
        VELK_PERF_SCOPE("bvh.residency");
        update_residency();
    }

    bool any_partition_dirty = dynamic_.dirty || dynamic_transforms_dirty_ || layout_dirty_;
    for (uint32_t p = 0; !any_partition_dirty && p < partitions_.size(); ++p) {
        any_partition_dirty = partitions_[p].active && partitions_[p].dirty;
    }
    if (any_partition_dirty) {
        VELK_PERF_SCOPE("renderer.bvh_build");
        flush_partitions(ctx);
        // Partitions drifted far are cut again by region, and written again.
        if (layout_dirty_) {
            check_recut();
            flush_partitions(ctx);
            layout_dirty_ = false;
        }
        if (dynamic_.dirty) {
            auto& members = dynamic_.members;
            members.erase(std::remove_if(members.begin(), members.end(),
                                         [&](uint32_t member) {
                                             return member_partition(member) != kDynamic || !member_live(member);
                                         }),
                          members.end());
            std::sort(members.begin(), members.end());
            members.erase(std::unique(members.begin(), members.end()), members.end());
        }
        if (dynamic_.dirty || dynamic_transforms_dirty_) {
            if (dynamic_.members.empty()) {
                release_cull(dynamic_, ctx);
            } else {
                upload_partition(dynamic_, ctx);
            }
            dynamic_.dirty = false;
            dynamic_transforms_dirty_ = false;
        }
    }
    // What came in has places in the raster region now, for this frame's draws, and culling
    // finds them through the slot tables: written wherever a place changed, which the slot
    // tables then clear.
    if (ctx.instance_arena) {
        VELK_PERF_SCOPE("bvh.flush_instances");
        for (auto& [element, group] : instance_groups_) {
            if (group.visual->loads_by_need() && (group.slot_table_dirty || group.slot_dirty_from < group.slot_dirty_to)) {
                group.visual->flush_instances(*ctx.instance_arena, *ctx.resources, group.world);
            }
        }
    }
    {
        VELK_PERF_SCOPE("bvh.slot_tables");
        write_slot_tables(ctx);
    }
    // No partition holds a member of a group removed in this update any more.
    retired_groups_.clear();

    for (uint32_t slot : freed_records_) {
        free_records_.push_back(slot);
    }
    freed_records_.clear();
    {
        VELK_PERF_SCOPE("bvh.object_transforms");
        flush_object_transforms(ctx);
    }
}

} // namespace velk::impl
