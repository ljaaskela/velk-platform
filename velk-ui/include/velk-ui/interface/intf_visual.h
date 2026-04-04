#ifndef VELK_UI_INTF_VISUAL_H
#define VELK_UI_INTF_VISUAL_H

#include <velk/api/math_types.h>
#include <velk/api/object_ref.h>
#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>

#include <velk-render/render_types.h>
#include <velk-ui/interface/intf_trait.h>

namespace velk::ui {

/**
 * @brief Visual representation attached to an element.
 *
 * Defines how an element appears on screen. An element can have one or more
 * IVisual attachments. The renderer iterates them and draws what they produce.
 */
class IVisual : public Interface<IVisual, ITrait>
{
public:
    VELK_INTERFACE(
        (PROP, ::velk::color, color, {}),
        (PROP, ObjectRef, paint, {}),
        (EVT, on_visual_changed)
    )

    virtual vector<DrawEntry> get_draw_entries(const rect& bounds) = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_VISUAL_H
