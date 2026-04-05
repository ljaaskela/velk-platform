#ifndef VELK_UI_INSTANCE_TYPES_H
#define VELK_UI_INSTANCE_TYPES_H

#include <velk/api/math_types.h>

namespace velk {

/// @file instance_types.h
/// C++ instance data structs that mirror GLSL shader layouts.
///
/// Each struct matches the std430 layout of its GLSL counterpart.
/// The first field must always be vec2 pos, as the renderer offsets it
/// by the element's world position during batching.

/// Instance data for rect and rounded-rect pipelines.
/// Matches GLSL: struct RectInstance { vec2 pos; vec2 size; vec4 color; };
struct RectInstance
{
    vec2 pos;
    vec2 size;
    color col;
};
static_assert(sizeof(RectInstance) == 32, "RectInstance must be 32 bytes (matches GLSL std430)");

/// Instance data for the text pipeline.
/// Matches GLSL: struct TextInstance { vec2 pos; vec2 size; vec4 color; vec2 uv_min; vec2 uv_max; };
struct TextInstance
{
    vec2 pos;
    vec2 size;
    color col;
    vec2 uv_min;
    vec2 uv_max;
};
static_assert(sizeof(TextInstance) == 48, "TextInstance must be 48 bytes (matches GLSL std430)");

} // namespace velk

#endif // VELK_UI_INSTANCE_TYPES_H
