#ifndef VELK_UI_SCENE_COLLECTOR_H
#define VELK_UI_SCENE_COLLECTOR_H

#include "view_renderer.h"

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
#include <velk-ui/interface/intf_scene.h>
#include <velk-ui/interface/intf_visual.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace velk::ui {

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

// Emits shape sites for a single element (one per analytic shape or
// one per rect draw entry). Shared by enumerate_scene_shapes (flat
// walk) and build_scene_bvh (tree walk, groups by element).
template <typename F>
void emit_shapes_for_element(IElement* element, IRenderContext* ctx, F&& cb)
{
    auto* storage = interface_cast<IObjectStorage>(element);
    if (!storage) {
        return;
    }
    auto es = read_state<IElement>(element);
    if (!es) return;

    const float ox = es->world_matrix(0, 3);
    const float oy = es->world_matrix(1, 3);
    const float oz = es->world_matrix(2, 3);
    const float ux = es->world_matrix(0, 0);
    const float uy = es->world_matrix(1, 0);
    const float uz = es->world_matrix(2, 0);
    const float vx = es->world_matrix(0, 1);
    const float vy = es->world_matrix(1, 1);
    const float vz = es->world_matrix(2, 1);
    const float wx = es->world_matrix(0, 2);
    const float wy = es->world_matrix(1, 2);
    const float wz = es->world_matrix(2, 2);
    const float ew = es->size.width;
    const float eh = es->size.height;
    const float ed = es->size.depth;

    // Element-level world AABB: works as a sane default for shape
    // kinds whose `geometry` fields are basis vectors, not AABB
    // min/max (i.e. analytics, 2D rects). Mesh shapes overwrite this
    // with their own tight per-primitive world AABB below.
    aabb elem_aabb = es->world_aabb;
    const ::velk::vec3 elem_min = elem_aabb.min();
    const ::velk::vec3 elem_max = elem_aabb.max();

    for (size_t j = 0; j < storage->attachment_count(); ++j) {
        auto* visual = interface_cast<IVisual>(storage->get_attachment(j));
        if (!visual) continue;


        IProgram* paint = nullptr;
        ::velk::color tint = ::velk::color::white();
        if (auto vs2d = read_state<IVisual2D>(visual)) {
            // 2D-authoring affordances (color + paint) live on IVisual2D.
            // 3D visuals have neither; color defaults to white and the
            // material lives on the mesh's first primitive.
            tint = vs2d->color;
            if (vs2d->paint) {
                auto prog_ptr = vs2d->paint.template get<IProgram>();
                paint = prog_ptr.get();
            }
        } else if (auto vs3d = read_state<IVisual3D>(visual)) {
            // 3D visual fallback: reach through IVisual3D::mesh to the
            // first primitive's material. Matches what CubeVisual /
            // SphereVisual stamp onto DrawEntry::material for the raster
            // path; here we feed the same program into the RT shape's
            // material_id resolution so cubes/spheres with authored
            // materials shade correctly in primary and bounce rays.
            auto mesh_obj = vs3d->mesh.template get<IMesh>();
            if (mesh_obj) {
                auto prims = mesh_obj->get_primitives();
                if (prims.size() > 0) {
                    if (auto ps = read_state<IMeshPrimitive>(prims[0].get()); ps && ps->material) {
                        auto prog_ptr = ps->material.template get<IProgram>();
                        paint = prog_ptr.get();
                    }
                }
            }
        }

        auto* analytic = interface_cast<IAnalyticShape>(visual);
        uint32_t shape_kind = analytic ? analytic->get_shape_kind() : 0;

        if (shape_kind != 0) {
            ShapeSite site{};
            site.visual = visual;
            site.paint = paint;
            auto& s = site.geometry;
            s.origin[0] = ox; s.origin[1] = oy; s.origin[2] = oz;
            s.u_axis[0] = ux * ew; s.u_axis[1] = uy * ew; s.u_axis[2] = uz * ew;
            s.v_axis[0] = vx * eh; s.v_axis[1] = vy * eh; s.v_axis[2] = vz * eh;
            s.w_axis[0] = wx * ed; s.w_axis[1] = wy * ed; s.w_axis[2] = wz * ed;
            s.color[0] = tint.r;
            s.color[1] = tint.g;
            s.color[2] = tint.b;
            s.color[3] = tint.a;
            if (shape_kind == 2) {
                s.params[0] = std::min({ew, eh, ed}) * 0.5f;
            }
            s.shape_kind = shape_kind;
            site.aabb_min[0] = elem_min.x; site.aabb_min[1] = elem_min.y; site.aabb_min[2] = elem_min.z;
            site.aabb_max[0] = elem_max.x; site.aabb_max[1] = elem_max.y; site.aabb_max[2] = elem_max.z;
            cb(site);
            continue;
        }

        // 3D visuals that aren't analytic primitives are triangle
        // meshes (e.g. MeshVisual from a glTF import). Emit one
        // Mesh-kind RtShape per IMeshPrimitive, with a MeshData payload
        // pointing at the existing IMeshBuffer's VBO+IBO regions. The
        // renderer callback resolves mesh_data_addr to the GPU address of
        // its uploaded MeshData record.
        if (auto vs3d = read_state<IVisual3D>(visual)) {
            auto mesh_obj = vs3d->mesh.template get<IMesh>();
            if (mesh_obj) {
                const ::velk::mat4& world = es->world_matrix;
                ::velk::mat4 inv_world = ::velk::mat4::inverse(world);
                auto prims = mesh_obj->get_primitives();
                for (auto& prim_ptr : prims) {
                    auto prim_state = read_state<IMeshPrimitive>(prim_ptr.get());
                    if (!prim_state) continue;
                    auto buf = prim_ptr->get_buffer();
                    if (!buf) continue;
                    uint64_t buffer_addr = buf->get_gpu_address();
                    if (buffer_addr == 0) continue;
                    uint32_t v_count = prim_ptr->get_vertex_count();
                    uint32_t i_count = prim_ptr->get_index_count();
                    uint32_t v_stride = prim_ptr->get_vertex_stride();
                    if (v_count == 0 || i_count < 3 || v_stride == 0) continue;
                    if (prim_ptr->get_topology() != MeshTopology::TriangleList) continue;

                    // Per-primitive material (pulled the same way as
                    // analytic CubeVisual / SphereVisual, but per
                    // primitive instead of just first).
                    IProgram* prim_paint = paint;
                    if (prim_state->material) {
                        auto prog_ptr = prim_state->material.template get<IProgram>();
                        if (prog_ptr) prim_paint = prog_ptr.get();
                    }

                    // World-space AABB by transforming the local AABB's
                    // 8 corners through the element world matrix.
                    aabb local_b = prim_ptr->get_bounds();
                    float lminx = local_b.position.x;
                    float lminy = local_b.position.y;
                    float lminz = local_b.position.z;
                    float lmaxx = lminx + local_b.extent.width;
                    float lmaxy = lminy + local_b.extent.height;
                    float lmaxz = lminz + local_b.extent.depth;
                    auto xf = [&](float lx, float ly, float lz) {
                        float wx_ = world(0, 0) * lx + world(0, 1) * ly + world(0, 2) * lz + world(0, 3);
                        float wy_ = world(1, 0) * lx + world(1, 1) * ly + world(1, 2) * lz + world(1, 3);
                        float wz_ = world(2, 0) * lx + world(2, 1) * ly + world(2, 2) * lz + world(2, 3);
                        return ::velk::vec3{wx_, wy_, wz_};
                    };
                    ::velk::vec3 c[8] = {
                        xf(lminx, lminy, lminz), xf(lmaxx, lminy, lminz),
                        xf(lminx, lmaxy, lminz), xf(lmaxx, lmaxy, lminz),
                        xf(lminx, lminy, lmaxz), xf(lmaxx, lminy, lmaxz),
                        xf(lminx, lmaxy, lmaxz), xf(lmaxx, lmaxy, lmaxz),
                    };
                    float wmin[3] = {c[0].x, c[0].y, c[0].z};
                    float wmax[3] = {c[0].x, c[0].y, c[0].z};
                    for (int k = 1; k < 8; ++k) {
                        wmin[0] = std::min(wmin[0], c[k].x); wmax[0] = std::max(wmax[0], c[k].x);
                        wmin[1] = std::min(wmin[1], c[k].y); wmax[1] = std::max(wmax[1], c[k].y);
                        wmin[2] = std::min(wmin[2], c[k].z); wmax[2] = std::max(wmax[2], c[k].z);
                    }

                    ShapeSite site{};
                    site.visual = visual;
                    site.paint = prim_paint;
                    auto& s = site.geometry;
                    s.origin[0] = wmin[0]; s.origin[1] = wmin[1]; s.origin[2] = wmin[2];
                    s.u_axis[0] = wmax[0]; s.u_axis[1] = wmax[1]; s.u_axis[2] = wmax[2];
                    s.color[0] = tint.r; s.color[1] = tint.g; s.color[2] = tint.b; s.color[3] = tint.a;
                    s.shape_kind = kRtShapeKindMesh;
                    site.aabb_min[0] = wmin[0]; site.aabb_min[1] = wmin[1]; site.aabb_min[2] = wmin[2];
                    site.aabb_max[0] = wmax[0]; site.aabb_max[1] = wmax[1]; site.aabb_max[2] = wmax[2];

                    // Per-frame instance data: world matrices + a
                    // pointer placeholder. The renderer callback fills
                    // mesh_static_addr from the primitive's persistent
                    // IDrawData buffer (stable across frames). Static
                    // mesh metadata (buffer_addr, offsets, counts,
                    // stride) lives in that buffer — not duplicated
                    // here.
                    auto& mi = site.mesh_instance;
                    std::memcpy(mi.world,     world.m,     sizeof(mi.world));
                    std::memcpy(mi.inv_world, inv_world.m, sizeof(mi.inv_world));
                    mi.mesh_static_addr = 0;  // resolved by the renderer cb.
                    site.mesh_primitive = prim_ptr.get();
                    site.has_mesh_data = true;
                    (void)buffer_addr;  // validated above; no longer carried inline.

                    cb(site);
                }
            }
            continue;
        }

        float radius = 0.f;
        if (analytic && !analytic->get_shape_intersect_source().empty()) {
            radius = std::min(std::min(ew, eh) * 0.5f, 12.f);
        }

        if (!ctx) continue;
        ::velk::size local_size{ew, eh, ed};
        auto entries = visual->get_draw_entries(*ctx, local_size);
        for (auto& dentry : entries) {
            // Instance layout — ElementInstance (see instance_types.h):
            //   [  0.. 63] mat4 world_matrix (raster only; zero here)
            //   [ 64.. 79] vec4 offset  (xy = local offset; z pad)
            //   [ 80.. 95] vec4 size    (xy = local extents; z pad)
            //   [ 96..111] vec4 color
            //   [112..115] uint shape_param (glyph index etc.)
            constexpr size_t kOffsetOff = 64;
            constexpr size_t kSizeOff   = 80;
            constexpr size_t kColorOff  = 96;
            constexpr size_t kParamsOff = 112;
            if (dentry.instance_size < kSizeOff + 8) continue;

            float local_px = 0, local_py = 0, sz_w = 0, sz_h = 0;
            std::memcpy(&local_px, dentry.instance_data + kOffsetOff,     4);
            std::memcpy(&local_py, dentry.instance_data + kOffsetOff + 4, 4);
            std::memcpy(&sz_w,     dentry.instance_data + kSizeOff,       4);
            std::memcpy(&sz_h,     dentry.instance_data + kSizeOff + 4,   4);

            float cr = tint.r, cg = tint.g, cb_ = tint.b, ca = tint.a;
            if (dentry.instance_size >= kColorOff + 16) {
                std::memcpy(&cr,  dentry.instance_data + kColorOff,      4);
                std::memcpy(&cg,  dentry.instance_data + kColorOff + 4,  4);
                std::memcpy(&cb_, dentry.instance_data + kColorOff + 8,  4);
                std::memcpy(&ca,  dentry.instance_data + kColorOff + 12, 4);
            }

            uint32_t shape_param = 0;
            if (dentry.instance_size >= kParamsOff + 4) {
                std::memcpy(&shape_param, dentry.instance_data + kParamsOff, 4);
            }

            ShapeSite site{};
            site.visual = visual;
            site.paint = paint;
            site.draw_entry = &dentry;
            auto& s = site.geometry;
            s.origin[0] = ox + ux * local_px + vx * local_py;
            s.origin[1] = oy + uy * local_px + vy * local_py;
            s.origin[2] = oz + uz * local_px + vz * local_py;
            s.u_axis[0] = ux * sz_w; s.u_axis[1] = uy * sz_w; s.u_axis[2] = uz * sz_w;
            s.v_axis[0] = vx * sz_h; s.v_axis[1] = vy * sz_h; s.v_axis[2] = vz * sz_h;
            s.color[0] = cr; s.color[1] = cg; s.color[2] = cb_; s.color[3] = ca;
            s.params[0] = radius;
            s.shape_param = shape_param;
            site.aabb_min[0] = elem_min.x; site.aabb_min[1] = elem_min.y; site.aabb_min[2] = elem_min.z;
            site.aabb_max[0] = elem_max.x; site.aabb_max[1] = elem_max.y; site.aabb_max[2] = elem_max.z;
            cb(site);
        }
    }
}

namespace detail {

// Pre-order DFS walk of the scene tree. `on_element` fires for each
// IElement encountered, before descending into its children. Children
// are visited in their natural storage order - callers that care
// about draw z-order should sort at each level themselves.
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

} // namespace detail

// Walks the scene tree and invokes `cb(ShapeSite&)` for each geometric
// shape found. Uses tree traversal (no flat visual_list dependency).
// Returns shapes in pre-order; ordering matters only for RT's painter
// sort which does its own depth sort afterwards.
template <typename F>
void enumerate_scene_shapes(const SceneState& scene_state, IRenderContext* ctx, F&& cb)
{
    if (!scene_state.scene) return;
    detail::walk_scene_preorder(scene_state.scene, scene_state.scene->root(),
        [&](IElement* elem) { emit_shapes_for_element(elem, ctx, cb); });
}

// Walks the scene tree (via IHierarchy) and builds a flat BVH: a
// BFS-ordered node array where each node's children are contiguous,
// plus an RtShape array grouped by emitting element. The callback is
// invoked per shape so callers can fill material/texture fields
// before the shape lands in the output (same contract as
// enumerate_scene_shapes). `scene` must be the scene the elements
// belong to (e.g. looked up from the `IScene*` key the Renderer
// already holds).
template <typename F>
BvhBuild build_scene_bvh(IScene* scene, IRenderContext* ctx, F&& cb)
{
    BvhBuild out;
    if (!scene) return out;

    auto root_obj = scene->root();
    auto root_elem = interface_pointer_cast<IElement>(root_obj);
    if (!root_elem) return out;

    // Step 1: collect all shapes (and their world AABBs + mesh
    // instance data) into flat arrays via a scene tree walk. The walk
    // order itself doesn't matter; we re-partition spatially below.
    vector<RtShape>          shapes;
    vector<MeshInstanceData> mesh_instances;
    vector<float>            shape_aabb_min; // 3 floats per shape
    vector<float>            shape_aabb_max;
    detail::walk_scene_preorder(scene, scene->root(), [&](IElement* elem) {
        emit_shapes_for_element(elem, ctx, [&](ShapeSite& site) {
            cb(site);
            shapes.push_back(site.geometry);
            mesh_instances.push_back(
                site.has_mesh_data ? site.mesh_instance : MeshInstanceData{});
            shape_aabb_min.push_back(site.aabb_min[0]);
            shape_aabb_min.push_back(site.aabb_min[1]);
            shape_aabb_min.push_back(site.aabb_min[2]);
            shape_aabb_max.push_back(site.aabb_max[0]);
            shape_aabb_max.push_back(site.aabb_max[1]);
            shape_aabb_max.push_back(site.aabb_max[2]);
        });
    });

    if (shapes.empty()) {
        // Empty BVH: emit a single inverted-AABB root so trace_any_hit
        // sees node_count > 0 but every ray rejects.
        out.nodes.push_back({});
        out.root_index = 0;
        out.nodes[0].aabb_min[0] = 1.f; out.nodes[0].aabb_max[0] = -1.f;
        return out;
    }

    // Step 2: build a spatial median-split binary BVH over the flat
    // shape list. The previous scene-graph BVH had bistro-style scenes
    // putting thousands of shapes as direct children of one root node;
    // any iterative BVH walker with a fixed traversal stack (32 / 128)
    // silently dropped most of them, making large swaths of geometry
    // invisible to RT shadows. A binary tree with leaf threshold 4
    // bounds depth at ceil(log2(N/4)) — well under 32 for any
    // realistic shape count.
    const uint32_t shape_count = static_cast<uint32_t>(shapes.size());
    struct ShapeRef
    {
        uint32_t index;        // index into the input arrays
        float    centroid[3];
    };
    vector<ShapeRef> refs;
    refs.resize(shape_count);
    for (uint32_t i = 0; i < shape_count; ++i) {
        refs[i].index = i;
        for (int k = 0; k < 3; ++k) {
            refs[i].centroid[k] =
                0.5f * (shape_aabb_min[i * 3 + k] + shape_aabb_max[i * 3 + k]);
        }
    }

    // Recursive top-down builder. Returns the index of the node it
    // wrote into out.nodes. Reorders refs[begin..end) so leaf shape
    // ranges are contiguous in the output.
    constexpr uint32_t kLeafThreshold = 4;
    constexpr float    kInf = 1e30f;

    // Reorder buffers (final shape order = leaf-traversal order).
    vector<RtShape>          out_shapes;
    vector<MeshInstanceData> out_mesh_instances;
    out_shapes.reserve(shape_count);
    out_mesh_instances.reserve(shape_count);

    // We can't easily express recursion on a templated function inside
    // another, so use an explicit work stack instead.
    struct BuildItem
    {
        uint32_t self_index;
        size_t   begin;
        size_t   end;
    };
    out.nodes.push_back({});
    out.root_index = 0;
    vector<BuildItem> work;
    work.push_back({0u, 0u, refs.size()});

    while (!work.empty()) {
        BuildItem item = work.back();
        work.pop_back();

        // Compute this node's AABB (union of contained shape AABBs).
        float bmin[3] = { kInf,  kInf,  kInf};
        float bmax[3] = {-kInf, -kInf, -kInf};
        for (size_t i = item.begin; i < item.end; ++i) {
            const uint32_t idx = refs[i].index;
            for (int k = 0; k < 3; ++k) {
                if (shape_aabb_min[idx * 3 + k] < bmin[k]) bmin[k] = shape_aabb_min[idx * 3 + k];
                if (shape_aabb_max[idx * 3 + k] > bmax[k]) bmax[k] = shape_aabb_max[idx * 3 + k];
            }
        }
        GpuBvhNode& node = out.nodes[item.self_index];
        for (int k = 0; k < 3; ++k) {
            node.aabb_min[k] = bmin[k];
            node.aabb_max[k] = bmax[k];
        }
        node.aabb_min[3] = 0.f;
        node.aabb_max[3] = 0.f;

        const size_t count = item.end - item.begin;

        auto emit_leaf = [&]() {
            const uint32_t first = static_cast<uint32_t>(out_shapes.size());
            for (size_t i = item.begin; i < item.end; ++i) {
                const uint32_t idx = refs[i].index;
                out_shapes.push_back(shapes[idx]);
                out_mesh_instances.push_back(mesh_instances[idx]);
            }
            node.first_shape = first;
            node.shape_count = static_cast<uint32_t>(count);
            node.first_child = 0;
            node.child_count = 0;
        };

        if (count <= kLeafThreshold) {
            emit_leaf();
            continue;
        }

        // Pick split axis = longest centroid extent.
        float cmin[3] = { kInf,  kInf,  kInf};
        float cmax[3] = {-kInf, -kInf, -kInf};
        for (size_t i = item.begin; i < item.end; ++i) {
            for (int k = 0; k < 3; ++k) {
                if (refs[i].centroid[k] < cmin[k]) cmin[k] = refs[i].centroid[k];
                if (refs[i].centroid[k] > cmax[k]) cmax[k] = refs[i].centroid[k];
            }
        }
        int axis = 0;
        const float dx = cmax[0] - cmin[0];
        const float dy = cmax[1] - cmin[1];
        const float dz = cmax[2] - cmin[2];
        if (dy >= dx && dy >= dz)      axis = 1;
        else if (dz >= dx && dz >= dy) axis = 2;

        if (cmax[axis] == cmin[axis]) {
            // All centroids coincide on the chosen axis: any sort
            // would oscillate forever. Fall back to a leaf even if
            // count > leaf threshold; the leaf is just slower to test
            // but still correct.
            emit_leaf();
            continue;
        }

        std::sort(refs.begin() + item.begin, refs.begin() + item.end,
                  [axis](const ShapeRef& a, const ShapeRef& b) {
                      return a.centroid[axis] < b.centroid[axis];
                  });
        const size_t mid = item.begin + count / 2;

        const uint32_t left_idx  = static_cast<uint32_t>(out.nodes.size());
        out.nodes.push_back({});
        const uint32_t right_idx = static_cast<uint32_t>(out.nodes.size());
        out.nodes.push_back({});

        // Re-acquire the node reference: the push_backs above may have
        // reallocated `out.nodes`, dangling the `node` reference taken
        // earlier (line `out.nodes[item.self_index]`). The AABB writes
        // earlier in this iteration landed in the pre-reallocation
        // buffer and were copied to the new buffer, so they're fine.
        // The first_child / child_count writes below MUST go through
        // a fresh reference; otherwise they hit freed memory and the
        // actual node stays with zero-initialised children — making
        // it look like an empty leaf and dropping every shape under
        // that subtree from the BVH walk.
        GpuBvhNode& self = out.nodes[item.self_index];
        self.first_shape = 0;
        self.shape_count = 0;
        self.first_child = left_idx;
        self.child_count = 2;

        // Push right first so left is processed first (cheaper for the
        // refs array's cache locality on the next iteration).
        work.push_back({right_idx, mid,        item.end});
        work.push_back({left_idx,  item.begin, mid});
    }

    out.shapes         = std::move(out_shapes);
    out.mesh_instances = std::move(out_mesh_instances);
    return out;
}

// Walks all lights in scene_state and invokes `cb(LightSite&)` for each
// ILight attachment found. `base` has every field populated except
// flags[1] (shadow_tech_id); the callback resolves a shadow technique
// (see find_shadow_technique) and stores the id before pushing.
template <typename F>
void enumerate_scene_lights(const SceneState& scene_state, F&& cb)
{
    constexpr float kDegToRad = 0.017453292519943295f;

    if (!scene_state.scene) return;
    detail::walk_scene_preorder(scene_state.scene, scene_state.scene->root(),
        [&](IElement* elem) {
            auto es = read_state<IElement>(elem);
            if (!es) return;
            auto* storage = interface_cast<IObjectStorage>(elem);
            if (!storage) return;

            for (size_t j = 0; j < storage->attachment_count(); ++j) {
                auto* light = interface_cast<ILight>(storage->get_attachment(j));
                if (!light) continue;
                auto ls = read_state<ILight>(light);
                if (!ls) continue;

                LightSite site{};
                site.element = elem;
                site.light = light;
                auto& g = site.base;
                g.flags[0] = static_cast<uint32_t>(ls->type);
                g.position[0] = es->world_matrix(0, 3);
                g.position[1] = es->world_matrix(1, 3);
                g.position[2] = es->world_matrix(2, 3);
                float fx = -es->world_matrix(0, 2);
                float fy = -es->world_matrix(1, 2);
                float fz = -es->world_matrix(2, 2);
                float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
                if (flen > 1e-6f) { fx /= flen; fy /= flen; fz /= flen; }
                g.direction[0] = fx;
                g.direction[1] = fy;
                g.direction[2] = fz;
                g.color_intensity[0] = ls->color.r;
                g.color_intensity[1] = ls->color.g;
                g.color_intensity[2] = ls->color.b;
                g.color_intensity[3] = ls->intensity;
                g.params[0] = ls->range;
                g.params[1] = std::cos(ls->cone_inner_deg * kDegToRad);
                g.params[2] = std::cos(ls->cone_outer_deg * kDegToRad);
                cb(site);
            }
        });
}

} // namespace velk::ui

#endif // VELK_UI_SCENE_COLLECTOR_H
