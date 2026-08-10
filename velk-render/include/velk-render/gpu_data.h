#ifndef VELK_RENDER_GPU_DATA_H
#define VELK_RENDER_GPU_DATA_H

#include <cstdint>

namespace velk {

/// @file gpu_data.h
/// Framework-level GPU data structures.
///
/// The universal per-instance type (`ElementInstance`) lives in
/// `velk-ui/instance_types.h`; visuals fill its fields and the
/// renderer's batch builder writes the world matrix.

/// Declares a struct with std430-compatible alignment (16 bytes).
/// Use for material GPU data structs that follow the DrawDataHeader.
/// The compiler pads the struct automatically, no manual _pad fields needed.
#define VELK_GPU_STRUCT struct alignas(16)

/// Renderer-side description of how a view reaches the scene BVH for
/// index-based shader reads. Carried through RenderView / FrameContext and
/// stamped (flat) into FrameGlobals / RtRoot. @c node_base / @c shape_base
/// locate the BVH's regions in the shared node / shape arenas; @c root and
/// the counts are relative to them. Not uploaded directly; the GPU structs
/// mirror these fields individually.
struct BvhBinding
{
    uint32_t root        = 0;
    uint32_t node_count  = 0;
    uint32_t shape_count = 0;
    uint32_t node_base   = 0;
    uint32_t shape_base  = 0;

    bool operator==(const BvhBinding& o) const
    {
        return root == o.root && node_count == o.node_count
            && shape_count == o.shape_count && node_base == o.node_base
            && shape_base == o.shape_base;
    }
    bool operator!=(const BvhBinding& o) const { return !(*this == o); }
};

/// Per-frame global data written by the renderer, read by all shaders.
///
/// Layout must match the `GlobalData` struct declaration in velk.glsl
/// (scalar layout). The view preparer writes one of these per view per
/// frame into its persistent region of the shared globals arena
/// (set = 1 slot 2); shaders read it as `velk_globals.data[base]`.
struct FrameGlobals
{
    float    view_projection[16];          ///< Combined view-projection matrix from the camera.
    float    inverse_view_projection[16];  ///< Inverse of view_projection.
    float    viewport[4];                  ///< width, height, 1/width, 1/height.
    float    cam_pos[4];                   ///< World-space camera position (xyz) + pad.
    uint32_t bvh_root;                     ///< Index of the root BvhNode, relative to bvh_node_base; 0 if no BVH.
    uint32_t bvh_node_count;               ///< Total BvhNodes; 0 if no BVH.
    uint32_t bvh_shape_count;              ///< Total RtShapes the BVH indexes.
    uint32_t present_counter;              ///< Monotonic CPU frame index (RT noise seed; never a GPU-completion proxy).
    uint32_t bvh_node_base;                ///< Element base added to BVH node indices (this BVH's region of the node arena).
    uint32_t bvh_shape_base;               ///< Element base added to BVH shape indices.
    float    prev_view_projection[16];     ///< Previous frame's view-projection (identity on the first frame). For temporal reprojection.
};

static_assert(sizeof(FrameGlobals) == 248, "FrameGlobals layout must match velk.glsl");

/**
 * @brief Standard draw data header at the start of every draw's GPU data.
 *
 * 32 bytes via VELK_GPU_STRUCT (16-byte aligned for std430). Every field is
 * now an index or a count: the alignment pads existed only to keep the two
 * 8-byte vertex-stream addresses aligned, and went with them.
 *
 * Material data does not trail the header: it lives in the set = 1 material
 * arena, indexed by @c material_base (region offset / material record size).
 *
 * Multi-texture materials (e.g. StandardMaterial) embed their bindless
 * TextureIds directly in their own UBO via ITextureResolver, so the
 * header carries only the single texture_id used by simple visuals
 * (image, texture, text).
 */
VELK_GPU_STRUCT DrawDataHeader
{
    uint32_t globals_base;      ///< Index into the set = 1 globals buffer; shaders read velk_globals.data[globals_base].
    uint32_t instances_base;    ///< Element base into the set = 1 instance arena; shader reads velk_instances.data[instances_base + gl_InstanceIndex].
    uint32_t texture_id;        ///< Bindless texture index (0 = none).
    uint32_t instance_count;    ///< Number of instances in this draw.
    uint32_t vbo_base;          ///< Word base of the draw's vertex stream in the set = 1 mesh-word arena.
    uint32_t uv1_base;          ///< Word base of the draw's TEXCOORD_1 stream, or of a context-owned single-vertex fallback when @c uv1_enabled is 0.
    uint32_t uv1_enabled;       ///< 1 = per-vertex UV1 stream at @c uv1_base; 0 = fallback, vertex shader reads vertex 0 only. Used as a branchless index multiplier in the vertex shader.
    uint32_t material_base;     ///< Element base into the set = 1 material arena; fragment shader reads velk_materials.data[material_base].
};

static_assert(sizeof(DrawDataHeader) == 32, "DrawDataHeader must be 32 bytes for std430 alignment");

// ===== Scene-data GPU structs =====
// Mirrors of GLSL types consumed by RT and deferred compute shaders.
// Plain POD, no scene deps — packed for std430 indexed reads.

/// GPU-side shape record. Mirrors the RtShape struct in the RT compute
/// prelude and the deferred lighting compute. Geometry + material +
/// texture + shape discriminator in 128 bytes.
///
/// shape_kind:
///   0 = rect, 1 = cube, 2 = sphere — analytic primitives.
///   255 = mesh — triangle soup; `origin/u_axis` carry the world-space
///   AABB and `mesh_instance_base` indexes the mesh-instance arena.
///   3..254 reserved for future analytic kinds.
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
    uint32_t material_base;   ///< Word index of this shape's record in the set = 1 material arena.
    uint32_t _pad0;
    uint32_t mesh_instance_base; ///< for shape_kind == 255: element index into the set = 1 mesh-instance arena; otherwise 0
    uint32_t _pad1;
};
static_assert(sizeof(RtShape) == 128, "RtShape layout mismatch");

/// Sentinel value of RtShape::shape_kind for triangle-mesh shapes.
inline constexpr uint32_t kRtShapeKindMesh = 255;

/// Mesh-static metadata: same for every element instance referencing a
/// given IMeshPrimitive. The primitive owns a persistent region of the
/// shared mesh-static arena, so the element index is stable across frames
/// and instances can cache it. The BLAS it describes lives in its own
/// arenas, reached by `blas_node_base` / `blas_tri_base` rather than by
/// trailing this record. Mirrors GLSL `MeshStaticData`.
VELK_GPU_STRUCT MeshStaticData
{
    uint32_t vbo_base;        ///< word base of this primitive's first vertex in the mesh-word arena.
    uint32_t ibo_base;        ///< word base of this primitive's first index in the mesh-word arena.
    uint32_t triangle_count;
    uint32_t vertex_stride;   ///< bytes per vertex, from the primitive (48 for VelkVertex3D).
    uint32_t blas_root;       ///< root index within this primitive's BLAS node run.
    uint32_t blas_node_count; ///< length of this primitive's BLAS node run; 0 = no BLAS.
    uint32_t blas_node_base;  ///< element base of the node run in the BLAS node arena.
    uint32_t blas_tri_base;   ///< element base of the triangle-index run in the BLAS triangle arena.
};
static_assert(sizeof(MeshStaticData) == 32, "MeshStaticData layout mismatch");

/// Sentinel `MeshInstanceData::mesh_static_base` for a mesh whose static
/// record is not resolvable yet (geometry not uploaded, no BLAS built).
/// The intersector skips the shape; the CPU retries on a later frame.
inline constexpr uint32_t kInvalidMeshStaticBase = 0xFFFFFFFFu;

/// Per-shape mesh instance data. Holds the element's world matrices plus
/// a pointer to the mesh-static buffer. Lives in the set = 1 mesh-instance
/// arena; shapes reach their record by `mesh_instance_base`. Mirrors GLSL
/// `MeshInstanceData`.
VELK_GPU_STRUCT MeshInstanceData
{
    float    world[16];       ///< column-major mesh-local -> world.
    float    inv_world[16];   ///< column-major world -> mesh-local.
    uint32_t mesh_static_base;///< element index into the mesh-static arena; kInvalidMeshStaticBase when unresolved.
    uint32_t _pad0;
    uint32_t _pad1[2];        ///< keeps the record at 144 bytes.
};
static_assert(sizeof(MeshInstanceData) == 144, "MeshInstanceData layout mismatch");

/// GPU-side scene light. Mirrors the `Light` struct in the compute
/// shaders (80 bytes). `flags.y` is shadow_tech_id; callers populate
/// it after resolving their shadow technique registry.
VELK_GPU_STRUCT GpuLight
{
    uint32_t flags[4];            ///< x = LightType, y = shadow_tech_id, zw = _
    float    position[4];         ///< xyz = world position (point / spot)
    float    direction[4];        ///< xyz = world forward (dir / spot)
    float    color_intensity[4];  ///< rgb = colour, a = intensity multiplier
    float    params[4];           ///< x = range, y = cos(inner), z = cos(outer), w = light size (dir: angular radius rad; point/spot: world radius)
};
static_assert(sizeof(GpuLight) == 80, "GpuLight layout mismatch");

} // namespace velk

#endif // VELK_RENDER_GPU_DATA_H
