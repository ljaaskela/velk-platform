#ifndef VELK_RENDER_INTF_STANDARD_MATERIAL_H
#define VELK_RENDER_INTF_STANDARD_MATERIAL_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_interface.h>
#include <velk/vector.h>

#include <velk-render/interface/material/intf_material_property.h>

namespace velk {

/**
 * @brief Interface for the "standard" physically-based surface material.
 *
 * Metallic-roughness workflow matching the glTF 2.0 Core spec, with
 * KHR_materials_specular. Named `StandardMaterial` rather than `PbrMaterial`
 * to align with Three.js/Godot/Unity/Unreal conventions.
 *
 * Material inputs are modeled as attached IMaterialProperty objects rather
 * than fixed PROPs on this interface. See design-notes/material_properties.md.
 * The effective property for any given class is the most recently attached
 * one of that class ("last wins"); earlier attachments of the same class
 * remain as dormant overrides.
 */
class IStandardMaterial : public Interface<IStandardMaterial>
{
public:
    /// Effective property for the given class id, or null if none attached.
    virtual IMaterialProperty::Ptr get_material_property(Uid class_id) const = 0;

    /// All currently effective properties (one per class, last attached wins).
    virtual vector<IMaterialProperty::Ptr> get_material_properties() const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_STANDARD_MATERIAL_H
