#ifndef VELK_UI_INTF_FIXED_SIZE_H
#define VELK_UI_INTF_FIXED_SIZE_H

#include <velk/interface/intf_metadata.h>

#include <velk-scene/types.h>

namespace velk::ui {

/**
 * @brief Sets an element's width, height and depth.
 *
 * Each axis is none (take the space offered), px, pct of the space offered,
 * or auto (fit the content: the visuals' content size and the children).
 */
class IFixedSize : public Interface<IFixedSize>
{
public:
    VELK_INTERFACE(
        (PROP, dim, width, dim::none()),  ///< Width. None = the space offered.
        (PROP, dim, height, dim::none()), ///< Height. None = the space offered.
        (PROP, dim, depth, dim::none())   ///< Depth (3D shapes only), px. None = 0.
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_FIXED_SIZE_H
