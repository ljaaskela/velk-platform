#ifndef VELK_UI_API_INPUT_DRAG_H
#define VELK_UI_API_INPUT_DRAG_H

#include <velk/api/event.h>
#include <velk/api/state.h>

#include <velk-ui/api/trait.h>
#include <velk-ui/interface/trait/intf_drag.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around IDrag.
 *
 *   auto drag = input::create_drag();
 *   element.add_trait(drag);
 *   drag.on_drag_start().add_handler([](auto) { ... });
 */
class Drag : public Trait
{
public:
    Drag() = default;
    explicit Drag(IObject::Ptr obj) : Trait(check_object<IDrag>(obj)) {}
    explicit Drag(IDrag::Ptr d) : Trait(as_object(d)) {}

    operator IDrag::Ptr() const { return as_ptr<IDrag>(); }

    /** @brief Returns true while a drag gesture is active. */
    auto is_dragging() const { return read_state_value<IDrag>(&IDrag::State::dragging); }

    /** @brief Returns the on_drag_start event. */
    auto on_drag_start() const
    {
        return with<IDrag>([](auto& d) { return d.on_drag_start(); });
    }

    /** @brief Returns the on_drag_move event. */
    auto on_drag_move() const
    {
        return with<IDrag>([](auto& d) { return d.on_drag_move(); });
    }

    /** @brief Returns the on_drag_end event. */
    auto on_drag_end() const
    {
        return with<IDrag>([](auto& d) { return d.on_drag_end(); });
    }
};

namespace input {

/** @brief Creates a new Drag input trait. */
inline Drag create_drag()
{
    return Drag(instance().create<IDrag>(ClassId::Input::Drag));
}

} // namespace input

} // namespace velk::ui

#endif // VELK_UI_API_INPUT_DRAG_H
