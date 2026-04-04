#include "matrix.h"

#include <velk/api/state.h>
#include <velk-ui/interface/intf_element.h>

namespace velk::ui {

void Matrix::transform(IElement& element)
{
    auto state = read_state<IMatrix>(this);
    if (!state) {
        return;
    }

    auto elem_state = read_state<IElement>(&element);
    if (!elem_state) {
        return;
    }

    mat4 world = elem_state->world_matrix * state->matrix;
    write_state<IElement>(&element, [&](IElement::State& es) { es.world_matrix = world; });
}

} // namespace velk::ui
