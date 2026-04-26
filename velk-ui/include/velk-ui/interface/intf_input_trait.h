#ifndef VELK_UI_INTF_INPUT_TRAIT_H
#define VELK_UI_INTF_INPUT_TRAIT_H

#include <velk-ui/input_types.h>
#include <velk-scene/interface/intf_trait.h>

namespace velk::ui {

/**
 * @brief Trait for elements that receive input events.
 *
 * Attach to an element to opt it into hit testing and event dispatch.
 * Only elements with an IInputTrait participate in the input pipeline.
 *
 * Pointer dispatch follows a two-pass model:
 *   1. Intercept (top-down): ancestors can steal the event via on_intercept().
 *   2. Bubble (bottom-up): the target element handles first, then ancestors.
 *
 * Return InputResult::Consumed to stop bubbling, or InputResult::Captured
 * to also capture all future pointer events until release.
 */
class IInputTrait : public Interface<IInputTrait, ITrait>
{
public:
    /**
     * @brief Intercept pass (top-down, root to target).
     *
     * Called on each ancestor before the target receives the event.
     * Return Consumed or Captured to steal the event from the child.
     */
    virtual InputResult on_intercept(PointerEvent& event) = 0;

    /**
     * @brief Handle a pointer event (bubble pass, target to root).
     *
     * Return Consumed or Captured to stop bubbling.
     */
    virtual InputResult on_pointer_event(PointerEvent& event) = 0;

    /** @brief Pointer entered this element's bounds. Does not bubble. */
    virtual void on_pointer_enter(const PointerEvent& event) = 0;

    /** @brief Pointer left this element's bounds. Does not bubble. */
    virtual void on_pointer_leave(const PointerEvent& event) = 0;

    /**
     * @brief Handle a scroll event (bubble pass, target to root).
     *
     * Return Consumed or Captured to stop bubbling.
     */
    virtual InputResult on_scroll_event(ScrollEvent& event) = 0;

    /**
     * @brief Handle a key event (bubble pass from focused element).
     *
     * Return Consumed to stop bubbling.
     */
    virtual InputResult on_key_event(KeyEvent& event) = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_INPUT_TRAIT_H
