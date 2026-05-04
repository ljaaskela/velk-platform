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

/// Per-frame global data written by the renderer, read by all shaders.
///
/// Layout must match the `ViewGlobalsBuffer` UBO declaration in
/// velk.glsl (scalar layout). The view preparer writes one of these
/// per view per frame into the per-frame staging buffer; the graph
/// executor binds the relevant region as descriptor binding 4 before
/// each pass, and shaders read view-level state (camera, viewport,
/// BVH, present_counter) from `view_globals.X`.
struct FrameGlobals
{
    float    view_projection[16];          ///< Combined view-projection matrix from the camera.
    float    inverse_view_projection[16];  ///< Inverse of view_projection.
    float    viewport[4];                  ///< width, height, 1/width, 1/height.
    float    cam_pos[4];                   ///< World-space camera position (xyz) + pad.
    uint32_t bvh_root;                     ///< Index of the root BvhNode; 0 if no BVH.
    uint32_t bvh_node_count;               ///< Total BvhNodes; 0 if no BVH.
    uint32_t bvh_shape_count;              ///< Total RtShapes the BVH indexes.
    uint32_t present_counter;              ///< Monotonic CPU frame index (RT noise seed; never a GPU-completion proxy).
    uint64_t bvh_nodes_addr;               ///< GPU pointer to the BvhNode array.
    uint64_t bvh_shapes_addr;              ///< GPU pointer to the scene's RtShape array.
};

static_assert(sizeof(FrameGlobals) == 192, "FrameGlobals layout must match velk.glsl");

/**
 * @brief Standard draw data header at the start of every draw's GPU data.
 *
 * 48 bytes via VELK_GPU_STRUCT (16-byte aligned for std430) so material
 * data that follows can begin at offset 48 with its own alignment intact.
 *
 * Multi-texture materials (e.g. StandardMaterial) embed their bindless
 * TextureIds directly in their own UBO via ITextureResolver, so the
 * header carries only the single texture_id used by simple visuals
 * (image, texture, text).
 */
VELK_GPU_STRUCT DrawDataHeader
{
    uint64_t _reserved0;        ///< Reserved 8 bytes; keeps the header at 48 bytes so material data following the header begins on a 16-byte boundary in std430 layout.
    uint64_t instances_address; ///< GPU pointer to the instance data array.
    uint32_t texture_id;        ///< Bindless texture index (0 = none).
    uint32_t instance_count;    ///< Number of instances in this draw.
    uint64_t vbo_address;       ///< GPU pointer to the draw's vertex buffer (mesh VBO).
    uint64_t uv1_address;       ///< GPU pointer to the draw's TEXCOORD_1 stream (vec2 per vertex), or a context-owned single-vertex fallback when @c uv1_enabled is 0.
    uint32_t uv1_enabled;       ///< 1 = per-vertex UV1 stream at @c uv1_address; 0 = fallback buffer, vertex shader reads index 0 only. Used as a branchless index multiplier in the vertex shader.
    uint32_t _pad0;
};

static_assert(sizeof(DrawDataHeader) == 48, "DrawDataHeader must be 48 bytes for std430 alignment");

// ===== Scene-data GPU structs =====
// Mirrors of GLSL types consumed by RT and deferred compute shaders.
// Plain POD, no scene deps — packed for std430 / buffer_reference reads.

/// GPU-side shape record. Mirrors the RtShape struct in the RT compute
/// prelude and the deferred lighting compute. Geometry + material +
/// texture + shape discriminator in 128 bytes.
///
/// shape_kind:
///   0 = rect, 1 = cube, 2 = sphere — analytic primitives.
///   255 = mesh — triangle soup; `origin/u_axis` carry the world-space
///   AABB and `mesh_data_addr` is a GPU pointer at a MeshData record.
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
    uint64_t material_data_addr;
    uint64_t mesh_data_addr;  ///< for shape_kind == 255: MeshData*; otherwise 0
};
static_assert(sizeof(RtShape) == 128, "RtShape layout mismatch");

/// Sentinel value of RtShape::shape_kind for triangle-mesh shapes.
inline constexpr uint32_t kRtShapeKindMesh = 255;

/// Mesh-static metadata: same for every element instance referencing a
/// given IMeshPrimitive. Owned by the primitive (returned via
/// IDrawData::get_data_buffer), so the GPU address is stable across
/// frames and shapes can cache it. Mirrors GLSL `MeshStaticData`.
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

/// Per-shape, per-frame mesh instance data. Holds the element's world
/// matrices plus a pointer to the mesh-static buffer. Mirrors GLSL
/// `MeshInstanceData`.
VELK_GPU_STRUCT MeshInstanceData
{
    float    world[16];       ///< column-major mesh-local -> world.
    float    inv_world[16];   ///< column-major world -> mesh-local.
    uint64_t mesh_static_addr;///< MeshStaticData* (persistent; stable across frames).
    uint64_t _pad;
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
    float    params[4];           ///< x = range, y = cos(inner), z = cos(outer)
};
static_assert(sizeof(GpuLight) == 80, "GpuLight layout mismatch");

} // namespace velk

#endif // VELK_RENDER_GPU_DATA_H
