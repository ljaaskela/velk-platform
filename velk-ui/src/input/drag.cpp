#include "drag.h"

#include <velk/api/state.h>

namespace velk::ui {

InputResult Drag::on_pointer_event(PointerEvent& event)
{
    if (event.action == PointerAction::Down) {
        start_position_ = event.local_position;
        last_position_ = event.local_position;
        active_ = true;
        return InputResult::Captured;
    }

    if (!active_) {
        return InputResult::Ignored;
    }

    if (event.action == PointerAction::Move) {
        auto reader = read_state<IDrag>(this);
        if (reader && !reader->dragging) {
            set_dragging(true);
            invoke_event(get_interface(IInterface::UID), "on_drag_start");
        } else {
            invoke_event(get_interface(IInterface::UID), "on_drag_move");
        }
        last_position_ = event.local_position;
        return InputResult::Consumed;
    }

    if (event.action == PointerAction::Up || event.action == PointerAction::Cancel) {
        auto reader = read_state<IDrag>(this);
        if (reader && reader->dragging) {
            invoke_event(get_interface(IInterface::UID), "on_drag_end");
            set_dragging(false);
        }
        active_ = false;
        return InputResult::Consumed;
    }

    return InputResult::Ignored;
}

void Drag::set_dragging(bool v)
{
    write_state<IDrag>(this, [v](IDrag::State& s) { s.dragging = v; });
}

} // namespace velk::ui
