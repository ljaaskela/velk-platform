#include "insets.h"

#include <velk/api/state.h>

namespace velk::ui {

void Padding::describe(LayoutBox& box) const
{
    if (auto state = read_state<IPadding>(this)) {
        box.padding = {state->left, state->top, state->right, state->bottom};
    }
}

void Margin::describe(LayoutBox& box) const
{
    if (auto state = read_state<IMargin>(this)) {
        box.margin = {state->left, state->top, state->right, state->bottom};
    }
}

void Align::describe(LayoutBox& box) const
{
    if (auto state = read_state<IAlign>(this)) {
        if (state->horizontal != Alignment::Unset) box.align_x = state->horizontal;
        if (state->vertical != Alignment::Unset) box.align_y = state->vertical;
    }
}

} // namespace velk::ui
