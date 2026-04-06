#include "look_at.h"

#include <velk/api/state.h>
#include <velk-ui/interface/intf_element.h>

#include <cmath>

namespace velk::ui {

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

    // Target position (center of target element + offset)
    float tx = target_state->world_matrix(0, 3) + target_state->size.width * 0.5f + state->target_offset.x;
    float ty = target_state->world_matrix(1, 3) + target_state->size.height * 0.5f + state->target_offset.y;
    float tz = state->target_offset.z;

    // Eye position from current world matrix
    float ex = elem_state->world_matrix(0, 3);
    float ey = elem_state->world_matrix(1, 3);
    float ez = elem_state->world_matrix(2, 3);

    // Forward = normalize(target - eye)
    float fx = tx - ex, fy = ty - ey, fz = tz - ez;
    float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-6f) {
        return;
    }
    fx /= fl; fy /= fl; fz /= fl;

    // Up = (0, 1, 0)
    // Right = normalize(forward x up)
    float rx = fy * 0.f - fz * 1.f;
    float ry = fz * 0.f - fx * 0.f;
    float rz = fx * 1.f - fy * 0.f;
    float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rl < 1e-6f) {
        return;
    }
    rx /= rl; ry /= rl; rz /= rl;

    // Recompute up = right x forward
    float ux = ry * fz - rz * fy;
    float uy = rz * fx - rx * fz;
    float uz = rx * fy - ry * fx;

    // Write world matrix: columns are right, up, -forward, position
    write_state<IElement>(&element, [&](IElement::State& es) {
        es.world_matrix(0, 0) = rx;  es.world_matrix(1, 0) = ry;  es.world_matrix(2, 0) = rz;
        es.world_matrix(0, 1) = ux;  es.world_matrix(1, 1) = uy;  es.world_matrix(2, 1) = uz;
        es.world_matrix(0, 2) = -fx; es.world_matrix(1, 2) = -fy; es.world_matrix(2, 2) = -fz;
        // Position unchanged
    });
}

} // namespace velk::ui
