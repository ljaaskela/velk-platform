#ifndef VELK_UI_INTF_ELEMENT_H
#define VELK_UI_INTF_ELEMENT_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>

#include <cstdint>
#include <velk-render/render_types.h>
#include <velk-scene/types.h>

namespace velk {

class IScene;

/**
 * @brief Base interface for all UI elements.
 *
 * Provides position, size, and z-ordering properties.
 * Visual appearance is defined by IVisual attachments (not by IElement itself).
 * The solver writes world_matrix; user code reads it.
 */
class IElement : public Interface<IElement>
{
public:
    VELK_INTERFACE(
        (PROP, vec3, position, {}),                           ///< Position in parent-local space.
        (PROP, ::velk::size, size, {}),                       ///< Element size (width, height).
        (RPROP, mat4, world_matrix, {}),                      ///< Computed world-space transform. Written by solver.
        (RPROP, aabb, world_aabb, {}),                        ///< World-space bounds of this element's box and visuals, excluding children. Written by solver.
        (PROP, int32_t, z_index, 0),                          ///< Draw order among siblings. Higher draws on top.
        (PROP, BlendMode, blend_mode, BlendMode::Alpha),      ///< Blend mode when compositing.
        (PROP, RenderMode, render_mode, RenderMode::Default)  ///< Controls surface vs trait rendering.
    )

    /** @brief Returns the scene this element belongs to, or nullptr. */
    virtual shared_ptr<IScene> get_scene() const = 0;

    /** @brief Atomically reads and clears accumulated dirty flags for this element. */
    virtual DirtyFlags consume_dirty() = 0;

    /** @brief Returns the accumulated dirty flags without clearing them. */
    virtual DirtyFlags pending_dirty() const = 0;

    /**
     * @brief Adds dirty flags and notifies the scene. Does nothing when the
     *        element is not in a scene.
     *
     * State writes do not say which member changed, so code that writes size,
     * position or world_matrix directly calls this to report it.
     */
    virtual void mark_dirty(DirtyFlags flags) = 0;

    virtual bool has_render_traits() const = 0;
};

} // namespace velk

#endif // VELK_UI_INTF_ELEMENT_H
