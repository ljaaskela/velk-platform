#ifndef VELK_RENDER_MATERIAL_PROPERTY_H
#define VELK_RENDER_MATERIAL_PROPERTY_H

#include <velk/ext/object.h>

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

class AlphaModeProperty
    : public ext::Object<AlphaModeProperty, IAlphaModeProperty, IMaterialProperty>
{
public:
    VELK_CLASS_UID(ClassId::AlphaModeProperty, "AlphaModeProperty");
};

class DoubleSidedProperty
    : public ext::Object<DoubleSidedProperty, IDoubleSidedProperty, IMaterialProperty>
{
public:
    VELK_CLASS_UID(ClassId::DoubleSidedProperty, "DoubleSidedProperty");
};

} // namespace velk::impl

#endif // VELK_RENDER_MATERIAL_PROPERTY_H
