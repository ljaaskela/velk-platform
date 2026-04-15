#ifndef VELK_UI_CAMERA_H
#define VELK_UI_CAMERA_H

#include <velk-ui/ext/trait.h>
#include <velk-ui/interface/intf_camera.h>
#include <velk-ui/plugin.h>

namespace velk::ui::impl {

/**
 * @brief Default camera trait implementation.
 *
 * Supports orthographic and perspective projection. The element's world
 * transform provides the camera position and orientation.
 */
class Camera : public ext::Render<Camera, ICamera>
{
public:
    VELK_CLASS_UID(ClassId::Render::Camera, "Camera");

    mat4 get_view_projection(const IElement& element,
                             float width, float height) const override;

    void screen_to_ray(const IElement& element, vec2 screen_pos,
                       float width, float height,
                       vec3& origin, vec3& direction) const override;
};

} // namespace velk::ui::impl

#endif // VELK_UI_CAMERA_H
