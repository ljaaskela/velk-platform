#ifndef VELK_UI_INTF_FIXED_SIZE_H
#define VELK_UI_INTF_FIXED_SIZE_H

#include <velk/interface/intf_metadata.h>

#include <velk-ui/types.h>

namespace velk::ui {

/**
 * @brief Constraint that clamps an element to a specific width and/or height.
 *
 * Either axis can be dim::none() to leave it unconstrained. Values are
 * resolved against the parent's available space (px = absolute, pct = relative).
 */
class IFixedSize : public Interface<IFixedSize>
{
public:
    VELK_INTERFACE(
        (PROP, dim, width, dim::none()),  ///< Fixed width. None = use parent's width.
        (PROP, dim, height, dim::none()), ///< Fixed height. None = use parent's height.
        (PROP, dim, depth, dim::none())   ///< Fixed depth (3D shapes only). None = leaves element depth at 0.
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_FIXED_SIZE_H
