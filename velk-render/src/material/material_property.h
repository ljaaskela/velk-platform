#ifndef VELK_RENDER_MATERIAL_PROPERTY_H
#define VELK_RENDER_MATERIAL_PROPERTY_H

#include <velk/ext/object.h>
#include <velk/interface/intf_metadata_observer.h>

#include <velk-render/interface/material/intf_material_options.h>
#include <velk-render/interface/material/intf_material_property.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Property classes attached to a material to configure its inputs.
 *
 * Each implements two interfaces: its class-specific interface (e.g.
 * IBaseColorProperty) carrying class-specific factors, and IMaterialProperty
 * carrying the common texture + UV transform + tex_coord state. Instances
 * are pure data holders; all behavior lives on the material that owns them.
 *
 * See design-notes/material_properties.md.
 */

/**
 * @brief Common base: self-observes and fires `on_property_changed` after any
 *        write, so the owning material knows its record is stale.
 *
 * Same shape as MaterialOptions below. Every write reaches here, whether to
 * IMaterialProperty's shared state or to the class-specific interface, so the
 * owning material cannot miss a mutation.
 */
template <class T, class... Interfaces>
class PropertyBase
    : public ext::Object<T, Interfaces..., IMaterialProperty, IMetadataObserver>
{
public:
    void on_state_changed(string_view, IMetadata&, Uid) override
    {
        ::velk::invoke_event(static_cast<IMaterialProperty*>(this), "on_property_changed");
    }
};

class BaseColorProperty : public PropertyBase<BaseColorProperty, IBaseColorProperty>
{
public:
    VELK_CLASS_UID(ClassId::BaseColorProperty, "BaseColorProperty");
};

class MetallicRoughnessProperty
    : public PropertyBase<MetallicRoughnessProperty, IMetallicRoughnessProperty>
{
public:
    VELK_CLASS_UID(ClassId::MetallicRoughnessProperty, "MetallicRoughnessProperty");
};

class NormalProperty : public PropertyBase<NormalProperty, INormalProperty>
{
public:
    VELK_CLASS_UID(ClassId::NormalProperty, "NormalProperty");
};

class OcclusionProperty : public PropertyBase<OcclusionProperty, IOcclusionProperty>
{
public:
    VELK_CLASS_UID(ClassId::OcclusionProperty, "OcclusionProperty");
};

class EmissiveProperty : public PropertyBase<EmissiveProperty, IEmissiveProperty>
{
public:
    VELK_CLASS_UID(ClassId::EmissiveProperty, "EmissiveProperty");
};

class SpecularProperty : public PropertyBase<SpecularProperty, ISpecularProperty>
{
public:
    VELK_CLASS_UID(ClassId::SpecularProperty, "SpecularProperty");
};

/**
 * @brief Material pipeline options carrier.
 *
 * Self-observes via IMetadataObserver and fires `on_options_changed`
 * after any PROP write. The name-based invoke_event uses Resolve::Existing,
 * so the event object is not lazily created when no one is listening.
 */
class MaterialOptions
    : public ext::Object<MaterialOptions, IMaterialOptions, IMetadataObserver>
{
public:
    VELK_CLASS_UID(ClassId::MaterialOptions, "MaterialOptions");

    void on_state_changed(string_view /*name*/, IMetadata& /*owner*/, Uid interface_id) override
    {
        if (interface_id != IMaterialOptions::UID) {
            return;
        }
        ::velk::invoke_event(static_cast<IMaterialOptions*>(this), "on_options_changed");
    }
};

} // namespace velk::impl

#endif // VELK_RENDER_MATERIAL_PROPERTY_H
