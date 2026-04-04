# Input

velk-ui's input system dispatches platform events (mouse, keyboard, scroll) to elements through an opt-in trait-based model. Elements only receive input if they have an `IInputTrait` attached.

## InputDispatcher

`InputDispatcher` is the scene-level coordinator. It is created with a reference to a scene and exposes three entry points for feeding platform events:

```cpp
auto input = velk::ui::create_input_dispatcher(scene);

input.pointer_event(ev);  // PointerEvent (down, up, move, cancel)
input.scroll_event(ev);   // ScrollEvent
input.key_event(ev);      // KeyEvent
```

The application is responsible for translating platform-specific events (GLFW, SDL, Win32, etc.) into the POD event structs and calling the appropriate method. Events should be fed before `velk::instance().update()` so that any property changes made by handlers are picked up by the layout solver in the same frame.

### State queries

The dispatcher tracks pointer and focus state:

```cpp
input.get_hovered();   // element under the pointer, or empty
input.get_pressed();   // element where pointer-down occurred, or empty
input.get_focused();   // element with keyboard focus, or empty
input.set_focus(elem); // set keyboard focus (empty to clear)
```

## Dispatch model

### Pointer events

Pointer events follow a two-pass dispatch:

1. **Hit test**: the dispatcher walks the scene's visual list in reverse z-order (topmost first) and finds the deepest element with an `IInputTrait` whose world-space bounds contain the pointer.

2. **Intercept pass** (top-down, root to target): each ancestor with an `IInputTrait` gets `on_intercept()`. If any returns `Consumed` or `Captured`, that ancestor steals the event and becomes the new target. This is how a scroll container can intercept events from its children.

3. **Bubble pass** (bottom-up, target to root): the target element's trait gets `on_pointer_event()` first, then each ancestor. `Consumed` or `Captured` stops bubbling.

### Scroll events

Scroll events hit-test and then bubble from target to root (no intercept pass).

### Key events

Key events dispatch to the focused element's `IInputTrait` and bubble upward through ancestors.

### Hover tracking

The dispatcher compares the hit element on each move event with the previous one. When it changes, `on_pointer_leave` fires on the old element and `on_pointer_enter` on the new one. These do not bubble.

### Press capture

When a pointer-down handler returns `Captured`, all subsequent move and up events route directly to that element until release, even if the pointer moves outside its bounds. An ancestor can still steal via interception.

## InputResult

Every input handler returns an `InputResult`:

| Value | Effect |
|-------|--------|
| `Ignored` | Not handled. Continue bubbling. |
| `Consumed` | Handled. Stop bubbling. |
| `Captured` | Consumed, and capture all future pointer events until release. |

## IInputTrait

Input is one of five [trait phases](traits.md#trait-phases). The base interface for elements that receive input is `IInputTrait`. Attach it to opt an element into hit testing and event dispatch.

```cpp
class IInputTrait : public velk::Interface<IInputTrait, ITrait> {
    virtual InputResult on_intercept(PointerEvent& event) = 0;
    virtual InputResult on_pointer_event(PointerEvent& event) = 0;
    virtual void on_pointer_enter(const PointerEvent& event) = 0;
    virtual void on_pointer_leave(const PointerEvent& event) = 0;
    virtual InputResult on_scroll_event(ScrollEvent& event) = 0;
    virtual InputResult on_key_event(KeyEvent& event) = 0;
};
```

The CRTP base `ext::Input<T>` provides no-op defaults for all methods. Override only what you need.

## Built-in input traits

See [Traits](traits.md#input-traits) for the full list with usage examples. Summary:

| Trait | Events | Properties |
|-------|--------|------------|
| Click | `on_click` | `pressed` (read-only) |
| Hover | `on_hover_changed` | `hovered` (read-only) |
| Drag | `on_drag_start`, `on_drag_move`, `on_drag_end` | `dragging` (read-only) |

## Event types

All event structs are PODs defined in `input_types.h`. Position fields are in scene-space; the dispatcher fills `local_position` with the element-local coordinate before delivery.

| Struct | Fields |
|--------|--------|
| `PointerEvent` | `position`, `local_position`, `button`, `action`, `modifiers`, `pointer_id` |
| `ScrollEvent` | `position`, `local_position`, `delta`, `unit`, `modifiers` |
| `KeyEvent` | `key`, `scancode`, `action`, `modifiers`, `codepoint` |

## Custom input traits

Subclass `ext::Input<T>` and override the methods you need:

```cpp
class IMyInput : public velk::Interface<IMyInput> {
public:
    VELK_INTERFACE(
        (EVT, on_double_click)
    )
};

class MyInput : public velk::ui::ext::Input<MyInput, IMyInput> {
public:
    VELK_CLASS_UID(ClassId::MyInput, "MyInput");

    InputResult on_pointer_event(PointerEvent& event) override {
        if (event.action == PointerAction::Down) {
            // detect double-click timing...
            velk::invoke_event(get_interface(velk::IInterface::UID), "on_double_click");
            return InputResult::Consumed;
        }
        return InputResult::Ignored;
    }
};
```

## Debug logging

Build with `-DVELK_INPUT_DEBUG=ON` to enable verbose input dispatch logging (`INPUT_LOG` macro in the dispatcher and built-in traits).
