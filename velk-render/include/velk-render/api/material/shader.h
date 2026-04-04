#ifndef VELK_RENDER_API_MATERIAL_SHADER_H
#define VELK_RENDER_API_MATERIAL_SHADER_H

#include <velk/api/object.h>
#include <velk/api/property.h>
#include <velk/api/state.h>
#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_shader_material.h>
#include <velk-render/plugin.h>

namespace velk {

/**
 * @brief Convenience wrapper around a ShaderMaterial.
 *
 * Provides ergonomic access to shader input properties:
 *
 *   auto sm = velk::create_shader_material(ctx, frag, vert);
 *   sm.input<float>("scale").set_value(8.f);
 *   sm.input<color>("tint").set_value({1, 0, 0, 1});
 */
class ShaderMaterial : public Object
{
public:
    ShaderMaterial() = default;
    explicit ShaderMaterial(IObject::Ptr obj) : Object(check_object<IShaderMaterial>(obj)) {}
    explicit ShaderMaterial(IMaterial::Ptr m) : Object(as_object(m)) {}

    operator IMaterial::Ptr() const { return as_ptr<IMaterial>(); }

    /// Returns a typed property accessor for a shader input by name.
    template <class T>
    Property<T> input(string_view name)
    {
        auto* meta = interface_cast<IMetadata>(inputs().get());
        return Property<T>(meta ? meta->get_property(name, Resolve::Existing) : nullptr);
    }

    template <class T>
    void set_input(string_view name, T value)
    {
        if (auto prop = input<T>(name)) {
            prop.set_value(std::move(value));
        }
    }

private:
    /// Returns the inputs object directly.
    IObject::Ptr inputs() const
    {
        return with<IShaderMaterial>([](auto& m) -> IObject::Ptr {
            auto state = ::velk::read_state<IShaderMaterial>(&m);
            return (state && state->inputs) ? state->inputs.get() : nullptr;
        });
    }
};

/**
 * @brief Creates a shader material from GLSL source via the render context.
 *
 * The shader is compiled, the pipeline registered, and input properties
 * are created from shader reflection.
 */
inline ShaderMaterial create_shader_material(IRenderContext& ctx, string_view fragment_source,
                                             string_view vertex_source = {})
{
    return ShaderMaterial(ctx.create_shader_material(fragment_source, vertex_source));
}

} // namespace velk

#endif // VELK_RENDER_API_MATERIAL_SHADER_H
