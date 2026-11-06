#include "look_at.h"

#include <velk/api/state.h>
#include <velk-scene/interface/intf_element.h>

namespace velk {

mat4 LookAt::transform(IElement&, const mat4& world)
{
    auto state = read_state<ILookAt>(this);
    if (!state) {
        return world;
    }

    auto target_obj = state->target.get<IElement>();
    if (!target_obj) {
        return world;
    }

    auto target_state = read_state<IElement>(target_obj);
    if (!target_state) {
        return world;
    }

    // Eye position from the incoming world matrix
    vec3 eye{world(0, 3), world(1, 3), world(2, 3)};

    // Target position (center of target element + offset)
    vec3 target{
        target_state->world_matrix(0, 3) + target_state->size.width * 0.5f + state->target_offset.x,
        target_state->world_matrix(1, 3) + target_state->size.height * 0.5f + state->target_offset.y,
        state->target_offset.z
    };

    // Forward = normalize(target - eye)
    vec3 fwd = target - eye;
    if (vec3::is_zero(fwd)) {
        return world;
    }
    fwd = vec3::normalize(fwd);

    // Right = normalize(forward x up), up = (0, 1, 0)
    vec3 right = vec3::cross(fwd, vec3::unit_y());
    if (vec3::is_zero(right)) {
        return world;
    }
    right = vec3::normalize(right);

    // Recompute up = right x forward
    vec3 up = vec3::cross(right, fwd);

    // Columns are right, up, -forward (position unchanged)
    mat4 result = world;
    result.set_col(0, right);
    result.set_col(1, up);
    result.set_col(2, -fwd);
    return result;
}

} // namespace velk
