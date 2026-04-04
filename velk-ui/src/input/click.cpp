#include "click.h"

#include <velk/api/state.h>

#ifdef VELK_INPUT_DEBUG
#define INPUT_LOG(...) VELK_LOG(I, __VA_ARGS__)
#else
#define INPUT_LOG(...) ((void)0)
#endif

namespace velk::ui {

InputResult Click::on_pointer_event(PointerEvent& event)
{
    if (event.action == PointerAction::Down) {
        INPUT_LOG("Click: pointer down, setting pressed=true");
        set_pressed(true);
        return InputResult::Consumed;
    }

    if (event.action == PointerAction::Up) {
        auto reader = read_state<IClick>(this);
        INPUT_LOG("Click: pointer up, pressed=%s", (reader && reader->pressed) ? "true" : "false");
        if (reader && reader->pressed) {
            set_pressed(false);
            INPUT_LOG("Click: firing on_click");
            invoke_event(get_interface(IInterface::UID), "on_click");
            return InputResult::Consumed;
        }
    }

    return InputResult::Ignored;
}

void Click::on_pointer_leave(const PointerEvent&)
{
    // Cancel press when pointer leaves bounds
    auto reader = read_state<IClick>(this);
    if (reader && reader->pressed) {
        INPUT_LOG("Click: pointer leave while pressed, cancelling");
        set_pressed(false);
    }
}

void Click::set_pressed(bool v)
{
    write_state<IClick>(this, [v](IClick::State& s) { s.pressed = v; });
}

} // namespace velk::ui
