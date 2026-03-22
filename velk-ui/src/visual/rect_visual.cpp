#include "rect_visual.h"

#include <velk/api/state.h>

namespace velk_ui {

velk::vector<DrawCommand> RectVisual::get_draw_commands(const velk::rect& bounds)
{
    auto state = velk::read_state<IVisual>(this);
    if (!state) {
        return {};
    }

    DrawCommand cmd{};
    cmd.type = DrawCommandType::FillRect;
    cmd.bounds = bounds;
    cmd.color = state->color;

    return {cmd};
}

} // namespace velk_ui
