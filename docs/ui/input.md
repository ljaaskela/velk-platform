# Input

velk-ui's input system dispatches platform events (mouse, keyboard, scroll) to elements through an opt-in trait-based model. Elements only receive input if they have an `IInputTrait` attached.

## Contents
- [InputDispatcher](#inputdispatcher)
  - [Where it comes from](#where-it-comes-from)
  - [State queries](#state-queries)
- [Dispatch model](#dispatch-model)
  - [Pointer events](#pointer-events)
  - [Scroll events](#scroll-events)
  - [Key events](#key-events)
  - [Hover tracking](#hover-tracking)
  - [Press capture](#press-capture)
- [InputResult](#inputresult)
- [IInputTrait](#iinputtrait)
- [Built-in input traits](#built-in-input-traits)
- [Dispatcher events](#dispatcher-events)
- [Event types](#event-types)
- [Custom input traits](#custom-input-traits)
- [Debug logging](#debug-logging)
- [Classes](#classes)


## InputDispatcher

`InputDispatcher` is the scene-level coordinator. It receives platform input events, hit-tests against the scene, and dispatches to elements with input traits.

### Where it comes from

When using the runtime, every `Window` already owns an `IInputDispatcher`. The platform plugin (GLFW on desktop) auto-feeds platform events into it. When you call `app.add_view(window, camera)`, the runtime also binds the window's dispatcher to the camera's scene, so hit testing works against that scene.

```cpp
auto window = app.create_window({.width = 1280, .height = 720});
auto scene = velk::create_scene("app://scenes/main.json");
app.add_view(window, scene.child_at(scene.root(), 0));   // also binds dispatcher to scene
```

You typically don't construct an `InputDispatcher` directly — the runtime does it. But if you need raw access (framework-driven mode where you're feeding events yourself, or testing), you can:

```cpp
auto* dispatcher = window.input();           // IInputDispatcher* from Window
dispatcher->pointer_event(ev);               // PointerEvent (down, up, move, cancel)
dispatcher->scroll_event(ev);                // ScrollEvent
dispatcher->key_event(ev);                   // KeyEvent
```

In framework-driven mode (Android, embedded), the host translates platform events into the POD event structs and feeds them to the window dispatcher. Events should be fed before `app.update()` so that any property changes made by handlers are picked up by the layout solver in the same frame.

### State queries

The dispatcher tracks pointer and focus state:

```cpp
dispatcher->get_hovered();   // element under the pointer, or empty
dispatcher->get_pressed();   // element where pointer-down occurred, or empty
dispatcher->get_focused();   // element with keyboard focus, or empty
dispatcher->set_focus(elem); // set keyboard focus (empty to clear)
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

| Trait | Event | Argument | Property |
|-------|-------|----------|----------|
| Click | `on_click` | (none) | `pressed` (read-only) |
| Hover | `on_hover_changed` | `bool hovered` | `hovered` (read-only) |
| Drag | `on_drag_start`, `on_drag_move`, `on_drag_end` | `DragEvent` | `dragging` (read-only) |

`DragEvent` carries the gesture's spatial state (start position, current position, delta since last move, total delta since start, button, modifiers). Adding new fields later doesn't change the signature.

```cpp
auto drag = velk::ui::trait::input::create_drag();
elem.add_trait(drag);

velk::ScopedHandler move_sub(drag.on_drag_move(),
    [&elem](const velk::ui::DragEvent& d) {
        elem.set_position(elem.get_position() + vec3{d.delta.x, d.delta.y, 0});
    });
```

## Dispatcher events

The `IInputDispatcher` itself exposes typed events that fire BEFORE hit testing and trait dispatch. Use these for global handlers (debug overlays, global hotkeys, orbit cameras that don't go through the trait system):

| Event | Argument |
|-------|----------|
| `on_pointer_event` | `PointerEvent` |
| `on_scroll_event` | `ScrollEvent` |
| `on_focus_changed` | `bool focused` |

```cpp
velk::ScopedHandler raw_pointer(window.input()->on_pointer_event(),
    [](const velk::ui::PointerEvent& e) {
        if (e.action == velk::ui::PointerAction::Down &&
            e.button == velk::ui::PointerButton::Right) {
            // start an orbit drag, irrespective of any element under the pointer
        }
    });
```

These fire on every event, regardless of whether anything in the scene consumed them.

## Event types

All event structs are PODs defined in `input_types.h`. Position fields are in scene-space; the dispatcher fills `local_position` with the element-local coordinate before delivery.

| Struct | Fields |
|--------|--------|
| `PointerEvent` | `position`, `local_position`, `button`, `action`, `modifiers`, `pointer_id` |
| `ScrollEvent` | `position`, `local_position`, `delta`, `unit`, `modifiers` |
| `KeyEvent` | `key`, `scancode`, `action`, `modifiers`, `codepoint` |

## Custom input traits

Subclass `ext::Input<T>` and override the methods you need. Declare typed event signatures in `VELK_INTERFACE` so the metadata documents what handlers receive:

```cpp
class IMyInput : public velk::Interface<IMyInput> {
public:
    VELK_INTERFACE(
        (EVT, on_double_click, (PointerEvent, event))
    )
};

class MyInput : public velk::ui::ext::Input<MyInput, IMyInput> {
public:
    VELK_CLASS_UID(ClassId::MyInput, "MyInput");

    InputResult on_pointer_event(PointerEvent& event) override {
        if (event.action == PointerAction::Down) {
            // detect double-click timing...
            velk::invoke_event(get_interface(velk::IInterface::UID), "on_double_click", event);
            return InputResult::Consumed;
        }
        return InputResult::Ignored;
    }
};
```

The `invoke_event(this_iface, "name", arg)` form auto-wraps `arg` in `Any<T>` and dispatches to handlers. Handlers can be registered with typed lambdas:

```cpp
velk::ScopedHandler dc(my_input.on_double_click(),
    [](const velk::ui::PointerEvent& e) { /* handle */ });
```

For event types to work with `Any<T>`, the type must be registered. velk-ui registers `PointerEvent`, `ScrollEvent`, `KeyEvent`, and `DragEvent` in its plugin's `initialize()`. If your custom event uses a custom payload struct, register it the same way:

```cpp
rv &= register_type<::velk::ext::AnyValue<MyEventPayload>>(velk);
```

## Debug logging

Build with `-DVELK_INPUT_DEBUG=ON` to enable verbose input dispatch logging (`INPUT_LOG` macro in the dispatcher and built-in traits).

## Classes

ClassIds for the input system. The dispatcher is owned by the window — you don't usually construct it directly. The trait classes attach to elements via the standard `add_trait()` API.

| ClassId | Implements | Description |
|---|---|---|
| `velk::ui::ClassId::Input::Dispatcher` | `IInputDispatcher` | Per-window event coordinator. Receives raw events, hit-tests against the bound scene, dispatches to elements with input traits. Exposes `on_pointer_event`, `on_scroll_event`, `on_focus_changed` for global subscribers. |
| `velk::ui::ClassId::Input::Click` | `IClick`, `IInputTrait` | Click gesture detection. Property: `pressed` (read-only). Event: `on_click`. |
| `velk::ui::ClassId::Input::Hover` | `IHover`, `IInputTrait` | Hover state tracking. Property: `hovered` (read-only). Event: `on_hover_changed(bool)`. |
| `velk::ui::ClassId::Input::Drag` | `IDrag`, `IInputTrait` | Drag gesture tracking. Property: `dragging` (read-only). Events: `on_drag_start(DragEvent)`, `on_drag_move(DragEvent)`, `on_drag_end(DragEvent)`. |
