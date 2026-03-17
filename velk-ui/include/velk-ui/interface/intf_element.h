#ifndef VELK_UI_INTF_ELEMENT_H
#define VELK_UI_INTF_ELEMENT_H

#include <velk/interface/intf_metadata.h>

namespace velk_ui {

class IElement : public velk::Interface<IElement>
{
public:
    VELK_INTERFACE(
        (PROP, float, x, 0.f),
        (PROP, float, y, 0.f),
        (PROP, float, width, 100.f),
        (PROP, float, height, 100.f),
        (PROP, float, r, 1.f),
        (PROP, float, g, 0.f),
        (PROP, float, b, 0.f),
        (PROP, float, a, 1.f)
    )
};

} // namespace velk_ui

#endif // VELK_UI_INTF_ELEMENT_H
