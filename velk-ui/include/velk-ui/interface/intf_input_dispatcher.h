#ifndef VELK_UI_INTF_INPUT_DISPATCHER_H
#define VELK_UI_INTF_INPUT_DISPATCHER_H

#include <velk/interface/intf_metadata.h>

#include <velk-ui/input_types.h>
#include <velk-ui/interface/intf_element.h>

namespace velk::ui {

/**
 * @brief Scene-level input coordinator.
 *
 * Receives platform input events, performs hit testing against the scene's
 * element hierarchy, and dispatches events to IInputTrait handlers using
 * the intercept + bubble model.
 *
 * Tracks hover, press-capture, and keyboard focus state.
 */
class IInputDispatcher : public Interface<IInputDispatcher>
{
public:
    VELK_INTERFACE(
        (EVT, on_focus_changed)
    )

    /** @brief Feed a pointer event from the platform layer. */
    virtual void pointer_event(const PointerEvent& event) = 0;

    /** @brief Feed a scroll event from the platform layer. */
    virtual void scroll_event(const ScrollEvent& event) = 0;

    /** @brief Feed a key event from the platform layer. */
    virtual void key_event(const KeyEvent& event) = 0;

    /** @brief Returns the element currently under the pointer, or empty. */
    virtual IElement::Ptr get_hovered() const = 0;

    /** @brief Returns the element that captured pointer-down, or empty. */
    virtual IElement::Ptr get_pressed() const = 0;

    /** @brief Returns the element with keyboard focus, or empty. */
    virtual IElement::Ptr get_focused() const = 0;

    /** @brief Sets keyboard focus to an element (nullptr to clear). */
    virtual void set_focus(const IElement::Ptr& element) = 0;

    /** @brief Binds this dispatcher to a scene for hit testing. */
    virtual void set_scene(const shared_ptr<IScene>& scene) = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_INPUT_DISPATCHER_H
