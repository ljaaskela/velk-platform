#include "matrix.h"

#include <velk/api/state.h>
#include <velk-scene/interface/intf_element.h>

namespace velk {

void Matrix::transform(IElement& element)
{
    const auto state = read_state<IMatrix>(this);
    const auto elem_state = read_state<IElement>(&element);
    if (state && elem_state) {
        mat4 world = elem_state->world_matrix * state->matrix;
        write_state<IElement>(&element, [&](IElement::State& es) { es.world_matrix = world; });
    }
}

} // namespace velk
