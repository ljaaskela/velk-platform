#ifndef VELK_UI_INTF_STACK_H
#define VELK_UI_INTF_STACK_H

#include <velk/interface/intf_metadata.h>

#include <cstdint>

namespace velk::ui {

/**
 * @brief Layout constraint that arranges children sequentially along an axis.
 *
 * Children are laid out one after another with optional spacing. Each child
 * fills the cross-axis by default. The stack itself fills its parent.
 */
class IStack : public Interface<IStack>
{
public:
    VELK_INTERFACE(
        (PROP, uint8_t, axis, 1), ///< Stack axis: 0 = horizontal (X), 1 = vertical (Y).
        (PROP, float, spacing, 0.f)  ///< Gap between children in pixels.
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_STACK_H
