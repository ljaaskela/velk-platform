#ifndef VELK_RENDER_INTF_LIGHT_H
#define VELK_RENDER_INTF_LIGHT_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/intf_render_trait.h>

namespace velk {

/** @brief Light source kind. */
enum class LightType : uint8_t
{
    Directional = 0, ///< Infinite source (sun). Direction from host's world-matrix forward axis; position ignored.
    Point       = 1, ///< Omnidirectional local source. Position from host's world-matrix translation.
    Spot        = 2, ///< Cone from a point along a direction. Uses both position and direction.
};

/**
 * @brief Light render-trait. Describes a light source in the scene.
 *
 * Paralleling `ICamera`: `ILight` is scene description ("there is a
 * light here, of this kind, this colour"). The renderer reads the
 * host's world matrix to get position and forward axis; the
 * interface itself carries only intrinsic source properties.
 *
 * Shadow casting is not part of this interface. The choice of shadow
 * technique (RT, shadow map, none) attaches to the light via
 * `RenderTrait::add_technique` as a separate `IShadowTechnique`. A
 * light with no shadow technique attached casts no shadows.
 */
class ILight : public Interface<ILight, IRenderTrait>
{
public:
    VELK_INTERFACE(
        (PROP, LightType, type, LightType::Directional),
        (PROP, ::velk::color, color, (::velk::color::white())),
        (PROP, float, intensity, 1.f),
        /// Effective range for point / spot lights. Unused for directional.
        (PROP, float, range, 1000.f),
        /// Inner cone half-angle in degrees (full intensity inside). Spot only.
        (PROP, float, cone_inner_deg, 20.f),
        /// Outer cone half-angle in degrees (falloff to zero). Spot only.
        (PROP, float, cone_outer_deg, 30.f)
    )
};

} // namespace velk

#endif // VELK_RENDER_INTF_LIGHT_H
