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
 * The first field must always be vec2 pos, as the renderer offsets it
 * by the element's world position during batching.
 */

/**
 * @brief Instance data for rect and rounded-rect pipelines.
 *
 * Matches GLSL: struct RectInstance { vec2 pos; vec2 size; vec4 color; };
 */
VELK_GPU_STRUCT RectInstance
{
    vec2 pos;
    vec2 size;
    color col;
};
static_assert(sizeof(RectInstance) == 32, "RectInstance must be 32 bytes (matches GLSL std430)");

/**
 * @brief Instance data for the analytic-Bezier text pipeline.
 *
 * One instance per laid-out glyph quad. The shader uses `glyph_index` to
 * look up the glyph's curve and band data via the per-batch buffer
 * references emitted by the text material; uv is derived from the unit
 * quad with a Y flip in the vertex shader (the curves use FreeType's Y-up
 * convention).
 *
 * Padded to 48 bytes because the GLSL struct contains a vec4 (color),
 * which forces a 16-byte struct alignment in std430. An array of such
 * structs has stride ceil(40/16)*16 = 48, so the C++ struct must match
 * that stride or the GPU and CPU disagree on instance offsets.
 *
 * Matches GLSL: struct TextInstance { vec2 pos; vec2 size; vec4 color;
 * uint glyph_index; uint _pad0; uint _pad1; uint _pad2; };
 */
VELK_GPU_STRUCT TextInstance
{
    vec2 pos;
    vec2 size;
    color col;
    uint32_t glyph_index;
};
static_assert(sizeof(TextInstance) == 48, "TextInstance must be 48 bytes (std430 array stride)");

} // namespace velk

#endif // VELK_UI_INSTANCE_TYPES_H
