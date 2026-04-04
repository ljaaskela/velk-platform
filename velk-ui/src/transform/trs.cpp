#include "trs.h"

#include <velk/api/state.h>
#include <velk-ui/interface/intf_element.h>

namespace velk::ui {

static constexpr float deg_to_rad(float deg) { return deg * 3.14159265358979323846f / 180.f; }

void Trs::transform(IElement& element)
{
    auto state = read_state<ITrs>(this);
    if (!state) {
        return;
    }

    mat4 t = mat4::translate(state->translate);
    mat4 r = mat4::rotate_z(deg_to_rad(state->rotation));
    mat4 s = mat4::scale({state->scale.x, state->scale.y, 1.f});
    mat4 trs = t * r * s;

    auto elem_state = read_state<IElement>(&element);
    if (!elem_state) {
        return;
    }

    mat4 world = elem_state->world_matrix * trs;
    write_state<IElement>(&element, [&](IElement::State& es) { es.world_matrix = world; });
}

} // namespace velk::ui
