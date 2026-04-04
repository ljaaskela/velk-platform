#ifndef VELK_UI_API_INPUT_DISPATCHER_H
#define VELK_UI_API_INPUT_DISPATCHER_H

#include <velk/api/object.h>

#include <velk-ui/api/element.h>
#include <velk-ui/api/scene.h>
#include <velk-ui/interface/intf_input_dispatcher.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around IInputDispatcher.
 *
 * Provides null-safe access to the input dispatcher. Typically created
 * once per scene and fed platform events.
 *
 *   auto input = create_input_dispatcher(scene);
 *   input.pointer_event({.position = {x, y}, .action = PointerAction::Move});
 */
class InputDispatcher : public Object
{
public:
    InputDispatcher() = default;
    explicit InputDispatcher(IObject::Ptr obj) : Object(check_object<IInputDispatcher>(obj)) {}
    explicit InputDispatcher(IInputDispatcher::Ptr d) : Object(as_object(d)) {}

    operator IInputDispatcher::Ptr() const { return as_ptr<IInputDispatcher>(); }

    /** @brief Feed a pointer event from the platform layer. */
    void pointer_event(const PointerEvent& event)
    {
        with<IInputDispatcher>([&](auto& d) { d.pointer_event(event); });
    }

    /** @brief Feed a scroll event from the platform layer. */
    void scroll_event(const ScrollEvent& event)
    {
        with<IInputDispatcher>([&](auto& d) { d.scroll_event(event); });
    }

    /** @brief Feed a key event from the platform layer. */
    void key_event(const KeyEvent& event)
    {
        with<IInputDispatcher>([&](auto& d) { d.key_event(event); });
    }

    /** @brief Returns the element currently under the pointer, or empty. */
    Element get_hovered() const
    {
        return Element(with<IInputDispatcher>([](auto& d) { return d.get_hovered(); }));
    }

    /** @brief Returns the element that captured pointer-down, or empty. */
    Element get_pressed() const
    {
        return Element(with<IInputDispatcher>([](auto& d) { return d.get_pressed(); }));
    }

    /** @brief Returns the element with keyboard focus, or empty. */
    Element get_focused() const
    {
        return Element(with<IInputDispatcher>([](auto& d) { return d.get_focused(); }));
    }

    /** @brief Sets keyboard focus to an element (empty to clear). */
    void set_focus(const IElement::Ptr& element)
    {
        with<IInputDispatcher>([&](auto& d) { d.set_focus(element); });
    }
};

/**
 * @brief Creates an input dispatcher bound to a scene.
 * @param scene The scene to perform hit testing against.
 */
inline InputDispatcher create_input_dispatcher(Scene& scene)
{
    auto dispatcher = instance().create<IInputDispatcher>(ClassId::Input::Dispatcher);
    if (dispatcher) {
        dispatcher->set_scene(scene);
    }
    return InputDispatcher(std::move(dispatcher));
}

} // namespace velk::ui

#endif // VELK_UI_API_INPUT_DISPATCHER_H
