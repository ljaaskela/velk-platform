#ifndef VELK_UI_INTF_VISUAL_H
#define VELK_UI_INTF_VISUAL_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>

#include <velk-ui/interface/intf_trait.h>
#include <velk-ui/types.h>

namespace velk_ui {

/**
 * @brief Visual representation attached to an element.
 *
 * Defines how an element appears on screen. An element can have one or more
 * IVisual attachments. The renderer iterates them and draws what they produce.
 * The renderer only knows IVisual; it never needs to know about specific visual
 * types (RectVisual, TextVisual, etc.).
 */
class IVisual : public velk::Interface<IVisual, ITrait>
{
public:
    VELK_INTERFACE(
        (PROP, velk::color, color, {}),  ///< Base color. Fill color for rect, text color for text.
        (EVT, on_visual_changed)         ///< Fired when visual state changes (color, text, etc.).
    )

    /**
     * @brief Produces draw commands for this visual within the given bounds.
     * @param bounds Element-local rect (from layout). The visual fills within this space.
     * @return Draw commands in element-local space. The renderer applies world_matrix.
     */
    virtual velk::vector<DrawCommand> get_draw_commands(const velk::rect& bounds) = 0;
};

} // namespace velk_ui

#endif // VELK_UI_INTF_VISUAL_H
