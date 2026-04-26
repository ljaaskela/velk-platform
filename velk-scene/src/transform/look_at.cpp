#include "look_at.h"

#include <velk/api/state.h>
#include <velk-scene/interface/intf_element.h>

namespace velk {

void LookAt::transform(IElement& element)
{
    auto state = read_state<ILookAt>(this);
    if (!state) {
        return;
    }

    auto target_obj = state->target.get<IElement>();
    if (!target_obj) {
        return;
    }

    auto target_state = read_state<IElement>(target_obj);
    auto elem_state = read_state<IElement>(&element);
    if (!target_state || !elem_state) {
        return;
    }

    vec3 eye_in{
        elem_state->world_matrix(0, 3),
        elem_state->world_matrix(1, 3),
        elem_state->world_matrix(2, 3)
    };
    CacheKey key{
        target_obj.get(),
        {target_state->world_matrix(0, 3), target_state->world_matrix(1, 3), target_state->world_matrix(2, 3)},
        target_state->size,
        state->target_offset,
        eye_in
    };
    if (!cache_.changed(key)) {
        return;
    }

    // Target position (center of target element + offset)
    vec3 target{
        target_state->world_matrix(0, 3) + target_state->size.width * 0.5f + state->target_offset.x,
        target_state->world_matrix(1, 3) + target_state->size.height * 0.5f + state->target_offset.y,
        state->target_offset.z
    };

    // Eye position from current world matrix
    const vec3& eye = eye_in;

    // Forward = normalize(target - eye)
    vec3 fwd = target - eye;
    if (vec3::is_zero(fwd)) {
        return;
    }
    fwd = vec3::normalize(fwd);

    // Right = normalize(forward x up), up = (0, 1, 0)
    vec3 right = vec3::cross(fwd, vec3::unit_y());
    if (vec3::is_zero(right)) {
        return;
    }
    right = vec3::normalize(right);

    // Recompute up = right x forward
    vec3 up = vec3::cross(right, fwd);

    // Write world matrix: columns are right, up, -forward (position unchanged)
    write_state<IElement>(&element, [&](IElement::State& es) {
        es.world_matrix.set_col(0, right);
        es.world_matrix.set_col(1, up);
        es.world_matrix.set_col(2, -fwd);
    });
}

} // namespace velk
