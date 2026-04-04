#ifndef VELK_UI_API_MATERIAL_SHADER_H
#define VELK_UI_API_MATERIAL_SHADER_H

#include <velk/api/object.h>

#include <velk-render/interface/intf_material.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around a ShaderMaterial.
 *
 * Provides null-safe access to a custom shader material. The pipeline handle
 * is set at creation time via a render context factory method.
 */
class ShaderMaterial : public Object
{
public:
    ShaderMaterial() = default;
    explicit ShaderMaterial(IObject::Ptr obj) : Object(check_object<IMaterial>(obj)) {}
    operator IMaterial::Ptr() const { return as_ptr<IMaterial>(); }
};

namespace material {

/** @brief Creates a new ShaderMaterial (pipeline handle must be set separately). */
inline ShaderMaterial create_shader()
{
    return ShaderMaterial(instance().create<IObject>(ClassId::Material::Shader));
}

} // namespace material

} // namespace velk::ui

#endif // VELK_UI_API_MATERIAL_SHADER_H
