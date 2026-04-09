#ifndef VELK_UI_INTF_CAMERA_H
#define VELK_UI_INTF_CAMERA_H

#include <velk/api/object_ref.h>
#include <velk-ui/interface/intf_element.h>
#include <velk-ui/interface/intf_trait.h>

namespace velk::ui {

/** @brief Projection type for cameras. */
enum class Projection : uint8_t
{
    Ortho,
    Perspective,
};

/**
 * @brief Camera trait that defines how a scene is observed.
 *
 * Attached to an element in the scene hierarchy. The element's world transform
 * provides the camera position and orientation. The camera trait adds projection
 * behavior and can convert screen coordinates to world-space rays.
 */
class ICamera : public Interface<ICamera, ITrait>
{
public:
    VELK_INTERFACE(
        (PROP, Projection, projection, Projection::Ortho),
        (PROP, float, zoom, 1.f),
        (PROP, float, scale, 1.f),
        (PROP, float, fov, 60.f),
        (PROP, float, near_clip, 0.1f),
        (PROP, float, far_clip, 1000.f),
        (PROP, ObjectRef, environment, {}) ///< Optional IEnvironment for skybox/background.
    )

    /**
     * @brief Computes the combined view-projection matrix.
     * @param element The element this camera is attached to (provides world transform).
     * @param width   Surface width in pixels.
     * @param height  Surface height in pixels.
     */
    virtual mat4 get_view_projection(const IElement& element,
                                     float width, float height) const = 0;

    /**
     * @brief Converts a screen-space point to a world-space ray.
     * @param element    The element this camera is attached to.
     * @param screen_pos Normalized screen position (0..1, 0..1).
     * @param width      Surface width in pixels.
     * @param height     Surface height in pixels.
     * @param origin     [out] Ray origin in world space.
     * @param direction  [out] Ray direction in world space.
     */
    virtual void screen_to_ray(const IElement& element, vec2 screen_pos,
                               float width, float height,
                               vec3& origin, vec3& direction) const = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_CAMERA_H
