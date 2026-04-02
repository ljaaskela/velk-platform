# velk-ui

UI framework built on the [Velk](https://github.com/ljaaskela/velk) component object model. Declarative scene loading from JSON, programmatic element creation, trait-based composition (constraints, visuals), and a plugin architecture for rendering and text.

```cpp
// Load a scene
auto scene = velk_ui::create_scene("app://scenes/my_scene.json");

// Create an OpenGL renderer
auto renderer = ::velk::instance().create<velk_ui::IRenderer>(velk_ui::ClassId::GlRenderer);
renderer->init(1280, 720));

// Set renderer
scene.set_renderer(renderer);

// Or build UI programmatically
auto elem = velk_ui::create_element();                          // Create an empty element

auto fs = velk_ui::constraint::create_fixed_size();             // Create size constraint
fs.set_size(velk_ui::dim::px(200.f), velk_ui::dim::px(100.f));  // Define constraints (in pixels)

auto rect = velk_ui::visual::create_rect();                     // Create rectangle visual
rect.set_color({0.9f, 0.2f, 0.2f, 1.f});                        // Set the draw color

elem.add_trait(fs);                                             // Add the traits to the element
elem.add_trait(rect);

scene.add(scene.root(), elem);                                  // Add the element as child or root node in the scene
```

## Documentation

| Document | Description |
|----------|-------------|
| [Getting started](docs/getting-started.md) | Scene loading, programmatic API, and the two ways to build UI |
| [Scene](docs/scene.md) | Scene hierarchy, elements, geometry, JSON format |
| [Traits](docs/traits.md) | Trait system: phases, layout, transform, visual, and input traits |
| [Input](docs/input.md) | Input dispatcher, hit testing, event dispatch, built-in input traits |
| [Update cycle](docs/update-cycle.md) | Frame loop, dirty flags, layout solving, and rendering |
| [Performance](docs/performance.md) | Design choices: single element type, flat hierarchy, traits, batched updates |

## Project structure

| Directory | Description |
|-----------|-------------|
| `velk-ui/` | Core UI library: elements, scene, layout solver, constraints, visuals |
| `velk-ui/include/velk-ui/api/` | High-level API wrappers (Scene, Element, Trait, etc.) |
| `velk-ui/include/velk-ui/interface/` | Pure virtual interfaces (IScene, IElement, IConstraint, IVisual, etc.) |
| `plugins/render/gl/` | OpenGL renderer plugin (`velk_gl`) |
| `plugins/text/` | Text plugin (`velk_text`): FreeType + HarfBuzz font shaping and rendering |
| `app/` | Test application: GLFW window, plugin loading, scene loading, render loop |
| `test/scenes/` | Scene definitions in JSON |
| `third_party/` | GLFW 3.4 (vendored) |

## Building

Velk is built from source automatically as part of the velk-ui build. The `VELK_SOURCE_DIR` cache variable defaults to `../velk`.

```bash
cmake -B build -G "Visual Studio 16 2019" -A x64 -T v142
cmake --build build --config Release
```

To use a different velk checkout:

```bash
cmake -B build -DVELK_SOURCE_DIR=/path/to/velk -G "Visual Studio 16 2019" -A x64 -T v142
```

## Running

```bash
./build/bin/Release/velk_ui_app.exe
```

## Dependencies

* [Velk](https://github.com/ljaaskela/velk) (`../velk/` by default)
* GLFW 3.4 (vendored)
* GLAD 2 (included)
* FreeType 2.13, HarfBuzz 10.2 (vendored in `plugins/text/third_party/`)
* CMake 3.14+
