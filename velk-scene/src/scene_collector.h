#ifndef VELK_UI_SCENE_COLLECTOR_H
#define VELK_UI_SCENE_COLLECTOR_H

#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-render/blas.h>
#include <velk-render/frame/render_view.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_light.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_shader_source.h>
#include <velk-render/interface/intf_shadow_technique.h>
#include <velk-render/render_types.h>
#include <velk-scene/interface/intf_scene.h>
#include <velk-scene/interface/intf_visual.h>

namespace velk {

// RtShape, RtRecord, MeshStaticData, GpuLight live in
// velk-render/gpu_data.h alongside other GPU types they're paired with.

// What enumerate_scene_shapes passes to its callback. `geometry` is
// pre-filled with origin/axes/color/params/shape_kind/shape_param, in the
// element's local space; the callback mutates material_id / material_base /
// texture_id and pushes the record into its own output vector.
//
// `draw_entry` is non-null for rect-path shapes (one call per draw
// entry from IVisual::get_draw_entries) and null for analytic shapes
// (cube / sphere, one call per visual).
struct ShapeSite
{
    RtShape geometry;
    IVisual* visual = nullptr;
    IProgram* paint = nullptr;
    const DrawEntry* draw_entry = nullptr;

    /// The owning element's world matrix, which `geometry` is local to. Valid
    /// for the duration of the callback.
    const mat4* world = nullptr;
    /// Consecutive shapes with the same non-null value share one record, like a
    /// text visual's glyphs. Null gives the shape its own record.
    const void* record_group = nullptr;

    /// The mesh primitive backing this shape; the callback publishes the
    /// per-mesh static data through IMeshPrimitiveInternal::ensure_rt_data and
    /// stores its index in geometry.mesh_static_base. Null for non-mesh shapes.
    IMeshPrimitive* mesh_primitive = nullptr;
    bool has_mesh_data = false;

    /// World-space AABB enclosing this shape, which SceneBvh unions into
    /// per-node BVH AABBs (the BVH outer ray_aabb test culls against
    /// these). Independent of geometry.{origin,u_axis,...}, which are
    /// local per-kind data (basis vectors for analytic, AABB-min/max for
    /// mesh) that the inner intersect functions consume.
    float aabb_min[3] = { 0, 0, 0 };
    float aabb_max[3] = { 0, 0, 0 };
};

/// Builds a record for shapes local to @p world. Its shape fields are left for
/// the caller.
inline RtRecord make_rt_record(const mat4& world)
{
    const mat4 inverse = mat4::inverse(world);
    RtRecord record{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            record.inv_row[row][col] = inverse(row, col);
        }
        record.inv_row[row][3] = world(row, 3);
    }
    record.tree_root = kRtRecordNoTree;
    return record;
}

/// The world-space plane of @p site's shape. A mesh has no plane, so it gets its
/// bounds' minimum corner and zero axes.
inline RenderView::ShapePlane world_shape_plane(const ShapeSite& site)
{
    RenderView::ShapePlane plane{};
    if (site.geometry.shape_kind == kRtShapeKindMesh) {
        for (int row = 0; row < 3; ++row) {
            plane.origin[row] = site.aabb_min[row];
        }
        return plane;
    }
    const RtShape& shape = site.geometry;
    const mat4& world = *site.world;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            plane.origin[row] += world(row, col) * shape.origin[col];
            plane.u_axis[row] += world(row, col) * shape.u_axis[col];
            plane.v_axis[row] += world(row, col) * shape.v_axis[col];
        }
        plane.origin[row] += world(row, 3);
    }
    return plane;
}

/// GpuBvhNode::child_count of a node that refers to a partition. Its first_child is
/// the partition's root, and its origin is in aabb_min[3], aabb_max[3] and the bits
/// of first_shape. Mirrors GLSL VELK_PARTITION_REF.
inline constexpr uint32_t kBvhPartitionRef = 0xFFFFFFFFu;

// Scene-wide BVH node. Matches the GLSL `BvhNode` layout (48 bytes)
// from velk.glsl. Leaf nodes carry a contiguous record range; inner
// nodes carry a contiguous child range.
VELK_GPU_STRUCT GpuBvhNode
{
    float    aabb_min[4];
    float    aabb_max[4];
    uint32_t first_shape;
    uint32_t shape_count;
    uint32_t first_child;
    uint32_t child_count;
};
static_assert(sizeof(GpuBvhNode) == 48, "GpuBvhNode layout mismatch");

/// World-space bounds of one item in a tree build.
struct BvhItemBounds
{
    float min[3];
    float max[3];
};

/// A binary tree with its root at index 0. Indices are relative: children to
/// `nodes`, and a leaf's first_shape to `order`, which lists the item indices in
/// leaf order.
struct BvhTree
{
    vector<GpuBvhNode> nodes;
    vector<uint32_t> order;
};

// What enumerate_scene_lights passes to its callback. `base` has every
// field set except flags[1] (shadow_tech_id); the callback resolves any
// attached IShadowTechnique, assigns the id, and pushes the record.
struct LightSite
{
    GpuLight base;
    IElement* element = nullptr;
    ILight* light = nullptr;
};

// Returns the first IShadowTechnique attached to the given light, or
// nullptr if none. Both RayTracer and DeferredLighter resolve lights to
// a shadow tech this way; the lookup is centralised here.
inline IShadowTechnique::Ptr find_shadow_technique(ILight* light)
{
    return ::velk::find_attachment<IShadowTechnique>(light);
}

// C-style callback signatures. Callers wrap a stateful lambda in a
// stateless thunk that recovers the lambda from `user`, e.g.:
//
//   struct State { Renderer* self; FrameContext& ctx; };
//   State s{this, ctx};
//   enumerate_scene_lights(scene_state,
//       +[](void* u, LightSite& site) {
//           auto& s = *static_cast<State*>(u);
//           // ... use s.self, s.ctx, site ...
//       }, &s);
//
// We avoid std::function here to keep the call sites heap-allocation-free
// and to let the implementations live in a .cpp without per-instantiation
// template bodies in every translation unit.
using ShapeCb = void (*)(void* user, ShapeSite& site);
using LightCb = void (*)(void* user, LightSite& site);

// Pre-order DFS walk of the scene tree. `on_element` fires for each
// IElement encountered, before descending into its children. Children
// are visited in their natural storage order; callers that care about
// draw z-order should sort at each level themselves.
template <typename F>
void walk_scene_preorder(IScene* scene, const IObject::Ptr& obj, F&& on_element)
{
    if (auto* elem = interface_cast<IElement>(obj)) {
        on_element(elem);
        if (scene) {
            for (auto& kid : scene->children_of(obj)) {
                walk_scene_preorder(scene, kid, on_element);
            }
        }
    }
}

// Emits shape sites for a single element (one per analytic shape or
// one per rect draw entry). Shared by enumerate_scene_shapes and SceneBvh.
void emit_shapes_for_element(IElement* element, IRenderContext* ctx,
                             ShapeCb cb, void* user);

// Walks the scene tree and invokes the callback for each geometric
// shape found. Pre-order traversal; ordering matters only for RT's
// painter sort which does its own depth sort afterwards.
void enumerate_scene_shapes(const SceneState& scene_state, IRenderContext* ctx,
                            ShapeCb cb, void* user);

/// Builds an SAH tree over @p items. Subtrees of at most @p leaf_size items become
/// leaves. @p force_split keeps splitting down to @p leaf_size even where SAH prefers
/// a larger leaf or the items' centroids coincide.
BvhTree build_bvh_tree(array_view<const BvhItemBounds> items, uint32_t leaf_size = 1,
                       bool force_split = false);

/// Reorders flat rect @p shapes into a tree's leaf order and returns the tree in
/// @p tree, with its root at index 0 and indices relative to @p tree and @p shapes.
void build_local_tree(vector<RtShape>& shapes, vector<BlasNode>& tree);

// Walks all lights in scene_state and invokes the callback for each
// ILight attachment found. `base` has every field populated except
// flags[1] (shadow_tech_id); the callback resolves a shadow technique
// (see find_shadow_technique) and stores the id before pushing.
void enumerate_scene_lights(const SceneState& scene_state,
                            LightCb cb, void* user);

} // namespace velk

#endif // VELK_UI_SCENE_COLLECTOR_H
