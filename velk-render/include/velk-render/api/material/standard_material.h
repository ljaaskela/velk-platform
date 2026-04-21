#ifndef VELK_RENDER_API_MATERIAL_STANDARD_MATERIAL_H
#define VELK_RENDER_API_MATERIAL_STANDARD_MATERIAL_H

#include <velk/api/object.h>
#include <velk/api/state.h>

#include <velk-render/api/material/material.h>
#include <velk-render/api/material/material_property.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/interface/material/intf_material_property.h>
#include <velk-render/interface/material/intf_standard_material.h>
#include <velk-render/plugin.h>

namespace velk {

/**
 * @brief Convenience wrapper around a StandardMaterial object.
 *
 * glTF metallic-roughness surface. Inputs are modeled as attached
 * IMaterialProperty objects (BaseColorProperty, MetallicRoughnessProperty,
 * NormalProperty, ...). The `set_*` accessors here update the canonical
 * (first-attached) property of each class, creating it lazily on first use.
 * Callers who want override-style layering should attach additional property
 * objects explicitly; the most recently attached of each class is effective
 * ("last wins").
 */
class StandardMaterial : public Material
{
public:
    StandardMaterial() = default;
    explicit StandardMaterial(IObject::Ptr obj) : Material(check_object<IStandardMaterial>(obj)) {}
    explicit StandardMaterial(IStandardMaterial::Ptr mat) : Material(as_object(mat)) {}
    operator IStandardMaterial::Ptr() const { return as_ptr<IStandardMaterial>(); }

    color get_base_color() const { return get_prop<IBaseColorProperty>(&IBaseColorProperty::State::factor); }
    void set_base_color(const color& v)
    {
        set_prop<IBaseColorProperty>(ClassId::BaseColorProperty, &IBaseColorProperty::State::factor, v);
    }

    float get_metallic() const
    {
        return get_prop<IMetallicRoughnessProperty>(&IMetallicRoughnessProperty::State::metallic_factor);
    }
    void set_metallic(float v)
    {
        set_prop<IMetallicRoughnessProperty>(
            ClassId::MetallicRoughnessProperty, &IMetallicRoughnessProperty::State::metallic_factor, v);
    }

    float get_roughness() const
    {
        return get_prop<IMetallicRoughnessProperty>(&IMetallicRoughnessProperty::State::roughness_factor);
    }
    void set_roughness(float v)
    {
        set_prop<IMetallicRoughnessProperty>(
            ClassId::MetallicRoughnessProperty, &IMetallicRoughnessProperty::State::roughness_factor, v);
    }

    /// Canonical (first-attached) property wrappers. Empty wrapper if none attached.
    BaseColorProperty base_color() const { return canonical<BaseColorProperty, IBaseColorProperty>(); }
    MetallicRoughnessProperty metallic_roughness() const
    {
        return canonical<MetallicRoughnessProperty, IMetallicRoughnessProperty>();
    }
    NormalProperty normal() const { return canonical<NormalProperty, INormalProperty>(); }
    OcclusionProperty occlusion() const { return canonical<OcclusionProperty, IOcclusionProperty>(); }
    EmissiveProperty emissive() const { return canonical<EmissiveProperty, IEmissiveProperty>(); }
    SpecularProperty specular() const { return canonical<SpecularProperty, ISpecularProperty>(); }

private:
    template <class Wrapper, class Interface>
    Wrapper canonical() const
    {
        return Wrapper(as_object(find_attachment<Interface>()));
    }

    /// Reads a field from the canonical (first-attached) @p P property, or its default if absent.
    template <class P, class T>
    T get_prop(T P::State::* member) const
    {
        auto p = find_attachment<P>();
        return p ? ::velk::read_state_value<P>(p, member) : T{};
    }

    /// Writes a field on the canonical @p P property, creating it with @p class_id on first use.
    template <class P, class T>
    void set_prop(Uid class_id, T P::State::* member, const T& value)
    {
        if (auto p = find_or_create_attachment<P>(class_id)) {
            ::velk::write_state_value<P>(p.get(), member, value);
        }
    }
};

namespace material {

/** @brief Creates a new StandardMaterial with the given parameters. */
inline StandardMaterial create_standard(color base = color{1.f, 1.f, 1.f, 1.f}, float metallic = 0.f,
                                        float roughness = 0.5f)
{
    StandardMaterial m(instance().create<IObject>(ClassId::StandardMaterial));
    m.set_base_color(base);
    m.set_metallic(metallic);
    m.set_roughness(roughness);
    return m;
}

} // namespace material

} // namespace velk

#endif // VELK_RENDER_API_MATERIAL_STANDARD_MATERIAL_H
