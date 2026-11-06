#include "trs.h"

#include <velk/api/state.h>

namespace velk {

mat4 Trs::transform(IElement&, const mat4& world)
{
    auto state = read_state<ITrs>(this);
    if (!state) {
        return world;
    }

    mat4 t = mat4::translate(state->translate);
    mat4 r = state->rotation.to_mat4();
    mat4 s = mat4::scale(state->scale);
    mat4 trs = t * r * s;

    return world * trs;
}

} // namespace velk
