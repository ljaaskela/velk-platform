#ifndef VELK_RENDER_INTF_CAMERA_H
#define VELK_RENDER_INTF_CAMERA_H

#include <velk/api/math_types.h>
#include <velk/api/object_ref.h>

#include <velk-render/interface/intf_render_trait.h>

namespace velk {

/** @brief Projection type for cameras. */
enum class Projection : uint8_t
{
    Ortho,
    Perspective,
};

/** @brief How the renderer produces pixels for a view driven by this camera. */
enum class RenderPath : uint8_t
{
    Raster,   ///< Conventional rasterization pass (default).
    RayTrace, ///< Compute-dispatch ray tracer writing to a storage image.
};

/**
 * @brief Camera render-trait defining how a scene is observed.
 *
 * Attached to a scene element. The world transform is supplied at
 * query time rather than read from the element — this keeps `ICamera`
 * independent of the velk-ui element hierarchy and usable from any
 * context that can produce a world matrix (game engine host, editor
 * viewport, headless renderer, ...).
 */
class ICamera : public Interface<ICamera, IRenderTrait>
{
public:
    VELK_INTERFACE(
        (PROP, Projection, projection, Projection::Ortho),
        (PROP, RenderPath, render_path, RenderPath::Raster),
        (PROP, float, zoom, 1.f),
        (PROP, float, scale, 1.f),
        (PROP, float, fov, 60.f),
        (PROP, float, near_clip, 0.1f),
        (PROP, float, far_clip, 1000.f),
        (PROP, ObjectRef, environment, {}) ///< Optional IEnvironment for skybox/background.
    )

    /**
     * @brief Computes the combined view-projection matrix.
     * @param world_matrix World transform of the host (camera position/orientation).
     * @param width  Surface width in pixels.
     * @param height Surface height in pixels.
     */
    virtual mat4 get_view_projection(const mat4& world_matrix,
                                     float width, float height) const = 0;

    /**
     * @brief Converts a screen-space point to a world-space ray.
     * @param world_matrix World transform of the host.
     * @param screen_pos   Normalized screen position (0..1, 0..1).
     * @param width        Surface width in pixels.
     * @param height       Surface height in pixels.
     * @param origin       [out] Ray origin in world space.
     * @param direction    [out] Ray direction in world space.
     */
    virtual void screen_to_ray(const mat4& world_matrix, vec2 screen_pos,
                               float width, float height,
                               vec3& origin, vec3& direction) const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_CAMERA_H
