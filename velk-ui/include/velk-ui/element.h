#ifndef VELK_UI_ELEMENT_H
#define VELK_UI_ELEMENT_H

#include <velk-ui/interface/intf_element.h>
#include <velk/ext/object.h>

namespace velk_ui {

class Element : public velk::ext::Object<Element, IElement>
{
public:
    VELK_CLASS_UID("136ea22f-189a-4750-ad12-d4d15bd6b7cf", "Element");
};

} // namespace velk_ui

#endif // VELK_UI_ELEMENT_H
