#ifndef VELK_UI_INSTANCE_TYPES_H
#define VELK_UI_INSTANCE_TYPES_H

#include "velk-render/gpu_data.h"

#include <velk/api/math_types.h>

namespace velk {

/**
 * @file instance_types.h
 * @brief C++ instance data structs that mirror GLSL shader layouts.
 *
 * Each struct matches the std430 layout of its GLSL counterpart.
 *
 * The first field of every instance type is world_matrix: the element's
 * full 4x4 transform in world space. The batch builder fills it in for
 * every instance it emits; visuals leave it zero-initialised. Vertex
 * shaders compute the final position as
 *
 *     gl_Position = view_projection * world_matrix * vec4(pos + q*size, 0, 1);
 *
 * where (pos, size) is the instance's local-space rect (identical to the
 * old 2D encoding). This lets arbitrary 3D transforms flow through
 * raster pipelines (rotated cards, flipping dials, ...) while keeping
 * pure-2D UI a special case (world_matrix = translation-only).
 */

/**
 * @brief Instance data for rect and rounded-rect pipelines.
 *
 * Matches GLSL: struct RectInstance { mat4 world_matrix; vec2 pos; vec2 size; vec4 color; };
 */
VELK_GPU_STRUCT RectInstance
{
    mat4 world_matrix;
    vec2 pos;
    vec2 size;
    color col;
};
static_assert(sizeof(RectInstance) == 96, "RectInstance must be 96 bytes (matches GLSL std430)");

/**
 * @brief Instance data for the analytic-Bezier text pipeline.
 *
 * One instance per laid-out glyph quad. The shader uses `glyph_index` to
 * look up the glyph's curve and band data via the per-batch buffer
 * references emitted by the text material; uv is derived from the unit
 * quad with a Y flip in the vertex shader (the curves use FreeType's Y-up
 * convention).
 *
 * Matches GLSL: struct TextInstance { mat4 world_matrix; vec2 pos;
 *   vec2 size; vec4 color; uint glyph_index; uint _pad[3]; };
 */
VELK_GPU_STRUCT TextInstance
{
    mat4 world_matrix;
    vec2 pos;
    vec2 size;
    color col;
    uint32_t glyph_index;
    uint32_t _pad[3];
};
static_assert(sizeof(TextInstance) == 112, "TextInstance must be 112 bytes (std430 array stride)");

} // namespace velk

#endif // VELK_UI_INSTANCE_TYPES_H
