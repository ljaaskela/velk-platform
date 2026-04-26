# Getting started

A walkthrough of a minimal velk-platform application: build, run, then incrementally add scene loading, programmatic UI, and input handling.

For the architectural overview, see [Runtime](runtime/runtime.md). For the underlying systems, see [Scene](ui/scene.md), [Traits](ui/traits.md), [Input](ui/input.md).

## Contents
- [Building](#building)
- [Minimal main](#minimal-main)
- [JSON scenes](#json-scenes)
- [Programmatic UI](#programmatic-ui)
- [Adding text](#adding-text)
- [Handling input](#handling-input)
- [Performance overlay](#performance-overlay)
- [Next steps](#next-steps)


## Building

Requires CMake 3.14+, MSVC 2019 (C++17), and the Vulkan SDK (for shaderc).

```bash
cmake -B build -G "Visual Studio 16 2019" -A x64 -T v142
cmake --build build --config Release
./build/bin/Release/velk_ui_app.exe
```

velk is built from source automatically. The `VELK_SOURCE_DIR` cache variable defaults to `../velk`.

## Minimal main

The smallest velk-platform program creates an application, opens a window, loads a scene, and runs a frame loop:

```cpp
#include <velk-runtime/api/application.h>
#include <velk-scene/api/scene.h>

int main()
{
    auto app = velk::create_app({});
    if (!app) {
        return 1;
    }

    auto window = app.create_window({.width = 1280, .height = 720, .title = "demo"});
    auto scene = velk::create_scene("app://scenes/dashboard.json");

    if (auto camera = scene.find_first<velk::ICamera>()) {
        app.add_view(window, camera);
    }

    while (app.poll()) {
        app.update();
        app.present();
    }
}
```

That's the entire entry point. `velk::create_app()` loads all standard plugins, sets up the render context, and returns an `Application` ready to create windows. `app.create_window()` opens a native window via the GLFW platform plugin. `scene.find_first<ICamera>()` walks the scene hierarchy and returns the first element with an `ICamera` trait — no need to know the scene's structural layout. `app.add_view()` binds that camera to the window and wires up input routing and resize handling automatically.

## JSON scenes

The example above loads a scene from a JSON file. A minimal scene file looks like this:

```json
{
  "version": 1,
  "objects": [
    { "id": "scene_root", "class": "velk-scene.Element" },
    { "id": "camera",     "class": "velk-scene.Element" },
    { "id": "panel",      "class": "velk-scene.Element" }
  ],
  "hierarchies": {
    "scene": {
      "scene_root": ["panel", "camera"]
    }
  },
  "attachments": [
    { "targets": ["camera"], "class": "velk-scene.Camera" },
    { "targets": ["panel"],  "class": "velk-ui.FixedSize",
      "properties": { "width": "300px", "height": "200px" } },
    { "targets": ["panel"],  "class": "velk-ui.RectVisual",
      "properties": { "color": { "r": 0.2, "g": 0.7, "b": 0.4, "a": 1 } } }
  ]
}
```

The `app://` URI scheme resolves paths relative to the executable's working directory. The first child of `scene_root` is the panel; the second is the camera (which `app.add_view()` reads). See [Scene](ui/scene.md) for the full JSON format.

## Programmatic UI

You can also build elements in code, mixing freely with JSON-loaded ones:

```cpp
#include <velk-scene/api/element.h>
#include <velk-ui/api/trait/fixed_size.h>
#include <velk-ui/api/visual/rect.h>

auto button = velk::create_element();

auto fs = velk::ui::trait::layout::create_fixed_size();
fs.set_size(velk::ui::dim::px(120.f), velk::ui::dim::px(40.f));
button.add_trait(fs);

auto rect = velk::ui::trait::visual::create_rect();
rect.set_color({0.9f, 0.2f, 0.2f, 1.f});
button.add_trait(rect);

scene.add(scene.root(), button);
```

## Adding text

```cpp
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/api/text_visual.h>

auto label = velk::create_element();
auto text = velk::ui::trait::visual::create_text();
text.set_font(velk::ui::get_default_font());   // built-in Inter Regular
text.set_text("Hello, velk!");
text.set_font_size(24.f);
text.set_color(velk::color::white());
label.add_trait(text);
scene.add(scene.root(), label);
```

The text plugin is loaded by `create_app()` automatically. See [Text](plugins/text.md) for details.

## Handling input

Input is routed automatically by `app.add_view()`. To make an element clickable, attach a `Click` trait and listen for the `on_click` event:

```cpp
#include <velk-ui/api/input/click.h>

auto click = velk::ui::trait::input::create_click();
button.add_trait(click);

velk::ScopedHandler click_sub(click.on_click(),
    []() { VELK_LOG(I, "clicked!"); });
```

The `ScopedHandler` unsubscribes automatically when it goes out of scope. See [Input](ui/input.md) for the full input pipeline (hit testing, intercept/bubble, custom traits).

For global input (orbit cameras, debug hotkeys), subscribe directly to the window's input dispatcher:

```cpp
auto* dispatcher = window.input();
velk::ScopedHandler raw(dispatcher->on_pointer_event(),
    [](const velk::ui::PointerEvent& e) {
        if (e.action == velk::ui::PointerAction::Down &&
            e.button == velk::ui::PointerButton::Right) {
            // handle global right-click
        }
    });
```

## Performance overlay

The runtime can render an FPS / CPU / frame-time overlay on any window:

```cpp
app.set_performance_overlay(window);
```

See [Runtime](runtime/runtime.md#performance-overlay) for configuration options.

## Next steps

- [Runtime](runtime/runtime.md) — full reference for `Application`, `Window`, both creation modes
- [Scene](ui/scene.md) — element hierarchies, JSON format, trait composition
- [Traits](ui/traits.md) — built-in layout / transform / visual / input traits, writing custom ones
- [Input](ui/input.md) — input dispatch model, gestures, custom input traits
- [Performance](ui/performance.md) — design choices behind velk-ui's hot path
