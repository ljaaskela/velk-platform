#ifndef VELK_UI_CAMERA_H
#define VELK_UI_CAMERA_H

#include <velk/ext/object.h>

#include <velk-render/interface/intf_camera.h>
#include <velk-ui/plugin.h>

namespace velk::ui::impl {

/**
 * @brief Default camera render-trait implementation.
 *
 * Supports orthographic and perspective projection. The host's world
 * transform is supplied at query time (via `get_view_projection`).
 */
class Camera : public ::velk::ext::Object<Camera, ::velk::ICamera>
{
public:
    VELK_CLASS_UID(ClassId::Render::Camera, "Camera");

    mat4 get_view_projection(const mat4& world_matrix,
                             float width, float height) const override;

    void screen_to_ray(const mat4& world_matrix, vec2 screen_pos,
                       float width, float height,
                       vec3& origin, vec3& direction) const override;
};

} // namespace velk::ui::impl

#endif // VELK_UI_CAMERA_H
