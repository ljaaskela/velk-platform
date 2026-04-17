#ifndef VELK_RENDER_INTF_STANDARD_H
#define VELK_RENDER_INTF_STANDARD_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>

namespace velk {

/**
 * @brief Interface for the "standard" physically-based surface material.
 *
 * Metallic-roughness workflow, matching the glTF Core spec. Named
 * `StandardMaterial` rather than `PbrMaterial` to align with
 * Three.js/Godot/Unity/Unreal conventions: "standard" is the default
 * PBR surface; extensions (transmission, clearcoat, sheen, anisotropy,
 * etc.) layer on as separate materials or properties later.
 *
 *   base_color: surface albedo for dielectrics; specular F0 for metals.
 *   metallic:   0 = dielectric, 1 = metal. Interpolated in-between.
 *   roughness:  0 = mirror, 1 = fully diffuse.
 */
class IStandard : public Interface<IStandard>
{
public:
    VELK_INTERFACE(
        (PROP, color, base_color, (color{1.f, 1.f, 1.f, 1.f})),
        (PROP, float, metallic, 0.f),
        (PROP, float, roughness, 0.5f)
    )
};

} // namespace velk

#endif // VELK_RENDER_INTF_STANDARD_H
