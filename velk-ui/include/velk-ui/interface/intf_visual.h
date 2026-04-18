#ifndef VELK_UI_INTF_VISUAL_H
#define VELK_UI_INTF_VISUAL_H

#include <velk/api/math_types.h>
#include <velk/api/object_ref.h>
#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>

#include <velk/string_view.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/render_types.h>
#include <velk-ui/interface/intf_trait.h>

namespace velk::ui {

/** @brief Controls when a visual draws relative to an element's children. */
enum class VisualPhase : uint8_t
{
    /** @brief Draw the visual before any of the children of the Element the visual is attached to.
               This is the default and the correct choice for most use cases. */
    BeforeChildren = 0,
    /** @brief Draw the visual after drawing the child hierarchy of the Element the Visual is attached to.
               A typical use case would be a border, overlay, focus indicator. */
    AfterChildren,
};

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
        (PROP, VisualPhase, visual_phase, VisualPhase::BeforeChildren)
    )

    /** @brief Returns draw entries for this visual within the given bounds. */
    virtual vector<DrawEntry> get_draw_entries(const rect& bounds) = 0;

    /**
     * @brief Returns GPU resources used by this visual that need uploading
     *        and lifetime tracking by the renderer.
     *
     * Returned by value (not array_view) so the visual is free to construct
     * the list however it likes (member, fresh allocation, conditional
     * inclusion) without committing to backing storage, and so the renderer
     * sees a stable snapshot with no aliasing into visual-internal state.
     *
     * The default returns an empty vector for visuals that have no GPU
     * resources (rect, rounded rect, gradient, etc.).
     */
    virtual vector<IBuffer::Ptr> get_gpu_resources() const { return {}; }

    // Shader provision and analytic-shape dispatch live on separate
    // role interfaces (`IRasterShader`, `IAnalyticShape`). A visual
    // that contributes its own pipeline implements `IRasterShader`;
    // a visual that's an analytic RT primitive implements
    // `IAnalyticShape`. Visuals can implement either, both, or
    // neither (e.g. a TextureVisual that piggybacks on a paint-
    // supplied material).
};

} // namespace velk::ui

#endif // VELK_UI_INTF_VISUAL_H
