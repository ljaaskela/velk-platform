#ifndef VELK_UI_INTF_ELEMENT_H
#define VELK_UI_INTF_ELEMENT_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>

#include <cstdint>
#include <velk-ui/types.h>

namespace velk::ui {

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
        (PROP, vec3, position, {}),      ///< Position in parent-local space.
        (PROP, ::velk::size, size, {}),          ///< Element size (width, height).
        (RPROP, mat4, world_matrix, {}), ///< Computed world-space transform. Written by solver.
        (PROP, int32_t, z_index, 0)            ///< Draw order among siblings. Higher draws on top.
    )

    /** @brief Returns the scene this element belongs to, or nullptr. */
    virtual shared_ptr<IScene> get_scene() const = 0;

    /** @brief Atomically reads and clears accumulated dirty flags for this element. */
    virtual DirtyFlags consume_dirty() = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_ELEMENT_H
