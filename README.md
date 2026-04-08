<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://github.com/ljaaskela/velk/blob/main/docs/logos/logo/velk-logo-standard-dark%402x.png">
  <source media="(prefers-color-scheme: light)" srcset="https://github.com/ljaaskela/velk/blob/main/docs/logos/logo/velk-logo-standard-light%402x.png">
  <img alt="Velk" src="https://github.com/ljaaskela/velk/blob/main/docs/logos/logo/velk-logo-standard-dark%402x.png" width="200">
</picture>

Application platform built on the [Velk](https://github.com/ljaaskela/velk) component object model. Provides GPU rendering, a UI framework, text rendering, and application infrastructure.

## Modules

### Rendering foundation ([velk-render](velk-render/))

Pointer-based GPU rendering abstraction:
* Minimal backend interface relying on buffer device addresses, bindless textures, push-constant-driven draw calls.
* Includes a Vulkan 1.2 backend (`velk::vk`) with BDA and bindless descriptors.

### UI framework ([velk-ui](velk-ui/))

Declarative UI framework:
* Scene graphs, element composition via traits (constraints, visuals, transforms, input), JSON scene loading.
* A scene renderer using velk-render that walks the visual tree and submits draw calls to the render backend.
* Text rendering plugin (`velk_text`) using analytic Bezier glyph coverage adapted from Eric Lengyel's [Slug](https://github.com/EricLengyel/Slug) reference shaders. No glyph atlas: outlines are baked once with FreeType, packed into GPU curve and band buffers, and shaded per-pixel with exact analytic coverage. Scale-independent: one font instance renders at any pixel size with no re-baking.
* Image plugin (`velk_image`) decoding raster images via stb_image into bindless GPU textures.

## Documentation

| Document | Description |
|---|---|
| [Getting started](velk-ui/docs/getting-started.md) | Build, run the demo, walk through a minimal scene |
| [Scene](velk-ui/docs/scene.md) | Element trees, traits, hierarchies, the visual graph |
| [Traits](velk-ui/docs/traits.md) | Built-in traits: layout, visuals, transforms, input |
| [Update cycle](velk-ui/docs/update-cycle.md) | The frame loop, dirty tracking, scene state consumption |
| [Input](velk-ui/docs/input.md) | Hit testing, event dispatch, focus |
| [Performance](velk-ui/docs/performance.md) | Renderer cost, batching, hot paths |
| **Built-in plugins** | |
| [Text](velk-ui/docs/plugins/text.md) | Analytic Bezier text rendering, scale-independent fonts |
| [Image](velk-ui/docs/plugins/image.md) | Image decoding and bindless texture binding |

## Quick start

```cpp
// Rendering setup
auto ctx = velk::create_render_context(config);                     // Create a render context
auto surface = ctx.create_surface(1280, 720);                       // Create a surface target surface 

// UI setup
auto scene = velk::ui::create_scene("app://scenes/dashboard.json"); // Load a Scene from JSON
auto renderer = velk::ui::create_renderer(*render_ctx);             // Create a Scene renderer and attach it to a target surfece
renderer->attach(surface, scene);

// Main loop
while (running) {
    velk.update();
    renderer->render();
}
```

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
