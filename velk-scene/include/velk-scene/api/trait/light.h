#ifndef VELK_UI_API_TRAIT_LIGHT_H
#define VELK_UI_API_TRAIT_LIGHT_H

#include <velk/api/state.h>

#include <velk-render/interface/intf_light.h>
#include <velk-scene/api/render_trait.h>
#include <velk-scene/plugin.h>

namespace velk {

/**
 * @brief Convenience wrapper around ILight.
 *
 * Paralleling Camera: a Light is a RenderTrait attached to an element,
 * describing "there is a light here." The owning element's world
 * transform gives the position (for point / spot) and forward axis
 * (for directional / spot). Shadow technique (RT, shadow map, none)
 * attaches to the light via `add_technique`, not to the element.
 *
 *   auto sun = trait::render::create_directional_light(
 *       ::velk::color::white(), 1.f);
 *   sun.add_technique(create_rt_shadow());   // not wired up yet
 *   auto elem = ::velk::create_element();
 *   elem.add_trait(sun);
 */
class Light : public RenderTrait
{
public:
    Light() = default;
    explicit Light(IObject::Ptr obj) : RenderTrait(check_object<ILight>(obj)) {}
    explicit Light(ILight::Ptr l) : RenderTrait(as_object(l)) {}

    operator ILight::Ptr() const { return as_ptr<ILight>(); }

    auto get_type() const { return read_state_value<ILight>(&ILight::State::type); }
    void set_type(LightType v) { write_state_value<ILight>(&ILight::State::type, v); }

    auto get_color() const { return read_state_value<ILight>(&ILight::State::color); }
    void set_color(const color& v) { write_state_value<ILight>(&ILight::State::color, v); }

    auto get_intensity() const { return read_state_value<ILight>(&ILight::State::intensity); }
    void set_intensity(float v) { write_state_value<ILight>(&ILight::State::intensity, v); }

    auto get_range() const { return read_state_value<ILight>(&ILight::State::range); }
    void set_range(float v) { write_state_value<ILight>(&ILight::State::range, v); }

    auto get_cone_inner_deg() const
    {
        return read_state_value<ILight>(&ILight::State::cone_inner_deg);
    }
    void set_cone_inner_deg(float v)
    {
        write_state_value<ILight>(&ILight::State::cone_inner_deg, v);
    }

    auto get_cone_outer_deg() const
    {
        return read_state_value<ILight>(&ILight::State::cone_outer_deg);
    }
    void set_cone_outer_deg(float v)
    {
        write_state_value<ILight>(&ILight::State::cone_outer_deg, v);
    }
};

namespace trait::render {

/** @brief Creates a new Light trait (defaults to directional white). */
inline Light create_light()
{
    return Light(instance().create<IObject>(ClassId::Render::Light));
}

/** @brief Creates a directional light (sun-like). Direction comes from the owning element's forward axis. */
inline Light create_directional_light(color c = color::white(), float intensity = 1.f)
{
    Light l = create_light();
    l.set_type(LightType::Directional);
    l.set_color(c);
    l.set_intensity(intensity);
    return l;
}

/** @brief Creates a point light. Position comes from the owning element's translation. */
inline Light create_point_light(color c = color::white(), float intensity = 1.f, float range = 1000.f)
{
    Light l = create_light();
    l.set_type(LightType::Point);
    l.set_color(c);
    l.set_intensity(intensity);
    l.set_range(range);
    return l;
}

/** @brief Creates a spot light. Uses both position and forward axis of the owning element. */
inline Light create_spot_light(color c = color::white(), float intensity = 1.f,
                               float range = 1000.f,
                               float cone_inner_deg = 20.f, float cone_outer_deg = 30.f)
{
    Light l = create_light();
    l.set_type(LightType::Spot);
    l.set_color(c);
    l.set_intensity(intensity);
    l.set_range(range);
    l.set_cone_inner_deg(cone_inner_deg);
    l.set_cone_outer_deg(cone_outer_deg);
    return l;
}

} // namespace trait::render

} // namespace velk

#endif // VELK_UI_API_TRAIT_LIGHT_H
