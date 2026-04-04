#ifndef VELK_UI_EXT_INPUT_H
#define VELK_UI_EXT_INPUT_H

#include <velk/ext/object.h>

#include <velk-ui/interface/intf_input_trait.h>

namespace velk::ui::ext {

/**
 * @brief CRTP base for IInputTrait implementations.
 *
 * Provides no-op defaults for all input methods. Subclasses override
 * only the events they care about.
 *
 * @tparam T     The concrete input trait class (CRTP parameter).
 * @tparam Extra Additional interfaces the trait implements.
 */
template <class T, class... Extra>
class Input : public ::velk::ext::Object<T, IInputTrait, Extra...>
{
public:
    TraitPhase get_phase() const override { return TraitPhase::Input; }
    InputResult on_intercept(PointerEvent&) override { return InputResult::Ignored; }
    InputResult on_pointer_event(PointerEvent&) override { return InputResult::Ignored; }
    void on_pointer_enter(const PointerEvent&) override {}
    void on_pointer_leave(const PointerEvent&) override {}
    InputResult on_scroll_event(ScrollEvent&) override { return InputResult::Ignored; }
    InputResult on_key_event(KeyEvent&) override { return InputResult::Ignored; }
};

} // namespace velk::ui::ext

#endif // VELK_UI_EXT_INPUT_H
