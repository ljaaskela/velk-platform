#ifndef VELK_RENDER_API_MATERIAL_MATERIAL_PROPERTY_H
#define VELK_RENDER_API_MATERIAL_MATERIAL_PROPERTY_H

#include <velk/api/math_types.h>
#include <velk/api/object.h>
#include <velk/api/object_ref.h>
#include <velk/api/state.h>

#include <velk-render/interface/intf_surface.h>
#include <velk-render/interface/material/intf_material_property.h>
#include <velk-render/plugin.h>

namespace velk {

/**
 * @brief Convenience wrapper around an IMaterialProperty object.
 *
 * Exposes the common fields every material property carries: texture reference,
 * tex coord index, and UV transform (KHR_texture_transform shape). Subclass
 * wrappers (BaseColorProperty, MetallicRoughnessProperty, ...) add class
 * specific accessors on top.
 *
 * Null-safe: default-constructed / empty wrappers return defaults on reads and
 * are no-ops on writes.
 */
class MaterialProperty : public Object
{
public:
    MaterialProperty() = default;
    /** @brief Wraps @p obj if it implements IMaterialProperty, otherwise constructs empty. */
    explicit MaterialProperty(IObject::Ptr obj) : Object(check_object<IMaterialProperty>(obj)) {}
    /** @brief Wraps an IMaterialProperty pointer directly. */
    explicit MaterialProperty(IMaterialProperty::Ptr mat) : Object(as_object(mat)) {}
    /** @brief Implicit conversion to the underlying IMaterialProperty pointer. */
    operator IMaterialProperty::Ptr() const { return as_ptr<IMaterialProperty>(); }

    /** @brief Returns the bound texture (ISurface), or null if none bound. */
    ISurface::Ptr get_texture() const
    {
        return with<IMaterialProperty>([](auto& m) {
            auto r = ::velk::read_state<IMaterialProperty>(&m);
            return r ? r->texture.template get<ISurface>() : nullptr;
        });
    }

    /** @brief Binds @p tex as the texture. Applies an ISurface constraint on the ObjectRef. */
    void set_texture(const ISurface::Ptr& tex)
    {
        ::velk::write_state<IMaterialProperty>(as<IMaterialProperty>(), [&](auto& s) {
            ::velk::set_object_ref(s.texture, tex);
            s.texture.template set_constraint<ISurface>();
        });
    }

    /** @brief glTF TEXCOORD_N index to sample the texture from. */
    int get_tex_coord() const
    {
        return read_state_value<IMaterialProperty>(&IMaterialProperty::State::tex_coord);
    }
    /** @copybrief get_tex_coord */
    void set_tex_coord(int v)
    {
        write_state_value<IMaterialProperty>(&IMaterialProperty::State::tex_coord, v);
    }

    /** @brief KHR_texture_transform offset applied to the sampled UV. */
    vec2 get_uv_offset() const
    {
        return read_state_value<IMaterialProperty>(&IMaterialProperty::State::uv_offset);
    }
    /** @copybrief get_uv_offset */
    void set_uv_offset(const vec2& v)
    {
        write_state_value<IMaterialProperty>(&IMaterialProperty::State::uv_offset, v);
    }

    /** @brief KHR_texture_transform rotation (radians) applied to the sampled UV. */
    float get_uv_rotation() const
    {
        return read_state_value<IMaterialProperty>(&IMaterialProperty::State::uv_rotation);
    }
    /** @copybrief get_uv_rotation */
    void set_uv_rotation(float v)
    {
        write_state_value<IMaterialProperty>(&IMaterialProperty::State::uv_rotation, v);
    }

    /** @brief KHR_texture_transform scale applied to the sampled UV. */
    vec2 get_uv_scale() const
    {
        return read_state_value<IMaterialProperty>(&IMaterialProperty::State::uv_scale);
    }
    /** @copybrief get_uv_scale */
    void set_uv_scale(const vec2& v)
    {
        write_state_value<IMaterialProperty>(&IMaterialProperty::State::uv_scale, v);
    }
};

/**
 * @brief Wrapper for a BaseColorProperty attachment.
 *
 * glTF 2.0 pbrMetallicRoughness.baseColorFactor and baseColorTexture. The factor
 * multiplies the sampled texture (which defaults to white when none is bound).
 */
class BaseColorProperty : public MaterialProperty
{
public:
    BaseColorProperty() = default;
    explicit BaseColorProperty(IObject::Ptr obj) : MaterialProperty(check_object<IBaseColorProperty>(obj)) {}

    /** @brief Base color factor. For dielectrics it tints the surface; for metals it sets specular F0. */
    color get_factor() const
    {
        return read_state_value<IBaseColorProperty>(&IBaseColorProperty::State::factor);
    }
    /** @copybrief get_factor */
    void set_factor(const color& v)
    {
        write_state_value<IBaseColorProperty>(&IBaseColorProperty::State::factor, v);
    }
};

/**
 * @brief Wrapper for a MetallicRoughnessProperty attachment.
 *
 * glTF 2.0 pbrMetallicRoughness.{metallic,roughness}Factor and metallicRoughnessTexture.
 * Metallic is sampled from the B channel, roughness from the G channel per the spec.
 */
class MetallicRoughnessProperty : public MaterialProperty
{
public:
    MetallicRoughnessProperty() = default;
    explicit MetallicRoughnessProperty(IObject::Ptr obj)
        : MaterialProperty(check_object<IMetallicRoughnessProperty>(obj))
    {}

    /** @brief 0 = dielectric, 1 = metal. Multiplies the texture's B channel. */
    float get_metallic_factor() const
    {
        return read_state_value<IMetallicRoughnessProperty>(
            &IMetallicRoughnessProperty::State::metallic_factor);
    }
    /** @copybrief get_metallic_factor */
    void set_metallic_factor(float v)
    {
        write_state_value<IMetallicRoughnessProperty>(&IMetallicRoughnessProperty::State::metallic_factor, v);
    }

    /** @brief 0 = mirror, 1 = fully diffuse. Multiplies the texture's G channel. */
    float get_roughness_factor() const
    {
        return read_state_value<IMetallicRoughnessProperty>(
            &IMetallicRoughnessProperty::State::roughness_factor);
    }
    /** @copybrief get_roughness_factor */
    void set_roughness_factor(float v)
    {
        write_state_value<IMetallicRoughnessProperty>(&IMetallicRoughnessProperty::State::roughness_factor,
                                                      v);
    }
};

/**
 * @brief Wrapper for a NormalProperty attachment.
 *
 * glTF 2.0 normalTexture + normalScale. Tangent-space normal map; @c scale applies
 * to the XY components of the sampled normal before re-normalization.
 */
class NormalProperty : public MaterialProperty
{
public:
    NormalProperty() = default;
    explicit NormalProperty(IObject::Ptr obj) : MaterialProperty(check_object<INormalProperty>(obj)) {}

    /** @brief Scalar applied to the sampled normal's XY before re-normalization. */
    float get_scale() const { return read_state_value<INormalProperty>(&INormalProperty::State::scale); }
    /** @copybrief get_scale */
    void set_scale(float v) { write_state_value<INormalProperty>(&INormalProperty::State::scale, v); }
};

/**
 * @brief Wrapper for an OcclusionProperty attachment.
 *
 * glTF 2.0 occlusionTexture + occlusionStrength. Ambient occlusion is sampled from
 * the texture's R channel and modulated by @c strength.
 */
class OcclusionProperty : public MaterialProperty
{
public:
    OcclusionProperty() = default;
    explicit OcclusionProperty(IObject::Ptr obj) : MaterialProperty(check_object<IOcclusionProperty>(obj)) {}

    /** @brief 0 = no occlusion, 1 = full occlusion from texture. */
    float get_strength() const
    {
        return read_state_value<IOcclusionProperty>(&IOcclusionProperty::State::strength);
    }
    /** @copybrief get_strength */
    void set_strength(float v)
    {
        write_state_value<IOcclusionProperty>(&IOcclusionProperty::State::strength, v);
    }
};

/**
 * @brief Wrapper for an EmissiveProperty attachment.
 *
 * glTF 2.0 emissiveFactor + emissiveTexture. @c strength covers
 * KHR_materials_emissive_strength (HDR emissive above 1.0).
 */
class EmissiveProperty : public MaterialProperty
{
public:
    EmissiveProperty() = default;
    explicit EmissiveProperty(IObject::Ptr obj) : MaterialProperty(check_object<IEmissiveProperty>(obj)) {}

    /** @brief Emissive color factor. Multiplies the sampled emissive texture. */
    color get_factor() const
    {
        return read_state_value<IEmissiveProperty>(&IEmissiveProperty::State::factor);
    }
    /** @copybrief get_factor */
    void set_factor(const color& v)
    {
        write_state_value<IEmissiveProperty>(&IEmissiveProperty::State::factor, v);
    }

    /** @brief HDR intensity multiplier. Applied on top of the factor (KHR_materials_emissive_strength). */
    float get_strength() const
    {
        return read_state_value<IEmissiveProperty>(&IEmissiveProperty::State::strength);
    }
    /** @copybrief get_strength */
    void set_strength(float v)
    {
        write_state_value<IEmissiveProperty>(&IEmissiveProperty::State::strength, v);
    }
};

/**
 * @brief Wrapper for a SpecularProperty attachment.
 *
 * KHR_materials_specular. @c factor modulates specular weight (sampled from the
 * texture's A channel); @c color_factor tints dielectric F0 (sampled from RGB).
 */
class SpecularProperty : public MaterialProperty
{
public:
    SpecularProperty() = default;
    explicit SpecularProperty(IObject::Ptr obj) : MaterialProperty(check_object<ISpecularProperty>(obj)) {}

    /** @brief Specular weight. Multiplies the texture's A channel. */
    float get_factor() const
    {
        return read_state_value<ISpecularProperty>(&ISpecularProperty::State::factor);
    }
    /** @copybrief get_factor */
    void set_factor(float v) { write_state_value<ISpecularProperty>(&ISpecularProperty::State::factor, v); }

    /** @brief Specular color tint for dielectrics. Multiplies the texture's RGB channels. */
    color get_color_factor() const
    {
        return read_state_value<ISpecularProperty>(&ISpecularProperty::State::color_factor);
    }
    /** @copybrief get_color_factor */
    void set_color_factor(const color& v)
    {
        write_state_value<ISpecularProperty>(&ISpecularProperty::State::color_factor, v);
    }
};

} // namespace velk

#endif // VELK_RENDER_API_MATERIAL_MATERIAL_PROPERTY_H
