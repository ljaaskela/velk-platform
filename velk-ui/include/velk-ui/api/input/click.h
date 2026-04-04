#ifndef VELK_UI_API_INPUT_CLICK_H
#define VELK_UI_API_INPUT_CLICK_H

#include <velk/api/event.h>
#include <velk/api/state.h>

#include <velk-ui/api/trait.h>
#include <velk-ui/interface/trait/intf_click.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around IClick.
 *
 *   auto click = input::create_click();
 *   element.add_trait(click);
 *   click.on_click().add_handler([](auto) { ... });
 */
class Click : public Trait
{
public:
    Click() = default;
    explicit Click(IObject::Ptr obj) : Trait(check_object<IClick>(obj)) {}
    explicit Click(IClick::Ptr c) : Trait(as_object(c)) {}

    operator IClick::Ptr() const { return as_ptr<IClick>(); }

    /** @brief Returns true while the pointer is down on this element. */
    auto is_pressed() const { return read_state_value<IClick>(&IClick::State::pressed); }

    /** @brief Returns the on_click event handler. */
    auto on_click() const
    {
        return with<IClick>([](auto& c) { return c.on_click(); });
    }
};

namespace input {

/** @brief Creates a new Click input trait. */
inline Click create_click()
{
    return Click(instance().create<IClick>(ClassId::Input::Click));
}

} // namespace input

} // namespace velk::ui

#endif // VELK_UI_API_INPUT_CLICK_H
