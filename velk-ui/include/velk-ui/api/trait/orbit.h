#ifndef VELK_UI_API_TRAIT_ORBIT_H
#define VELK_UI_API_TRAIT_ORBIT_H

#include <velk/api/state.h>

#include <velk-ui/api/trait.h>
#include <velk-ui/interface/trait/intf_orbit.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around IOrbit.
 *
 *   auto orb = transform::create_orbit();
 *   orb.set_target(target_element);
 *   orb.set_distance(1200.f);
 *   orb.set_yaw(30.f);
 *   orb.set_pitch(15.f);
 *   cam_element.add_trait(orb);
 */
class OrbitTrait : public Trait
{
public:
    OrbitTrait() = default;
    explicit OrbitTrait(IObject::Ptr obj) : Trait(check_object<IOrbit>(obj)) {}
    explicit OrbitTrait(IOrbit::Ptr p) : Trait(as_object(p)) {}

    operator IOrbit::Ptr() const { return as_ptr<IOrbit>(); }

    void set_target(const IObject::Ptr& t)
    {
        write_state<IOrbit>([&](IOrbit::State& s) { set_object_ref(s.target, t); });
    }
    void set_distance(float v) { write_state_value<IOrbit>(&IOrbit::State::distance, v); }
    void set_yaw(float v) { write_state_value<IOrbit>(&IOrbit::State::yaw, v); }
    void set_pitch(float v) { write_state_value<IOrbit>(&IOrbit::State::pitch, v); }
};

namespace transform {

/** @brief Creates a new Orbit transform trait. */
inline OrbitTrait create_orbit()
{
    return OrbitTrait(instance().create<IOrbit>(ClassId::Transform::Orbit));
}

} // namespace transform

} // namespace velk::ui

#endif // VELK_UI_API_TRAIT_ORBIT_H
