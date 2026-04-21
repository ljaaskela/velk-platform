#ifndef VELK_RENDER_INTF_MATERIAL_PROPERTY_H
#define VELK_RENDER_INTF_MATERIAL_PROPERTY_H

#include <velk/api/math_types.h>
#include <velk/api/object_ref.h>
#include <velk/interface/intf_metadata.h>

namespace velk {

/**
 * @brief Input to a material's shading model.
 *
 * Properties attach to a material (e.g. StandardMaterial) to configure one of
 * its inputs: base color, normal map, clearcoat weight, etc. Every property
 * carries a texture slot, tex coord index, and UV transform (KHR_texture_transform
 * shape); concrete subclasses add class specific factors (baseColor factor,
 * metallic/roughness factors, normal scale, ...).
 *
 * "Last attached of class X wins": multiple properties of the same class may
 * be attached simultaneously; the most recently attached one is effective,
 * earlier ones become dormant overrides that re-activate on detach.
 *
 * See design-notes/material_properties.md.
 */
class IMaterialProperty : public Interface<IMaterialProperty>
{
public:
    VELK_INTERFACE(
        (PROP, ObjectRef, texture, {}),
        (PROP, int, tex_coord, 0),
        (PROP, vec2, uv_offset, {}),
        (PROP, float, uv_rotation, 0.f),
        (PROP, vec2, uv_scale, (vec2{1.f, 1.f}))
    )
};

/// glTF 2.0 pbrMetallicRoughness.baseColorFactor / baseColorTexture.
class IBaseColorProperty : public Interface<IBaseColorProperty>
{
public:
    VELK_INTERFACE(
        (PROP, color, factor, (color{1.f, 1.f, 1.f, 1.f}))
    )
};

/// glTF 2.0 pbrMetallicRoughness.{metallic,roughness}Factor and metallicRoughnessTexture
/// (metallic sampled from B channel, roughness from G channel per spec).
class IMetallicRoughnessProperty : public Interface<IMetallicRoughnessProperty>
{
public:
    VELK_INTERFACE(
        (PROP, float, metallic_factor, 1.f),
        (PROP, float, roughness_factor, 1.f)
    )
};

/// glTF 2.0 normalTexture + normalScale. Tangent space normal map.
class INormalProperty : public Interface<INormalProperty>
{
public:
    VELK_INTERFACE(
        (PROP, float, scale, 1.f)
    )
};

/// glTF 2.0 occlusionTexture + occlusionStrength. Ambient occlusion sampled from R channel.
class IOcclusionProperty : public Interface<IOcclusionProperty>
{
public:
    VELK_INTERFACE(
        (PROP, float, strength, 1.f)
    )
};

/// glTF 2.0 emissiveFactor + emissiveTexture. `strength` covers KHR_materials_emissive_strength
/// without needing a separate property class.
class IEmissiveProperty : public Interface<IEmissiveProperty>
{
public:
    VELK_INTERFACE(
        (PROP, color, factor, (color{0.f, 0.f, 0.f, 1.f})),
        (PROP, float, strength, 1.f)
    )
};

/// KHR_materials_specular: specularFactor + specularTexture (A channel),
/// specularColorFactor + specularColorTexture (RGB channels).
class ISpecularProperty : public Interface<ISpecularProperty>
{
public:
    VELK_INTERFACE(
        (PROP, float, factor, 1.f),
        (PROP, color, color_factor, (color{1.f, 1.f, 1.f, 1.f}))
    )
};

} // namespace velk

#endif // VELK_RENDER_INTF_MATERIAL_PROPERTY_H
