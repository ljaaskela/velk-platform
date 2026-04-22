#include "camera.h"

#include <velk/api/state.h>

#include <cmath>

namespace velk::ui::impl {

mat4 Camera::get_view_projection(const mat4& world_matrix,
                                 float width, float height) const
{
    auto state = read_state<ICamera>(this);
    if (!state) {
        return mat4::identity();
    }

    // View matrix: affine inverse of the host's world transform
    // (transpose 3x3 rotation, negate rotated translation).
    mat4 view = mat4::identity();
    auto& w = world_matrix;
    view(0, 0) = w(0, 0); view(0, 1) = w(1, 0); view(0, 2) = w(2, 0);
    view(1, 0) = w(0, 1); view(1, 1) = w(1, 1); view(1, 2) = w(2, 1);
    view(2, 0) = w(0, 2); view(2, 1) = w(1, 2); view(2, 2) = w(2, 2);
    float tx = w(0, 3), ty = w(1, 3), tz = w(2, 3);
    view(0, 3) = -(view(0, 0) * tx + view(0, 1) * ty + view(0, 2) * tz);
    view(1, 3) = -(view(1, 0) * tx + view(1, 1) * ty + view(1, 2) * tz);
    view(2, 3) = -(view(2, 0) * tx + view(2, 1) * ty + view(2, 2) * tz);

    mat4 proj = mat4::zeros();
    float z = state->zoom > 0.f ? state->zoom : 1.f;

    if (state->projection == Projection::Ortho) {
        float hw = width / z;
        float hh = height / z;
        proj(0, 0) = 2.f / hw;
        proj(1, 1) = 2.f / hh;
        proj(2, 2) = -1.f;
        proj(0, 3) = -1.f;
        proj(1, 3) = -1.f;
        proj(3, 3) = 1.f;
    } else {
        float fov_rad = (state->fov / z) * 3.14159265f / 180.f;
        float f = 1.f / std::tan(fov_rad * 0.5f);
        float aspect = width / height;
        float n = state->near_clip;
        float fa = state->far_clip;
        // Vulkan clip-space: Z in [0, 1]. Y is left un-flipped because
        // velk's world Y points down (matching Vulkan's framebuffer Y
        // and the 2D UI convention). The world->framebuffer chain
        // produces a winding inversion for CCW-from-outside meshes,
        // which the backend accounts for via the default
        // FrontFace::Clockwise. See design-notes/coordinate_conventions.md.
        proj(0, 0) = f / aspect;
        proj(1, 1) = f;
        proj(2, 2) = -fa / (fa - n);
        proj(3, 2) = -1.f;
        proj(2, 3) = -(n * fa) / (fa - n);
    }

    return proj * view;
}

void Camera::screen_to_ray(const mat4& world_matrix, vec2 screen_pos,
                           float width, float height,
                           vec3& origin, vec3& direction) const
{
    auto state = read_state<ICamera>(this);
    float z = (state && state->zoom > 0.f) ? state->zoom : 1.f;

    float cx = world_matrix(0, 3);
    float cy = world_matrix(1, 3);

    if (!state || state->projection == Projection::Ortho) {
        float world_x = (screen_pos.x * width / z) + cx;
        float world_y = (screen_pos.y * height / z) + cy;
        origin = {world_x, world_y, 0.f};
        direction = {0.f, 0.f, 1.f};
    } else {
        float fov_rad = (state->fov / z) * 3.14159265f / 180.f;
        float half_h = std::tan(fov_rad * 0.5f);
        float aspect = width / height;

        float ndc_x = screen_pos.x * 2.f - 1.f;
        float ndc_y = screen_pos.y * 2.f - 1.f;

        origin = {cx, cy, 0.f};
        direction = {ndc_x * half_h * aspect, ndc_y * half_h, 1.f};
    }
}

} // namespace velk::ui::impl
