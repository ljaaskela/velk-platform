#ifndef VELK_UI_INTF_RENDERER_H
#define VELK_UI_INTF_RENDERER_H

#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/intf_surface.h>
#include <velk-ui/interface/intf_element.h>

namespace velk::ui {

/**
 * @brief Scene renderer / dispatcher.
 *
 * Walks scene trees, collects draw entries from visuals, batches them,
 * writes GPU buffers, and submits draw calls to the render backend.
 *
 * Views are defined by camera elements (elements with an ICamera trait).
 * Each view binds a camera to a surface. The camera's element provides
 * the scene (via get_scene()) and the view-projection matrix.
 *
 * Created via velk_ui::create_renderer(ctx).
 */
class IRenderer : public Interface<IRenderer>
{
public:
    /**
     * @brief Adds a view: renders the camera element's scene onto the surface.
     * @param camera_element Element with an ICamera trait attached.
     * @param surface        Render target surface.
     */
    virtual void add_view(const IElement::Ptr& camera_element,
                          const ISurface::Ptr& surface) = 0;

    /**
     * @brief Removes a previously added view.
     * @param camera_element The camera element used in add_view.
     * @param surface        The surface used in add_view.
     */
    virtual void remove_view(const IElement::Ptr& camera_element,
                             const ISurface::Ptr& surface) = 0;

    /** @brief Pulls state from all views, rebuilds batches, and draws. */
    virtual void render() = 0;

    /** @brief Releases all GPU resources. */
    virtual void shutdown() = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_RENDERER_H
