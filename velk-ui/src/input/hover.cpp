#include "hover.h"

#include <velk/api/state.h>

namespace velk::ui {

void Hover::on_pointer_enter(const PointerEvent&)
{
    write_state<IHover>(this, [](IHover::State& s) { s.hovered = true; });
    invoke_event(get_interface(IInterface::UID), "on_hover_changed");
}

void Hover::on_pointer_leave(const PointerEvent&)
{
    write_state<IHover>(this, [](IHover::State& s) { s.hovered = false; });
    invoke_event(get_interface(IInterface::UID), "on_hover_changed");
}

} // namespace velk::ui
