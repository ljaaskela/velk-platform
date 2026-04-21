#ifndef VELK_RENDER_API_MATERIAL_MATERIAL_H
#define VELK_RENDER_API_MATERIAL_MATERIAL_H

#include <velk/api/object.h>

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

    /// Returns the attached IMaterialOptions, or an empty wrapper if none.
    /// Empty reads return glTF-spec defaults; writes on empty are no-ops.
    MaterialOptions options() const
    {
        return MaterialOptions(as_object(find_attachment<IMaterialOptions>()));
    }

    /// Returns the attached IMaterialOptions, creating one on first call.
    /// Use when you need to modify pipeline state.
    MaterialOptions ensure_options()
    {
        return MaterialOptions(
            as_object(find_or_create_attachment<IMaterialOptions>(ClassId::MaterialOptions)));
    }
};

} // namespace velk

#endif // VELK_RENDER_API_MATERIAL_MATERIAL_H
