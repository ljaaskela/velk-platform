#ifndef VELK_UI_SHADER_MATERIAL_H
#define VELK_UI_SHADER_MATERIAL_H

#include <velk/ext/object.h>

#include <velk-ui/interface/intf_material.h>
#include <velk-ui/plugin.h>

namespace velk_ui {

/**
 * @brief Implementation of IMaterial with a custom fragment shader.
 */
class ShaderMaterial : public velk::ext::Object<ShaderMaterial, IMaterial>
{
public:
    VELK_CLASS_UID(ClassId::Material::Shader, "ShaderMaterial");
};

} // namespace velk_ui

#endif // VELK_UI_SHADER_MATERIAL_H
