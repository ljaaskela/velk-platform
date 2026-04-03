# velk-ui

UI framework built on the [Velk](https://github.com/ljaaskela/velk) component object model. Declarative scene loading from JSON, programmatic element creation, trait-based composition (constraints, visuals), and a plugin architecture for rendering and text.

![Dashboard](docs/velk-ui-social-preview.png)

## Pointer-based GPU backend

velk-ui uses a [pointer-based render backend](docs/render-backend-architecture.md) that maps directly to how modern GPUs work, rather than abstracting over graphics API concepts. The entire GPU interface is 15 methods. Shaders access all data through GPU pointers (`buffer_reference`), textures are bindless indices, and geometry is procedurally generated or pulled from buffers by the shader. No vertex input descriptions, no uniform reflection, no descriptor management.

This design is inspired by the observation that modern GPU hardware has converged: buffer device addresses, bindless descriptors, and coherent caches are universally available. When the hardware is uniform, the abstraction layer can be radically simplified.

See [Render Backend Architecture](docs/render-backend-architecture.md) for the full technical writeup.

## Quick start

```cpp
auto ctx = velk_ui::create_render_context(config);
auto renderer = ctx.create_renderer();
auto surface = ctx.create_surface(1280, 720);

auto scene = velk_ui::create_scene("app://scenes/dashboard.json");
renderer->attach(surface, scene);

while (running) {
    velk.update();
    renderer->render();
}
```

## Documentation

| Document | Description |
|----------|-------------|
| [Getting started](docs/getting-started.md) | Scene loading, programmatic API, and the two ways to build UI |
| [Render backend](docs/render-backend-architecture.md) | Pointer-based GPU abstraction: architecture, data flow, shader model |
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
| `plugins/render/core/` | Render core (`velk_render`): renderer, shader compiler, backend interface |
| `plugins/render/vk/` | Vulkan backend (`velk_vk`): Vulkan 1.2, BDA, bindless textures |
| `plugins/text/` | Text plugin (`velk_text`): FreeType + HarfBuzz font shaping and rendering |
| `app/` | Test application: GLFW window, plugin loading, scene loading, render loop |
| `test/scenes/` | Scene definitions in JSON |

## Building

Requires CMake 3.14+, MSVC 2019 (C++17), and the Vulkan SDK (for shaderc).

Velk is built from source automatically. The `VELK_SOURCE_DIR` cache variable defaults to `../velk`.

```bash
cmake -B build -G "Visual Studio 16 2019" -A x64 -T v142
cmake --build build --config Release
```

## Running

```bash
./build/bin/Release/velk_ui_app.exe
```

## Dependencies

* [Velk](https://github.com/ljaaskela/velk) (`../velk/` by default)
* GLFW 3.4 (vendored)
* Vulkan SDK (shaderc for runtime GLSL to SPIR-V compilation)
* volk, VMA (Vulkan function loader and memory allocator, header-only via Vulkan SDK)
* FreeType 2.13, HarfBuzz 10.2 (vendored in `plugins/text/third_party/`)
* CMake 3.14+
