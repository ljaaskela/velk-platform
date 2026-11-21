#include "fixed_size.h"

#include <velk/api/state.h>

namespace velk::ui {

void FixedSize::describe(LayoutBox& box) const
{
    if (auto state = read_state<IFixedSize>(this)) {
        if (state->width.unit != DimUnit::None) box.width = state->width;
        if (state->height.unit != DimUnit::None) box.height = state->height;
        if (state->depth.unit != DimUnit::None) box.depth = state->depth;
    }
}

} // namespace velk::ui
