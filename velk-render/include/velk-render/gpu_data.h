#ifndef VELK_RENDER_GPU_DATA_H
#define VELK_RENDER_GPU_DATA_H

#include <cstdint>

namespace velk {

/// @file gpu_data.h
/// Framework-level GPU data structures.
///
/// Visual-specific instance types (RectInstance, TextInstance, etc.) are defined
/// by the visuals that produce them, not here.

/// Declares a struct with std430-compatible alignment (16 bytes).
/// Use for material GPU data structs that follow the DrawDataHeader.
/// The compiler pads the struct automatically, no manual _pad fields needed.
#define VELK_GPU_STRUCT struct alignas(16)

/// Per-frame global data written by the renderer, read by all shaders.
///
/// Layout must match the `GlobalData` buffer_reference block in velk.glsl
/// (std430). Scene-wide state (BVH nodes + shapes, BVH metadata) lives
/// here so every view sees the same acceleration structure after
/// Renderer::build_frame_passes builds it once per scene per frame.
struct FrameGlobals
{
    float    view_projection[16];          ///< Combined view-projection matrix from the camera.
    float    inverse_view_projection[16];  ///< Inverse of view_projection.
    float    viewport[4];                  ///< width, height, 1/width, 1/height.
    float    cam_pos[4];                   ///< World-space camera position (xyz) + pad.
    uint32_t bvh_root;                     ///< Index of the root BvhNode; 0 if no BVH.
    uint32_t bvh_node_count;               ///< Total BvhNodes; 0 if no BVH.
    uint32_t bvh_shape_count;              ///< Total RtShapes the BVH indexes.
    uint32_t _pad0;
    uint64_t bvh_nodes_addr;               ///< GPU pointer to the BvhNode array.
    uint64_t bvh_shapes_addr;              ///< GPU pointer to the scene's RtShape array.
};

static_assert(sizeof(FrameGlobals) == 192, "FrameGlobals layout must match velk.glsl");

/**
 * @brief Standard draw data header at the start of every draw's GPU data.
 *
 * The shader receives a pointer to this via push constants. The header
 * contains GPU pointers to the globals and instance data, plus a bindless
 * texture index. Material-specific data follows immediately after.
 *
 * Padded to 32 bytes via VELK_GPU_STRUCT so material data (which may
 * start with a vec4) meets std430 16-byte alignment.
 */
VELK_GPU_STRUCT DrawDataHeader
{
    uint64_t globals_address;   ///< GPU pointer to FrameGlobals.
    uint64_t instances_address; ///< GPU pointer to the instance data array.
    uint32_t texture_id;        ///< Bindless texture index (0 = none).
    uint32_t instance_count;    ///< Number of instances in this draw.
};

static_assert(sizeof(DrawDataHeader) == 32, "DrawDataHeader must be 32 bytes for std430 alignment");

} // namespace velk

#endif // VELK_RENDER_GPU_DATA_H
