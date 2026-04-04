#ifndef VELK_UI_INPUT_CLICK_H
#define VELK_UI_INPUT_CLICK_H

#include <velk-ui/ext/input.h>
#include <velk-ui/interface/trait/intf_click.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

class Click : public ext::Input<Click, IClick>
{
public:
    VELK_CLASS_UID(ClassId::Input::Click, "Click");

    InputResult on_pointer_event(PointerEvent& event) override;
    void on_pointer_leave(const PointerEvent& event) override;

private:
    void set_pressed(bool v);
};

} // namespace velk::ui

#endif // VELK_UI_INPUT_CLICK_H
