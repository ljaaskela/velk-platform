#include "trs.h"

#include <velk/api/state.h>
#include <velk-scene/interface/intf_element.h>

namespace velk {

void Trs::transform(IElement& element)
{
    auto state = read_state<ITrs>(this);
    if (!state) {
        return;
    }

    mat4 t = mat4::translate(state->translate);
    mat4 r = state->rotation.to_mat4();
    mat4 s = mat4::scale(state->scale);
    mat4 trs = t * r * s;

    auto elem_state = read_state<IElement>(&element);
    if (!elem_state) {
        return;
    }

    mat4 world = elem_state->world_matrix * trs;
    write_state<IElement>(&element, [&](IElement::State& es) { es.world_matrix = world; });
}

} // namespace velk
