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

/**
 * @brief What a deferred view shows in place of its lit image, to see the state of the
 * renderer's structures.
 */
enum class DebugView : uint8_t
{
    None,
    Draw,          ///< A flat colour per draw.
    Instance,      ///< Per instance.
    Triangle,      ///< Per triangle.
    Pyramid,       ///< The occlusion depth pyramid.
    Compute,       ///< What compute raster drew.
    TriangleSize,  ///< How large triangles are on screen.
    Cluster,       ///< A flat colour per level of detail cluster of each instance.
    Level,         ///< Each cluster coloured by its level of detail, full detail first.
    ClusterSize,   ///< How wide each cluster is on screen.
    /// Instances dimmed, under the bounds of their visual's partitions: red while a partition
    /// chooses its levels of detail again this frame, cyan while it keeps them.
    Partition,
    /// Instances dimmed, under the boxes of one level of the trees rays walk, the one
    /// ICamera::debug_level says: 0 for leaves of four records, up to each partition's root.
    RayTree,
};

/**
 * @brief Camera render-trait defining how a scene is observed.
 *
 * Attached to a scene element. The world transform is supplied at
 * query time rather than read from the element — this keeps `ICamera`
 * independent of the velk-ui element hierarchy and usable from any
 * context that can produce a world matrix (game engine host, editor
 * viewport, headless renderer, ...).
 *
 * The render path (Forward / Deferred / RT / plugin-provided) is
 * attached to this trait via `Camera::add_render_path` (velk-scene),
 * not stored as an enum here, so plugins can ship custom paths
 * without modifying ICamera.
 */
class ICamera : public Interface<ICamera, IRenderTrait>
{
public:
    VELK_INTERFACE(
        (PROP, Projection, projection, Projection::Ortho),
        (PROP, float, zoom, 1.f),
        (PROP, float, scale, 1.f),
        (PROP, float, fov, 60.f),
        (PROP, float, near_clip, 0.1f),
        (PROP, float, far_clip, 1000.f),
        (PROP, bool, occlusion_culling, true), ///< Whether the deferred path skips drawing what is hidden behind other geometry.
        /// Clusters whose mean triangle is smaller than this on screen, in pixels across, are
        /// rasterized in compute instead of by the hardware, by the deferred path. 0 for none.
        (PROP, float, tiny_cluster_pixels, 2.f),
        (PROP, ObjectRef, environment, {}), ///< Optional IEnvironment for skybox/background.
        /// What the deferred path shows in place of the lit image. VELK_VISIBILITY_DEBUG overrides it.
        (PROP, DebugView, debug_view, DebugView::None),
        (PROP, uint32_t, debug_level, 0u), ///< Which level a debug view of a tree shows. See DebugView::RayTree.
        /// Another camera's element. When set, the deferred path draws what that camera would
        /// draw (its view, its levels of detail) as seen from this one, and draws its view's
        /// edges, so that what another view holds can be looked at from outside. Occlusion
        /// culling, compute raster and far instances are left out of such a view, since they
        /// only hold for the camera that chooses.
        (PROP, ObjectRef, choose_as, {})
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
