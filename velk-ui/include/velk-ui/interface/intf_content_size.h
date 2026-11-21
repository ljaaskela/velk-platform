#ifndef VELK_UI_INTF_CONTENT_SIZE_H
#define VELK_UI_INTF_CONTENT_SIZE_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_interface.h>

namespace velk::ui {

/**
 * @brief A visual with a natural size, such as text or an image.
 *
 * An element sized "auto" fits the largest content size of its visuals and
 * the desired sizes of its children.
 */
class IContentSize : public Interface<IContentSize>
{
public:
    /** @brief Returns the natural size of the content when offered @p available space. */
    virtual size content_size(const size& available) = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_CONTENT_SIZE_H
