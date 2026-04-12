#ifndef VELK_UI_API_TRAIT_TRS_H
#define VELK_UI_API_TRAIT_TRS_H

#include <velk/api/state.h>

#include <velk-ui/api/trait.h>
#include <velk-ui/interface/trait/intf_trs.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around ITrs.
 *
 *   auto trs = trait::transform::create_trs();
 *   trs.set_rotation(45.f);
 *   trs.set_scale({0.5f, 0.5f});
 *   elem.add_trait(trs);
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

    auto get_rotation() const { return read_state_value<ITrs>(&ITrs::State::rotation); }
    void set_rotation(const vec3& v) { write_state_value<ITrs>(&ITrs::State::rotation, v); }

    auto get_scale() const { return read_state_value<ITrs>(&ITrs::State::scale); }
    void set_scale(const vec2& v) { write_state_value<ITrs>(&ITrs::State::scale, v); }
};

namespace trait::transform {

/** @brief Creates a new Trs transform trait. */
inline Trs create_trs()
{
    return Trs(instance().create<ITrs>(ClassId::Transform::Trs));
}

} // namespace trait::transform

} // namespace velk::ui

#endif // VELK_UI_API_TRAIT_TRS_H
