#ifndef VELK_UI_API_MATERIAL_SHADER_H
#define VELK_UI_API_MATERIAL_SHADER_H

#include <velk/api/object.h>
#include <velk/api/state.h>

#include <velk-ui/interface/intf_material.h>
#include <velk-ui/plugin.h>

namespace velk_ui {

/**
 * @brief Convenience wrapper around IMaterial with a custom fragment shader.
 *
 * Provides null-safe access to shader material properties.
 *
 *   auto mat = material::create_shader();
 *   mat.set_fragment_source("...");
 *   rect.set_paint(mat);
 */
class ShaderMaterial : public velk::Object
{
public:
    /** @brief Default-constructed ShaderMaterial wraps no object. */
    ShaderMaterial() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement IMaterial. */
    explicit ShaderMaterial(velk::IObject::Ptr obj) : Object(check_object<IMaterial>(obj)) {}

    /** @brief Implicit conversion to IMaterial::Ptr. */
    operator IMaterial::Ptr() const { return as_ptr<IMaterial>(); }

    /** @brief Returns the fragment shader source. */
    auto get_fragment_source() const { return read_state_value<IMaterial>(&IMaterial::State::fragment_source); }

    /** @brief Sets the fragment shader source. */
    void set_fragment_source(velk::string_view src)
    {
        write_state_value<IMaterial>(&IMaterial::State::fragment_source, velk::string(src));
    }
};

namespace material {

/** @brief Creates a new ShaderMaterial. */
inline ShaderMaterial create_shader()
{
    return ShaderMaterial(velk::instance().create<velk::IObject>(ClassId::Material::Shader));
}

} // namespace material

} // namespace velk_ui

#endif // VELK_UI_API_MATERIAL_SHADER_H
