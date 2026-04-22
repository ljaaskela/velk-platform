#include "orbit.h"

#include <velk/api/state.h>
#include <velk-ui/interface/intf_element.h>

#include <cmath>

namespace velk::ui {

void Orbit::transform(IElement& element)
{
    auto state = read_state<IOrbit>(this);
    if (!state) {
        return;
    }
    auto target_obj = state->target.get<IElement>();
    auto target_state = read_state<IElement>(target_obj);
    if (!target_state) {
        return;
    }

    // Target position (center of target element)
    vec3 target{
        target_state->world_matrix(0, 3) + target_state->size.width * 0.5f,
        target_state->world_matrix(1, 3) + target_state->size.height * 0.5f,
        0.f
    };

    // velk-ui 3D world uses the same Y-down convention as the 2D UI
    // (matches Vulkan's framebuffer). The camera's local +Y axis is
    // aligned to world +Y — so "orbit up = +Y" is a local-axis
    // declaration, not a physical-up claim. Combined with Vulkan's
    // Y-down framebuffer, the resulting image renders upright.
    float yaw = deg_to_rad(state->yaw);
    float pitch = deg_to_rad(state->pitch);
    float d = state->distance;
    float cp = std::cos(pitch);

    vec3 eye{
        target.x + d * std::sin(yaw) * cp,
        target.y + d * std::sin(pitch),
        target.z + d * std::cos(yaw) * cp
    };

    // Forward = normalize(target - eye)
    vec3 fwd = target - eye;
    if (vec3::is_zero(fwd)) {
        return;
    }
    fwd = vec3::normalize(fwd);

    // Right = normalize(forward x up), up = (0, 1, 0) in camera-local
    // terms (see comment above).
    vec3 right = vec3::cross(fwd, vec3::unit_y());
    if (vec3::is_zero(right)) {
        return;
    }
    right = vec3::normalize(right);

    // Recompute up = right x forward
    vec3 up = vec3::cross(right, fwd);

    // Build the desired world matrix.
    mat4 new_world;
    new_world.set_col(0, right);
    new_world.set_col(1, up);
    new_world.set_col(2, -fwd);
    new_world.set_col(3, eye);

    // Skip the write if the camera element already has this exact matrix.
    // The layout solver overwrites world_matrix on every layout pass, so
    // checking the current state is the only reliable way to detect when
    // we need to re-apply the orbit transform.
    auto cam_state = read_state<IElement>(&element);
    if (cam_state && cam_state->world_matrix == new_world) {
        return;
    }

    write_state<IElement>(&element, [&](IElement::State& es) {
        es.world_matrix = new_world;
    });
}

} // namespace velk::ui
