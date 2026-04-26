#ifndef VELK_UI_API_INPUT_HOVER_H
#define VELK_UI_API_INPUT_HOVER_H

#include <velk/api/event.h>
#include <velk/api/state.h>

#include <velk-scene/api/trait.h>
#include <velk-ui/interface/trait/intf_hover.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around IHover.
 *
 *   auto hover = trait::input::create_hover();
 *   element.add_trait(hover);
 *   hover.on_hover_changed().add_handler([](auto) { ... });
 */
class Hover : public Trait
{
public:
    Hover() = default;
    explicit Hover(IObject::Ptr obj) : Trait(check_object<IHover>(obj)) {}
    explicit Hover(IHover::Ptr h) : Trait(as_object(h)) {}

    operator IHover::Ptr() const { return as_ptr<IHover>(); }

    /** @brief Returns true while the pointer is over this element. */
    auto is_hovered() const { return read_state_value<IHover>(&IHover::State::hovered); }

    /** @brief Returns the on_hover_changed event. */
    auto on_hover_changed() const
    {
        return with<IHover>([](auto& h) { return h.on_hover_changed(); });
    }
};

namespace trait::input {

/** @brief Creates a new Hover input trait. */
inline Hover create_hover()
{
    return Hover(instance().create<IHover>(ClassId::Input::Hover));
}

} // namespace trait::input

} // namespace velk::ui

#endif // VELK_UI_API_INPUT_HOVER_H
