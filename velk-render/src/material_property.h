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

class BaseColorProperty
    : public ext::Object<BaseColorProperty, IBaseColorProperty, IMaterialProperty>
{
public:
    VELK_CLASS_UID(ClassId::BaseColorProperty, "BaseColorProperty");
};

class MetallicRoughnessProperty
    : public ext::Object<MetallicRoughnessProperty, IMetallicRoughnessProperty, IMaterialProperty>
{
public:
    VELK_CLASS_UID(ClassId::MetallicRoughnessProperty, "MetallicRoughnessProperty");
};

class NormalProperty
    : public ext::Object<NormalProperty, INormalProperty, IMaterialProperty>
{
public:
    VELK_CLASS_UID(ClassId::NormalProperty, "NormalProperty");
};

class OcclusionProperty
    : public ext::Object<OcclusionProperty, IOcclusionProperty, IMaterialProperty>
{
public:
    VELK_CLASS_UID(ClassId::OcclusionProperty, "OcclusionProperty");
};

class EmissiveProperty
    : public ext::Object<EmissiveProperty, IEmissiveProperty, IMaterialProperty>
{
public:
    VELK_CLASS_UID(ClassId::EmissiveProperty, "EmissiveProperty");
};

class SpecularProperty
    : public ext::Object<SpecularProperty, ISpecularProperty, IMaterialProperty>
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
