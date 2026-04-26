#ifndef VELK_UI_API_TRAIT_TRS_H
#define VELK_UI_API_TRAIT_TRS_H

#include <velk/api/state.h>

#include <velk-scene/api/trait.h>
#include <velk-scene/interface/trait/intf_trs.h>
#include <velk-scene/plugin.h>

namespace velk {

/**
 * @brief Convenience wrapper around ITrs.
 *
 *   auto trs = trait::transform::create_trs();
 *   trs.set_rotation({0.f, 45.f, 0.f});   // Euler degrees (XYZ)
 *   trs.set_scale({0.5f, 0.5f, 0.5f});
 *   elem.add_trait(trs);
 *
 * Rotation is stored as a quaternion on the interface; Euler-degree
 * accessors are retained for hand-authored content and convert to / from
 * quat internally.
 */
class Trs : public Trait
{
public:
    Trs() = default;
    explicit Trs(IObject::Ptr obj) : Trait(check_object<ITrs>(obj)) {}
    explicit Trs(ITrs::Ptr t) : Trait(as_object(t)) {}

    operator ITrs::Ptr() const { return as_ptr<ITrs>(); }

    auto get_translate() const { return read_state_value<ITrs>(&ITrs::State::translate); }
    void set_translate(const vec3& v) { write_state_value<ITrs>(&ITrs::State::translate, v); }

    /// Native quaternion accessor.
    quat get_rotation_quat() const { return read_state_value<ITrs>(&ITrs::State::rotation); }
    void set_rotation_quat(const quat& q) { write_state_value<ITrs>(&ITrs::State::rotation, q); }

    /// Euler XYZ in degrees, matching the legacy `ITrs` rotation semantics
    /// (Rx * Ry * Rz). Rounds-trips for typical authored rotations; near
    /// gimbal lock the quat round-trip is not lossless.
    vec3 get_rotation() const
    {
        quat q = get_rotation_quat();
        vec3 r = q.to_euler();
        constexpr float kRadToDeg = 57.2957795130823f;
        return {r.x * kRadToDeg, r.y * kRadToDeg, r.z * kRadToDeg};
    }
    void set_rotation(const vec3& euler_degrees)
    {
        constexpr float kDegToRad = 0.0174532925199433f;
        set_rotation_quat(quat::from_euler({
            euler_degrees.x * kDegToRad,
            euler_degrees.y * kDegToRad,
            euler_degrees.z * kDegToRad
        }));
    }

    auto get_scale() const { return read_state_value<ITrs>(&ITrs::State::scale); }
    void set_scale(const vec3& v) { write_state_value<ITrs>(&ITrs::State::scale, v); }
};

namespace trait::transform {

/** @brief Creates a new Trs transform trait. */
inline Trs create_trs()
{
    return Trs(instance().create<ITrs>(ClassId::Transform::Trs));
}

} // namespace trait::transform

} // namespace velk

#endif // VELK_UI_API_TRAIT_TRS_H
