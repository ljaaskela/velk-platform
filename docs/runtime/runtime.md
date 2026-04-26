# Runtime

`velk-runtime` is the user-facing entry point. It owns a single `Application` that loads plugins, sets up the render context, manages platform windows, and drives the per-frame loop.

## Contents
- [Usage](#usage)
- [Two execution modes](#two-execution-modes)
  - [App-driven (desktop)](#app-driven-desktop)
  - [Framework-driven (Android, embedded host)](#framework-driven-android-embedded-host)
- [Application](#application)
  - [`ApplicationConfig`](#applicationconfig)
- [Window](#window)
  - [`WindowConfig`](#windowconfig)
  - [Update rate](#update-rate)
  - [Window lifecycle](#window-lifecycle)
  - [Window events](#window-events)
- [Frame loop](#frame-loop)
  - [`poll()`](#poll)
  - [`update()`](#update)
  - [`prepare()` and `submit(Frame)`](#prepare-and-submitframe)
  - [`present()`](#present)
- [Adding views](#adding-views)
- [Performance overlay](#performance-overlay)
- [Render context and renderer access](#render-context-and-renderer-access)
- [Input](#input)
- [Plugin loading](#plugin-loading)
- [Threading](#threading)
- [Classes](#classes)
- [See also](#see-also)

## Usage

For most apps, this is the only setup code you write. The runtime hides GLFW, Vulkan, the renderer, and the plugin loading boilerplate behind one class.

```cpp
#include <velk-runtime/api/application.h>
#include <velk-scene/api/scene.h>

int main()
{
    auto app = velk::create_app({});
    auto window = app.create_window({.width = 1280, .height = 720, .title = "demo"});

    auto scene = velk::create_scene("app://scenes/main.json");
    if (auto camera = scene.find_first<velk::ICamera>()) {
        app.add_view(window, camera);
    }

    while (app.poll()) {
        app.update();
        app.present();
    }
}
```

That's the entire desktop entry point. Everything else (Vulkan instance, swapchain, scene-to-renderer wiring, input dispatcher binding, scene geometry tracking on resize) is handled inside `Application`.

## Two execution modes

The runtime supports two distinct modes for who owns the platform window and the event loop:

### App-driven (desktop)

velk creates the native window via the GLFW platform plugin and runs its own poll/update/present loop. This is what `create_window()` does. The user controls the loop but doesn't touch GLFW or Vulkan directly.

```cpp
auto app = velk::create_app({});
auto window = app.create_window({.width = 1280, .height = 720});

while (app.poll()) {            // pumps GLFW events
    app.update();               // velk::instance().update()
    app.present();              // prepare + submit
}
```

### Framework-driven (Android, embedded host)

The host framework owns the native window and the event loop. velk wraps an externally provided platform surface (Android `ANativeWindow*`, embedded `HWND`, etc.) and the host calls into `update()` / `prepare()` / `submit()` from its own callbacks.

```cpp
// On a framework callback when the surface becomes available:
auto window = app.wrap_native_surface(native_handle);

// On the framework's frame callback (typically split between threads):
app.update();                   // main thread
auto frame = app.prepare();     // main thread
// post frame to render thread, which does:
app.submit(frame);              // render thread
```

In framework-driven mode, `poll()` is not used. The framework delivers input events; user code feeds them to `window.input()->pointer_event(...)` etc. Window resizes are notified by writing the new size to the window's state, which fires `on_resize`.

## Application

`velk::Application` is a header-only wrapper around the `IApplication` interface, in the same style as `Scene`, `Element`, and `RenderContext`. Construct it with `velk::create_app()`:

```cpp
velk::ApplicationConfig config;
config.backend = velk::RenderBackendType::Default;  // Vulkan on desktop
auto app = velk::create_app(config);
if (!app) {
    return 1;  // plugin loading or render context init failed
}
```

`create_app()` does several things in order:
1. Loads `velk_runtime.dll` (registers the Application class).
2. Calls `IApplication::init(config)`, which:
   - Loads all standard plugins (`velk_ui`, `velk_render`, `velk_vk`, `velk_text`, `velk_image`, `velk_importer`).
   - Loads the platform plugin (`velk_runtime_glfw` on desktop, `velk_runtime_android` on Android — selected via `#ifdef`).
   - Resolves the `IWindowProvider` from the platform plugin.

The render context and the default renderer are NOT created at this point. Both are created lazily on the first `create_window()` or `wrap_native_surface()` call, because Vulkan instance initialization needs to know about the first surface to enumerate the right device extensions.

### `ApplicationConfig`

| Field | Default | Description |
|---|---|---|
| `backend` | `RenderBackendType::Default` | GPU backend (Default = platform best). |
| `app_root` | empty | Override path for the `app://` resource scheme. |
| `assets_root` | empty | Override path for the `assets://` resource scheme. |

## Window

A `velk::Window` represents one render target with input. Create with `app.create_window(WindowConfig)` or wrap an existing platform surface with `app.wrap_native_surface(handle)`.

### `WindowConfig`

| Field | Default | Description |
|---|---|---|
| `width` / `height` | 1280×720 | Initial window size in pixels. |
| `title` | `"velk"` | Title bar text. |
| `resizable` | `true` | Whether the user can drag-resize the window. |
| `update_rate` | `UpdateRate::VSync` | Pacing mode (see below). |
| `target_fps` | 60 | Used when `update_rate == Targeted`. |

### Update rate

`UpdateRate` controls how often a window's surface is presented:

- **`VSync`** — Cap to display refresh. Vulkan FIFO present mode. No tearing. Default.
- **`Unlimited`** — Render as fast as possible. Vulkan IMMEDIATE/MAILBOX. May tear, but useful for benchmarks.
- **`Targeted`** — Software-paced to `target_fps`. The runtime measures each frame and sleeps in `submit()` to hit the target. Useful for fixed-rate animation rendering or conserving battery.

The setting is per-window — different windows in the same app can have different rates. For Targeted, the runtime tracks the smallest interval across all Targeted windows and sleeps to match.

### Window lifecycle

A managed (GLFW) window lives until closed by the user; `app.poll()` returns `false` once all managed windows have been requested to close. The `Window` value is a wrapper — the underlying `IWindow` is owned by the Application and stays alive until shutdown.

A wrapped (framework-driven) window has no GLFW backing. `should_close()` always returns `false`; the host framework controls the lifecycle and is expected to fire `on_surface_destroyed` when tearing down.

### Window events

`IWindow` exposes typed events that user code can subscribe to via `ScopedHandler`:

| Event | Argument | Fired by |
|---|---|---|
| `on_resize` | `velk::size` | Any window dimension change. |
| `on_surface_created` | (none) | Framework attached a new platform surface (framework-driven only). |
| `on_surface_changed` | `velk::size` | Existing surface was resized (framework-driven only). |
| `on_surface_destroyed` | (none) | Platform surface is being destroyed. |

```cpp
velk::ScopedHandler resize_sub(window.on_resize(),
    [](const velk::size& s) {
        VELK_LOG(I, "resized to %.0fx%.0f", s.width, s.height);
    });
```

## Frame loop

The frame loop is split into four phases that map to four `Application` methods:

### `poll()`
Pumps platform events. Returns `false` when all managed windows have been closed (the loop should exit). Not used in framework-driven mode — the host delivers events directly to window input dispatchers.

### `update()`
Calls `velk::instance().update()`, which advances the scene state: layout solving, deferred property updates, animations, plugin update hooks. After `update()` returns, scene element world transforms are valid for the next frame's rendering.

### `prepare()` and `submit(Frame)`
Two-phase frame submission for threaded rendering:

- `prepare()` walks all registered views, builds draw commands, and writes them into a frame slot. Returns a `Frame` handle. Safe on the main / UI thread.
- `submit(Frame)` submits the prepared frame to the GPU and presents it. Safe on any thread; typically called on a render thread separate from the main thread.

The split exists because the renderer's prepare phase is CPU-heavy (scene walking, batch building, GPU buffer writes) and benefits from running on a thread separate from the GPU submission. See [render/rendering.md](../render/rendering.md) for the internal details.

### `present()`
Convenience wrapper that calls `prepare()` then `submit(prepare())` on the same thread. Use this in single-threaded loops on desktop:

```cpp
while (app.poll()) {
    app.update();
    app.present();   // = submit(prepare())
}
```

For threaded use, call `prepare()` and `submit()` separately and route the `Frame` between threads.

## Adding views

A `view` is the (camera, surface, viewport) triple the renderer uses to draw a scene onto a window. Bind one with:

```cpp
app.add_view(window, camera_element);                       // full-window
app.add_view(window, camera_element, {0, 0, 0.5f, 1.0f});   // left half
app.add_view(window, camera_3d, {0.5f, 0, 0.5f, 1.0f});     // right half
```

The `camera_element` is any element with an `ICamera` trait attached. Multiple views on the same window let you composite multiple cameras (e.g. ortho UI overlay over a perspective 3D scene).

Behind the scenes, `add_view()`:
1. Calls `renderer->add_view(camera, window->surface(), viewport)`.
2. Reads the scene from `camera->get_scene()` and binds the window's input dispatcher to it.
3. Sets the scene's geometry to the window size and listens for `on_resize` to keep them synchronized.

So adding a view is enough to also wire up input routing and resize handling for that scene — you don't have to call `dispatcher.set_scene(...)` or `scene.set_geometry(...)` yourself.

## Performance overlay

The Application can render a built-in FPS / CPU / frame time overlay on any window:

```cpp
app.set_performance_overlay(window);                              // enable with defaults
app.set_performance_overlay(window, {.font_size = 18.f});         // custom
app.set_performance_overlay(window, {.enabled = false});          // disable
```

The overlay is implemented as a private scene with its own ortho camera and text element, registered as an extra view on the window's surface. It does NOT touch the user's scene. Frame timings are measured around `prepare()` / `submit()` in the runtime layer, so they reflect the cost of one full frame (scene update is excluded — it's measured separately by velk's update tick).

`PerformanceOverlayConfig`:

| Field | Default | Description |
|---|---|---|
| `enabled` | `true` | Set to `false` to remove the overlay. |
| `font_size` | 14 | Text size in pixels. |
| `text_color` | yellow | RGBA color. |

## Render context and renderer access

For most apps you don't need to touch the render context or renderer directly — the runtime handles them. If you need to (e.g. to compile a custom shader material), they're exposed:

```cpp
auto ctx = app.render_context();
auto sm = velk::create_shader_material(*ctx, frag_src, vert_src);

auto renderer = app.renderer();
renderer->set_max_frames_in_flight(2);
```

## Input

Each window owns an `IInputDispatcher`. For managed windows, the platform plugin auto-feeds GLFW pointer / scroll / key events. For wrapped surfaces, the user feeds them by hand:

```cpp
// Framework-driven: forward platform events to the window's dispatcher
auto* dispatcher = window.input();
dispatcher->pointer_event(my_pointer_event);
dispatcher->scroll_event(my_scroll_event);
dispatcher->key_event(my_key_event);
```

Subscribers (orbit cameras, custom global hotkeys, etc.) listen on the dispatcher's typed events:

```cpp
velk::ScopedHandler pointer_sub(window.input()->on_pointer_event(),
    [](const velk::ui::PointerEvent& e) {
        if (e.action == velk::ui::PointerAction::Down &&
            e.button == velk::ui::PointerButton::Right) {
            // start a drag
        }
    });
```

The dispatcher routes events through the bound scene's element hierarchy via hit testing for elements that have input traits (Click, Hover, Drag). See [ui/input.md](../ui/input.md) for the full input pipeline.

## Plugin loading

`Application::init()` loads a fixed set of standard plugins so the rest of the runtime works out of the box:

| Plugin | Purpose |
|---|---|
| `velk_ui` | UI element / scene / input system, built-in visuals. |
| `velk_render` | Render context, surface, shader material, base renderer. |
| `velk_vk` | Vulkan backend (selected by `RenderBackendType::Default` on desktop). |
| `velk_text` | Font loading and text rendering. |
| `velk_image` | Image loading (PNG, HDR) and image visual. |
| `velk_importer` | JSON scene loader. |
| Platform plugin | `velk_runtime_glfw` on desktop, `velk_runtime_android` on Android (planned). |

Custom plugins can be loaded explicitly after `create_app()`:

```cpp
auto app = velk::create_app({});
velk::instance().plugin_registry().load_plugin_from_path("my_plugin.dll");
```

## Threading

The runtime is single-threaded by default. Calling `app.present()` does prepare and submit on the calling thread.

For threaded rendering, split into `prepare()` (main / UI thread) and `submit(frame)` (render thread). The renderer is designed for this split — prepared frames live in `frame_slots_` and the render thread can pick them up via `submit()`. See [render/rendering.md](../render/rendering.md) for details.

The main thread should always call `update()` and `prepare()` (the scene update is single-threaded). Only `submit()` should run on a different thread.

## Classes

ClassIds for the runtime's main types. Most user code only touches `Application` (via `velk::create_app()`); the rest exists for embedding and advanced use.

| ClassId | Implements | Description |
|---|---|---|
| `velk::ClassId::Application` | `IApplication` | The runtime entry point. Owns the render context, default renderer, set of windows, frame loop. Construct via `velk::create_app(config)` rather than directly. |
| `velk::ClassId::GlfwWindow` | `IWindow` | GLFW-backed managed window. Created by the GLFW platform plugin in response to `app.create_window()` or `app.wrap_native_surface()`. Defined in `velk-runtime/plugins/glfw/plugin.h`. |

The runtime ships as two cooperating plugins:

| PluginId | Description |
|---|---|
| `velk::PluginId::RuntimePlugin` | Core plugin (`velk_runtime`) that registers the `Application` type. Loaded automatically by `create_app()`. |
| `velk::PluginId::RuntimeGlfwPlugin` | Desktop platform plugin (`velk_runtime_glfw`) implementing `IWindowProvider` for GLFW. Loaded by `Application::init()` via compile-time selection. |
| `velk::PluginId::RuntimeAndroidPlugin` | Android platform plugin (planned). Placeholder UID until implementation lands. |

## See also

- [Getting started](../getting-started.md) — minimal example walkthrough
- [Scene](../ui/scene.md) — what to render: elements, traits, hierarchies
- [Input](../ui/input.md) — input pipeline, gestures, custom traits
- [Update cycle](../ui/update-cycle.md) — what `velk::instance().update()` does
- [Rendering](../render/rendering.md) — what the renderer does internally
