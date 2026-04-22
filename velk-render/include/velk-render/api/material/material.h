#ifndef VELK_RENDER_API_MATERIAL_MATERIAL_H
#define VELK_RENDER_API_MATERIAL_MATERIAL_H

#include <velk/api/object.h>
#include <velk/api/state.h>

#include <velk-render/api/material/material_options.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/interface/material/intf_material_options.h>
#include <velk-render/plugin.h>

namespace velk {

/**
 * @brief Base convenience wrapper for any IMaterial.
 *
 * Provides accessors shared by every material: implicit conversion to
 * IMaterial::Ptr for painting, and `options` / `ensure_options` for the
 * pipeline-state attachment (alpha mode, cutoff, culling).
 */
class Material : public Object
{
public:
    Material() = default;
    explicit Material(IObject::Ptr obj) : Object(check_object<IMaterial>(obj)) {}
    explicit Material(IMaterial::Ptr mat) : Object(as_object(mat)) {}
    operator IMaterial::Ptr() const { return as_ptr<IMaterial>(); }

    /// Returns true if an IMaterialOptions attachment is already present.
    /// Non-creating; safe to call without side effects.
    bool has_options() const { return find_attachment<IMaterialOptions>() != nullptr; }

    /// Returns the material's IMaterialOptions attachment. Creates one
    /// on first access, so both reads and writes always work. Use
    /// has_options() to check for existence without creating.
    MaterialOptions options()
    {
        return MaterialOptions(
            as_object(find_or_create_attachment<IMaterialOptions>(ClassId::MaterialOptions)));
    }

    /// Write material options as one transaction
    ::velk::ReturnValue set_options(MaterialOptions::SetOptionsFn* fn) { return options().set_options(fn); }
};

} // namespace velk

#endif // VELK_RENDER_API_MATERIAL_MATERIAL_H
