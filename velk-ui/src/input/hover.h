#ifndef VELK_UI_INPUT_HOVER_H
#define VELK_UI_INPUT_HOVER_H

#include <velk-ui/ext/input.h>
#include <velk-ui/interface/trait/intf_hover.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

class Hover : public ext::Input<Hover, IHover>
{
public:
    VELK_CLASS_UID(ClassId::Input::Hover, "Hover");

    void on_pointer_enter(const PointerEvent& event) override;
    void on_pointer_leave(const PointerEvent& event) override;
};

} // namespace velk::ui

#endif // VELK_UI_INPUT_HOVER_H
