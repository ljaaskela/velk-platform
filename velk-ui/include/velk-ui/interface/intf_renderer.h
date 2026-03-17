#ifndef VELK_UI_INTF_RENDERER_H
#define VELK_UI_INTF_RENDERER_H

#include <velk/interface/intf_metadata.h>
#include <velk/interface/intf_object.h>

namespace velk_ui {

class IRenderer : public velk::Interface<IRenderer>
{
public:
    using VisualId = uint32_t;

    VELK_INTERFACE(
        (PROP, uint32_t, viewport_width, 0),
        (PROP, uint32_t, viewport_height, 0)
    )

    virtual bool init(int width, int height) = 0;
    virtual void render() = 0;
    virtual void shutdown() = 0;

    /// Register an element. Renderer subscribes to on_changed and allocates a GPU slot.
    virtual VisualId add_visual(velk::IObject::Ptr element) = 0;

    /// Unregister. Frees GPU slot, unsubscribes from changes.
    virtual void remove_visual(VisualId id) = 0;
};

} // namespace velk_ui

#endif // VELK_UI_INTF_RENDERER_H
