#ifndef VELK_UI_API_TRAIT_LOOK_AT_H
#define VELK_UI_API_TRAIT_LOOK_AT_H

#include <velk/api/state.h>

#include <velk-ui/api/trait.h>
#include <velk-ui/interface/trait/intf_look_at.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around ILookAt.
 *
 *   auto la = transform::create_look_at();
 *   la.set_target(target_element);
 *   elem.add_trait(la);
 */
class LookAtTrait : public Trait
{
public:
    LookAtTrait() = default;
    explicit LookAtTrait(IObject::Ptr obj) : Trait(check_object<ILookAt>(obj)) {}
    explicit LookAtTrait(ILookAt::Ptr p) : Trait(as_object(p)) {}

    operator ILookAt::Ptr() const { return as_ptr<ILookAt>(); }

    void set_target(const IObject::Ptr& t)
    {
        write_state<ILookAt>([&](ILookAt::State& s) { set_object_ref(s.target, t); });
    }
    void set_target_offset(vec3 v) { write_state_value<ILookAt>(&ILookAt::State::target_offset, v); }
};

namespace transform {

/** @brief Creates a new LookAt transform trait. */
inline LookAtTrait create_look_at()
{
    return LookAtTrait(instance().create<ILookAt>(ClassId::Transform::LookAt));
}

} // namespace transform

} // namespace velk::ui

#endif // VELK_UI_API_TRAIT_LOOK_AT_H
