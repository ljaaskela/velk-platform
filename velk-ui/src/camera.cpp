#include "camera.h"

#include <velk/api/state.h>

#include <cmath>

namespace velk::ui {

mat4 CameraImpl::get_view_projection(const IElement& element,
                                     float width, float height) const
{
    auto state = read_state<ICamera>(this);
    if (!state) {
        return mat4::identity();
    }

    auto elem_state = read_state<IElement>(&element);

    // View matrix: inverse of camera's world transform
    mat4 view = mat4::identity();
    if (elem_state) {
        float cx = elem_state->world_matrix(0, 3);
        float cy = elem_state->world_matrix(1, 3);
        view(0, 3) = -cx;
        view(1, 3) = -cy;
    }

    // Projection matrix
    mat4 proj = mat4::zeros();
    float z = state->zoom > 0.f ? state->zoom : 1.f;

    if (state->projection == Projection::Ortho) {
        // Orthographic: visible world size = surface size / zoom
        float hw = width / z;
        float hh = height / z;
        proj(0, 0) = 2.f / hw;
        proj(1, 1) = 2.f / hh;
        proj(2, 2) = -1.f;
        proj(0, 3) = -1.f;
        proj(1, 3) = -1.f;
        proj(3, 3) = 1.f;
    } else {
        // Perspective
        float fov_rad = (state->fov / z) * 3.14159265f / 180.f;
        float f = 1.f / std::tan(fov_rad * 0.5f);
        float aspect = width / height;
        float n = state->near_clip;
        float fa = state->far_clip;
        proj(0, 0) = f / aspect;
        proj(1, 1) = f;
        proj(2, 2) = -(fa + n) / (fa - n);
        proj(3, 2) = -1.f;
        proj(2, 3) = -(2.f * fa * n) / (fa - n);
    }

    return proj * view;
}

void CameraImpl::screen_to_ray(const IElement& element, vec2 screen_pos,
                                float width, float height,
                                vec3& origin, vec3& direction) const
{
    auto state = read_state<ICamera>(this);
    auto elem_state = read_state<IElement>(&element);

    float z = (state && state->zoom > 0.f) ? state->zoom : 1.f;

    // Camera world position
    float cx = elem_state ? elem_state->world_matrix(0, 3) : 0.f;
    float cy = elem_state ? elem_state->world_matrix(1, 3) : 0.f;

    if (!state || state->projection == Projection::Ortho) {
        // Ortho: screen position maps directly to world position
        float world_x = (screen_pos.x * width / z) + cx;
        float world_y = (screen_pos.y * height / z) + cy;
        origin = {world_x, world_y, 0.f};
        direction = {0.f, 0.f, 1.f};
    } else {
        // Perspective: unproject through the frustum
        float fov_rad = (state->fov / z) * 3.14159265f / 180.f;
        float half_h = std::tan(fov_rad * 0.5f);
        float aspect = width / height;

        // NDC from normalized screen pos
        float ndc_x = screen_pos.x * 2.f - 1.f;
        float ndc_y = screen_pos.y * 2.f - 1.f;

        origin = {cx, cy, 0.f};
        direction = {ndc_x * half_h * aspect, ndc_y * half_h, 1.f};
    }
}

} // namespace velk::ui
