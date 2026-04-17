#ifndef VELK_RENDER_API_MATERIAL_STANDARD_H
#define VELK_RENDER_API_MATERIAL_STANDARD_H

#include <velk/api/object.h>
#include <velk/api/state.h>

#include <velk-render/interface/intf_material.h>
#include <velk-render/interface/intf_standard.h>
#include <velk-render/plugin.h>

namespace velk {

/**
 * @brief Convenience wrapper around a StandardMaterial object.
 *
 * Null-safe accessors for glTF metallic-roughness parameters. The RT path
 * uses this material's fill snippet for stochastic environment reflections
 * and scene-to-scene mirror bounces. The raster path currently renders a
 * flat base-color fill.
 */
class StandardMaterial : public Object
{
public:
    StandardMaterial() = default;
    explicit StandardMaterial(IObject::Ptr obj) : Object(check_object<IStandard>(obj)) {}
    operator IMaterial::Ptr() const { return as_ptr<IMaterial>(); }

    auto get_base_color() const { return read_state_value<IStandard>(&IStandard::State::base_color); }
    void set_base_color(const color& v) { write_state_value<IStandard>(&IStandard::State::base_color, v); }

    auto get_metallic() const { return read_state_value<IStandard>(&IStandard::State::metallic); }
    void set_metallic(float v) { write_state_value<IStandard>(&IStandard::State::metallic, v); }

    auto get_roughness() const { return read_state_value<IStandard>(&IStandard::State::roughness); }
    void set_roughness(float v) { write_state_value<IStandard>(&IStandard::State::roughness, v); }
};

namespace material {

/** @brief Creates a new StandardMaterial with the given parameters. */
inline StandardMaterial create_standard(color base = color{1.f, 1.f, 1.f, 1.f},
                                        float metallic = 0.f, float roughness = 0.5f)
{
    StandardMaterial m(instance().create<IObject>(ClassId::StandardMaterial));
    m.set_base_color(base);
    m.set_metallic(metallic);
    m.set_roughness(roughness);
    return m;
}

} // namespace material

} // namespace velk

#endif // VELK_RENDER_API_MATERIAL_STANDARD_H
