#ifndef VELK_UI_INTF_STACK_H
#define VELK_UI_INTF_STACK_H

#include <velk/interface/intf_metadata.h>

#include <velk-scene/types.h>

#include <cstdint>

namespace velk::ui {

/**
 * @brief Layout that arranges children one after another along an axis.
 *
 * Children with a fixed or auto size along the axis get that size; the rest
 * share the space left. Across the axis children are placed by `align`,
 * unless they set their own with an Align trait.
 */
class IStack : public Interface<IStack>
{
public:
    VELK_INTERFACE(
        (PROP, uint8_t, axis, 1),                      ///< Stack axis: 0 = horizontal (X), 1 = vertical (Y).
        (PROP, float, spacing, 0.f),                   ///< Gap between children in pixels.
        (PROP, ::velk::Alignment, align, ::velk::Alignment::Fill) ///< Children's placement across the axis.
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_STACK_H
