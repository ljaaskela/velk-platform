# Getting started

There are two ways to build UI with velk-ui: JSON scene files and the programmatic API. Both can be mixed freely.

## Plugins

Before creating any UI objects, load the required plugins:

```cpp
auto& velk = velk::instance();
velk.plugin_registry().load_plugin_from_path("velk_ui.dll");
velk.plugin_registry().load_plugin_from_path("velk_gl.dll");
velk.plugin_registry().load_plugin_from_path("velk_text.dll");
velk.plugin_registry().load_plugin_from_path("velk_importer.dll");
```

## JSON scenes

Define a scene as a JSON file and load it with `create_scene`:

```json
{
  "version": 1,
  "objects": [
    { "id": "root", "class": "velk-ui.Element" },
    { "id": "panel", "class": "velk-ui.Element" }
  ],
  "hierarchies": {
    "scene": { "root": ["panel"] }
  },
  "attachments": [
    { "targets": ["root"],  "class": "velk-ui.Stack", "properties": { "axis": 1, "spacing": 10 } },
    { "targets": ["panel"], "class": "velk-ui.FixedSize", "properties": { "height": "200px" } },
    { "targets": ["panel"], "class": "velk-ui.RectVisual", "properties": { "color": { "r": 0.2, "g": 0.7, "b": 0.2 } } }
  ]
}
```

```cpp
#include <velk-ui/api/scene.h>

auto scene = velk_ui::create_scene("app://scenes/my_scene.json");
scene.set_renderer(renderer);
```

The `app://` protocol resolves paths relative to the working directory. `file://` uses absolute paths. See velk's resource store documentation for details.

## Programmatic API

Build UI in code using the API wrappers:

```cpp
#include <velk-ui/api/element.h>
#include <velk-ui/api/constraint/fixed_size.h>
#include <velk-ui/api/visual/rect.h>

auto elem = velk_ui::create_element();

auto fs = velk_ui::constraint::create_fixed_size();
fs.set_size(velk_ui::dim::px(300.f), velk_ui::dim::px(100.f));
elem.add_trait(fs);

auto rect = velk_ui::visual::create_rect();
rect.set_color({0.9f, 0.2f, 0.2f, 1.f});
elem.add_trait(rect);

scene.add(scene.root(), elem);
```

## Text

```cpp
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/api/text_visual.h>

auto font = velk_ui::create_font();
font.init_default();  // built-in Inter Regular
font.set_size(32.f);

auto tv = velk_ui::visual::create_text();
tv.set_font(font);
tv.set_text("Hello, Velk!");
tv.set_color(velk::color::white());

elem.add_trait(tv);
```

## Main loop

The velk update cycle drives layout and rendering:

```cpp
while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    velk::instance().update();
    renderer->render();
    glfwSwapBuffers(window);
}
```

See [Update cycle](update-cycle.md) for details on what happens during `update()`.
