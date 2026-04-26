#ifndef VELK_UI_SCENE_COLLECTOR_H
#define VELK_UI_SCENE_COLLECTOR_H

#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_light.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_shadow_technique.h>
#include <velk-render/render_types.h>
#include <velk-scene/interface/intf_scene.h>
#include <velk-scene/interface/intf_visual.h>

namespace velk {

// GPU-side shape record. Mirrors the RtShape struct declared in the RT
// compute prelude (and the deferred compute shader). Geometry + material
// + texture + shape discriminator in 128 bytes.
//
// shape_kind:
//   0 = rect, 1 = cube, 2 = sphere — analytic primitives.
//   255 = mesh — triangle soup; `origin/u_axis` carry the world-space
//   AABB and `mesh_data_addr` is a GPU pointer at a MeshData record
//   (see below). For non-mesh kinds `mesh_data_addr` is zero. We
//   deliberately leave 3..254 free for future analytic kinds.
VELK_GPU_STRUCT RtShape
{
    float    origin[4];       ///< xyz = world origin (corner for rect/cube, AABB corner for sphere, AABB min for mesh)
    float    u_axis[4];       ///< xyz = local x axis scaled by width (AABB max for mesh)
    float    v_axis[4];       ///< xyz = local y axis scaled by height
    float    w_axis[4];       ///< xyz = local z axis scaled by depth (cube only; zero otherwise)
    float    color[4];        ///< rgba base color (used when material_id == 0)
    float    params[4];       ///< x = corner radius (rect) or sphere radius; yzw reserved
    uint32_t material_id;     ///< 0 = no material (use color); otherwise dispatched via switch
    uint32_t texture_id;      ///< bindless index, 0 when unused
    uint32_t shape_param;     ///< per-shape material data (e.g. glyph index for text)
    uint32_t shape_kind;      ///< 0 = rect, 1 = cube, 2 = sphere, 255 = mesh
    uint64_t material_data_addr;
    uint64_t mesh_data_addr;  ///< for shape_kind == 255: MeshData*; otherwise 0
};
static_assert(sizeof(RtShape) == 128, "RtShape layout mismatch");

/// Sentinel value of RtShape::shape_kind for triangle-mesh shapes.
inline constexpr uint32_t kRtShapeKindMesh = 255;

// Mesh-static metadata: same for every element instance referencing a
// given IMeshPrimitive. Owned by the primitive (returned via its
// IDrawData::get_data_buffer), so the GPU address is stable across
// frames and shapes can cache it. Mirrors GLSL `MeshStaticData`.
VELK_GPU_STRUCT MeshStaticData
{
    uint64_t buffer_addr;     ///< IMeshBuffer GPU address; same buffer holds VBO + IBO.
    uint32_t vbo_offset;      ///< bytes from buffer_addr to first vertex this primitive uses.
    uint32_t ibo_offset;      ///< bytes from buffer_addr to first index this primitive uses.
    uint32_t triangle_count;
    uint32_t vertex_stride;   ///< bytes per vertex (32 for VelkVertex3D).
    uint32_t blas_root;       ///< root index in the trailing BLAS node array.
    uint32_t blas_node_count; ///< length of the trailing BLAS node array.
};
static_assert(sizeof(MeshStaticData) == 32, "MeshStaticData layout mismatch");

// Per-shape, per-frame mesh instance data. Holds the element's world
// matrices plus a pointer to the mesh-static buffer. SceneBvh writes
// one of these per Mesh-kind shape into the per-frame scratch buffer
// each frame and patches the shape's mesh_data_addr. Mirrors GLSL
// `MeshInstanceData`.
VELK_GPU_STRUCT MeshInstanceData
{
    float    world[16];       ///< column-major mesh-local -> world.
    float    inv_world[16];   ///< column-major world -> mesh-local.
    uint64_t mesh_static_addr;///< MeshStaticData* (persistent; stable across frames).
    uint64_t _pad;
};
static_assert(sizeof(MeshInstanceData) == 144, "MeshInstanceData layout mismatch");

// GPU-side scene light. Mirrors the `Light` struct in the compute shaders
// (80 bytes). flags.y is shadow_tech_id; callers populate it after
// resolving their shadow technique registry.
VELK_GPU_STRUCT GpuLight
{
    uint32_t flags[4];            ///< x = LightType, y = shadow_tech_id, zw = _
    float    position[4];         ///< xyz = world position (point / spot)
    float    direction[4];        ///< xyz = world forward (dir / spot)
    float    color_intensity[4];  ///< rgb = colour, a = intensity multiplier
    float    params[4];           ///< x = range, y = cos(inner), z = cos(outer)
};
static_assert(sizeof(GpuLight) == 80, "GpuLight layout mismatch");

// What enumerate_scene_shapes passes to its callback. `geometry` is
// pre-filled with origin/axes/color/params/shape_kind/shape_param; the
// callback mutates material_id / material_data_addr / texture_id and
// pushes the record into its own output vector.
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

    /// Set when geometry.shape_kind == kRtShapeKindMesh. The renderer
    /// callback fills `mesh_instance.mesh_static_addr` from the
    /// primitive's IDrawData buffer, then writes the instance record
    /// into the per-frame buffer and stamps the resulting GPU address
    /// into geometry.mesh_data_addr.
    MeshInstanceData mesh_instance{};
    /// The mesh primitive backing this shape; used by the renderer
    /// callback to resolve the per-mesh static-data buffer address via
    /// IDrawData::get_data_buffer. Null for non-mesh shapes.
    IMeshPrimitive* mesh_primitive = nullptr;
    bool has_mesh_data = false;

    /// World-space AABB enclosing this shape. Populated by
    /// emit_shapes_for_element so build_scene_bvh can union them into
    /// per-node BVH AABBs (the BVH outer ray_aabb test culls against
    /// these). Independent of geometry.{origin,u_axis,...}, which are
    /// per-kind data (basis vectors for analytic, AABB-min/max for mesh)
    /// that the inner intersect functions consume.
    float aabb_min[3] = { 0, 0, 0 };
    float aabb_max[3] = { 0, 0, 0 };
};

// Scene-wide BVH node. Matches the GLSL `BvhNode` layout (48 bytes)
// from velk.glsl. Leaf nodes carry a contiguous shape range; inner
// nodes carry a contiguous child range. A node can be both: the
// element itself contributes shapes AND has children.
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

struct BvhBuild
{
    vector<RtShape> shapes;
    vector<GpuBvhNode> nodes;
    /// Parallel to `shapes`. For shapes with shape_kind == kRtShapeKindMesh,
    /// contains the populated MeshInstanceData payload (world matrices
    /// + a stable pointer to the mesh's static-data buffer); for other
    /// kinds the entry is zero. SceneBvh re-uploads these per frame and
    /// patches the shapes' mesh_data_addr fields.
    vector<MeshInstanceData> mesh_instances;
    uint32_t root_index = 0;
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

// Emits shape sites for a single element (one per analytic shape or
// one per rect draw entry). Shared by enumerate_scene_shapes (flat
// walk) and build_scene_bvh (tree walk, groups by element).
void emit_shapes_for_element(IElement* element, IRenderContext* ctx,
                             ShapeCb cb, void* user);

// Walks the scene tree and invokes the callback for each geometric
// shape found. Pre-order traversal; ordering matters only for RT's
// painter sort which does its own depth sort afterwards.
void enumerate_scene_shapes(const SceneState& scene_state, IRenderContext* ctx,
                            ShapeCb cb, void* user);

// Walks the scene tree (via IHierarchy) and builds a flat BVH: a
// BFS-ordered node array where each node's children are contiguous,
// plus an RtShape array grouped by emitting element. The callback is
// invoked per shape so callers can fill material/texture fields
// before the shape lands in the output (same contract as
// enumerate_scene_shapes). `scene` must be the scene the elements
// belong to (e.g. looked up from the `IScene*` key the Renderer
// already holds).
BvhBuild build_scene_bvh(IScene* scene, IRenderContext* ctx,
                         ShapeCb cb, void* user);

// Walks all lights in scene_state and invokes the callback for each
// ILight attachment found. `base` has every field populated except
// flags[1] (shadow_tech_id); the callback resolves a shadow technique
// (see find_shadow_technique) and stores the id before pushing.
void enumerate_scene_lights(const SceneState& scene_state,
                            LightCb cb, void* user);

} // namespace velk

#endif // VELK_UI_SCENE_COLLECTOR_H
