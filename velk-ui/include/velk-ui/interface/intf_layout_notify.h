#ifndef VELK_UI_INTF_LAYOUT_NOTIFY_H
#define VELK_UI_INTF_LAYOUT_NOTIFY_H

#include <velk/interface/intf_metadata.h>

namespace velk::ui {

/**
 * @brief Optional interface for traits that affect layout.
 *
 * Layout traits and transform traits implement this to notify the owning
 * element when their properties change, triggering a layout re-solve.
 * The element subscribes to on_layout_changed and sets DirtyFlags::Layout.
 */
class ILayoutNotify : public Interface<ILayoutNotify,
    IInterface, VELK_UID("a6204170-312e-4b44-90ea-2ddc2978cdb1")>
{
public:
    VELK_INTERFACE(
        (EVT, on_layout_changed)
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_LAYOUT_NOTIFY_H
