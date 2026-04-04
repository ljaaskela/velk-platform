#ifndef VELK_UI_API_TRAIT_STACK_H
#define VELK_UI_API_TRAIT_STACK_H

#include <velk/api/state.h>

#include <velk-ui/api/trait.h>
#include <velk-ui/interface/trait/intf_stack.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around IStack.
 *
 * Provides null-safe access to stack layout constraint properties.
 *
 *   auto stack = constraint::create_stack();
 *   stack.set_axis(0);       // horizontal
 *   stack.set_spacing(10.f);
 */
class Stack : public Trait
{
public:
    /** @brief Default-constructed Stack wraps no object. */
    Stack() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement IStack. */
    explicit Stack(IObject::Ptr obj) : Trait(check_object<IStack>(obj)) {}

    /** @brief Wraps an existing IStack pointer. */
    explicit Stack(IStack::Ptr s) : Trait(as_object(s)) {}

    /** @brief Implicit conversion to IStack::Ptr. */
    operator IStack::Ptr() const { return as_ptr<IStack>(); }

    /** @brief Returns the stack axis: 0 = horizontal, 1 = vertical. */
    auto get_axis() const { return read_state_value<IStack>(&IStack::State::axis); }

    /** @brief Sets the stack axis: 0 = horizontal, 1 = vertical. */
    void set_axis(uint8_t v) { write_state_value<IStack>(&IStack::State::axis, v); }

    /** @brief Returns the spacing between children in pixels. */
    auto get_spacing() const { return read_state_value<IStack>(&IStack::State::spacing); }

    /** @brief Sets the spacing between children in pixels. */
    void set_spacing(float v) { write_state_value<IStack>(&IStack::State::spacing, v); }
};

namespace constraint {

/** @brief Creates a new Stack constraint. */
inline Stack create_stack()
{
    return Stack(instance().create<IStack>(ClassId::Constraint::Stack));
}

} // namespace constraint

} // namespace velk::ui

#endif // VELK_UI_API_TRAIT_STACK_H
