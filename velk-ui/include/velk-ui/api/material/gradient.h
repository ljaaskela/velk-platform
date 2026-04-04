#ifndef VELK_UI_API_MATERIAL_GRADIENT_H
#define VELK_UI_API_MATERIAL_GRADIENT_H

#include <velk/api/object.h>
#include <velk/api/state.h>

#include <velk-render/interface/intf_material.h>
#include <velk-ui/interface/intf_gradient.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around a GradientMaterial.
 *
 * Provides null-safe access to gradient properties (start_color, end_color, angle).
 *
 *   auto grad = material::create_gradient();
 *   grad.set_start_color(color::red());
 *   grad.set_end_color(color::blue());
 *   grad.set_angle(90.f);
 *   rect.set_paint(grad);
 */
class GradientMaterial : public Object
{
public:
    GradientMaterial() = default;
    explicit GradientMaterial(IObject::Ptr obj) : Object(check_object<IGradient>(obj)) {}
    operator IMaterial::Ptr() const { return as_ptr<IMaterial>(); }

    void set_gradient(color start_color, color end_color, float angle)
    {
        if (auto wh = write_state<IGradient>()) {
            wh->start_color = start_color;
            wh->end_color = end_color;
            wh->angle = angle;
        }
    }

    auto get_start_color() const { return read_state_value<IGradient>(&IGradient::State::start_color); }
    void set_start_color(const color& v) { write_state_value<IGradient>(&IGradient::State::start_color, v); }

    auto get_end_color() const { return read_state_value<IGradient>(&IGradient::State::end_color); }
    void set_end_color(const color& v) { write_state_value<IGradient>(&IGradient::State::end_color, v); }

    auto get_angle() const { return read_state_value<IGradient>(&IGradient::State::angle); }
    void set_angle(float v) { write_state_value<IGradient>(&IGradient::State::angle, v); }
};

namespace material {

/** @brief Creates a new GradientMaterial. */
inline GradientMaterial create_gradient(color start_color = color::white(),
                                        color end_color = color::black(), float angle = 0.f)
{
    auto grad = GradientMaterial(instance().create<IObject>(ClassId::Material::Gradient));
    grad.set_gradient(start_color, end_color, angle);
    return grad;
}

} // namespace material

} // namespace velk::ui

#endif // VELK_UI_API_MATERIAL_GRADIENT_H
