#ifndef VELK_UI_INTF_RENDERER_H
#define VELK_UI_INTF_RENDERER_H

#include <velk/array_view.h>
#include <velk/interface/intf_metadata.h>
#include <velk/interface/intf_object.h>

#include <velk-ui/interface/intf_element.h>

namespace velk_ui {

/**
 * @brief Interface for rendering UI elements.
 *
 * Manages GPU resources for visible elements. Elements are registered via
 * add_visual and unregistered via remove_visual. Each frame, the scene
 * calls update_visuals with the list of elements whose properties changed.
 */
class IRenderer : public velk::Interface<IRenderer>
{
public:
    /** @brief Opaque handle to a registered visual. Zero is invalid. */
    using VisualId = uint32_t;

    VELK_INTERFACE(
        (PROP, uint32_t, viewport_width, 0),  ///< Viewport width in pixels.
        (PROP, uint32_t, viewport_height, 0)  ///< Viewport height in pixels.
    )

    /** @brief Initializes GPU resources. Must be called with a valid GL context. */
    virtual bool init(int width, int height) = 0;

    /** @brief Uploads dirty data and draws all registered visuals. */
    virtual void render() = 0;

    /** @brief Releases all GPU resources. */
    virtual void shutdown() = 0;

    /** @brief Registers an element and allocates a GPU slot for it. */
    virtual VisualId add_visual(const IElement::Ptr& element) = 0;

    /** @brief Unregisters an element and frees its GPU slot. */
    virtual void remove_visual(VisualId id) = 0;

    /** @brief Re-reads state for each changed element and marks their GPU slots dirty. */
    virtual void update_visuals(velk::array_view<IElement*> changed) = 0;
};

} // namespace velk_ui

#endif // VELK_UI_INTF_RENDERER_H
