#ifndef VELK_UI_INTF_INSETS_H
#define VELK_UI_INTF_INSETS_H

#include <velk/interface/intf_metadata.h>

#include <velk-scene/types.h>

namespace velk::ui {

/**
 * @brief Space between an element's edges and its children.
 *
 * Values are px, or pct of the element's size on that axis. Visuals on the
 * element itself still cover the whole element.
 */
class IPadding : public Interface<IPadding>
{
public:
    VELK_INTERFACE(
        (PROP, dim, left, dim::zero()),
        (PROP, dim, top, dim::zero()),
        (PROP, dim, right, dim::zero()),
        (PROP, dim, bottom, dim::zero())
    )
};

/**
 * @brief Space around an element, inside the space its parent gives it.
 *
 * Values are px, or pct of the space offered on that axis.
 */
class IMargin : public Interface<IMargin>
{
public:
    VELK_INTERFACE(
        (PROP, dim, left, dim::zero()),
        (PROP, dim, top, dim::zero()),
        (PROP, dim, right, dim::zero()),
        (PROP, dim, bottom, dim::zero())
    )
};

/**
 * @brief An element's own placement in the space its parent gives it,
 *        overriding the parent's alignment. Unset keeps the parent's.
 */
class IAlign : public Interface<IAlign>
{
public:
    VELK_INTERFACE(
        (PROP, ::velk::Alignment, horizontal, ::velk::Alignment::Unset),
        (PROP, ::velk::Alignment, vertical, ::velk::Alignment::Unset)
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_INSETS_H
