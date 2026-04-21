#ifndef VELK_UI_INSTANCE_TYPES_H
#define VELK_UI_INSTANCE_TYPES_H

#include "velk-render/gpu_data.h"

#include <velk/api/math_types.h>

namespace velk {

/**
 * @file instance_types.h
 * @brief C++ instance data structs that mirror GLSL shader layouts.
 *
 * `ElementInstance` is the universal per-instance record. Every visual
 * (rect, rounded rect, text glyph, texture, image, env, cube, sphere,
 * and future glTF meshes) packs this layout into its DrawEntry. The
 * shader reads it as a single struct; 2D visuals leave `size.z = 0`
 * and `offset = 0`; text glyph instances carry per-glyph offset + a
 * glyph index in `params.x`; 3D primitives fill all three xyz extents
 * in `size`.
 *
 * `world_matrix` at offset 0 is written per-instance by the batch
 * builder (it copies the element's world transform into this slot);
 * visuals leave it zero-initialised.
 *
 * Matches GLSL: see `ElementInstance` in velk-ui.glsl.
 */

VELK_GPU_STRUCT ElementInstance
{
    mat4 world_matrix;  ///< 64 B — filled by batch_builder per instance.
    vec4 offset;        ///< 16 B — xyz = local offset (glyph pos for text, 0 otherwise).
    vec4 size;          ///< 16 B — xyz = extents (size.z = 0 for 2D visuals).
    color col;          ///< 16 B — visual tint.
    uint32_t params[4]; ///< 16 B — params[0] = shape_param (glyph index, ...); others reserved.
};
static_assert(sizeof(ElementInstance) == 128,
              "ElementInstance must be 128 bytes (matches GLSL std430)");

} // namespace velk

#endif // VELK_UI_INSTANCE_TYPES_H
