#ifndef VELK_RENDER_DEFERRED_GBUFFER_H
#define VELK_RENDER_DEFERRED_GBUFFER_H

#include <cstdint>

#include <velk-render/interface/intf_render_backend.h>

namespace velk {

/**
 * @brief Deferred path's G-buffer attachment layout.
 *
 * Internal to the deferred path. The lighting compute shader, the
 * raster gbuffer-fill shader template, and DeferredPath all share this
 * layout positionally; changing it requires coordinated edits across
 * those three sites. Not a public extension point.
 */
enum class GBufferAttachment : uint32_t
{
    Albedo         = 0, ///< RGBA8      rgb = surface albedo, a = alpha.
    Normal         = 1, ///< RGBA16F    xyz = world-space normal, w = unused.
    WorldPos       = 2, ///< RGBA32F    xyz = world position, w = unused. Full float because
                        ///  RGBA16F's ~3 decimal digits of precision quantizes a typical
                        ///  100 m scene to ~0.1 m steps, which collapses entire raster
                        ///  triangles into a single shader-visible position and breaks
                        ///  any per-pixel computation that needs world-space coords
                        ///  (notably RT shadows: triangle-granularity hit/miss).
    MaterialParams = 3, ///< RGBA8      r = metallic, g = roughness, b = lighting mode, a = unused.
    Emissive       = 4, ///< RGBA16F    rgb = HDR emissive radiance, a = unused. Half float so
                        ///  KHR_materials_emissive_strength values above 1 survive for bloom;
                        ///  the lighting pass adds this to the lit output before tonemap.
    Count          = 5
};

/** @brief G-buffer attachment formats in declaration order. */
inline constexpr PixelFormat kGBufferFormats[static_cast<uint32_t>(GBufferAttachment::Count)] = {
    PixelFormat::RGBA8,    // Albedo
    PixelFormat::RGBA16F,  // Normal
    PixelFormat::RGBA32F,  // WorldPos (precision-critical; see comment above)
    PixelFormat::RGBA8,    // MaterialParams
    PixelFormat::RGBA16F,  // Emissive (HDR; see comment above)
};

/**
 * @brief Tells the compute lighting pass how to interpret a G-buffer pixel.
 *
 * Encoded in MaterialParams.b (scaled by 255 on write, compared by range
 * on read). Add new modes by appending before Count.
 */
enum class LightingMode : uint8_t
{
    Unlit    = 0, ///< Pass albedo through. Used by UI visuals that draw their own color (text, gradient, rect).
    Standard = 1, ///< PBR direct lighting + shadows + env. Used by StandardMaterial.
    Count
};

} // namespace velk

#endif // VELK_RENDER_DEFERRED_GBUFFER_H
