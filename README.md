<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://github.com/ljaaskela/velk/blob/main/docs/logos/logo/velk-logo-standard-dark%402x.png">
  <source media="(prefers-color-scheme: light)" srcset="https://github.com/ljaaskela/velk/blob/main/docs/logos/logo/velk-logo-standard-light%402x.png">
  <img alt="Velk" src="https://github.com/ljaaskela/velk/blob/main/docs/logos/logo/velk-logo-standard-dark%402x.png" width="200">
</picture>

Application platform built on the [Velk](https://github.com/ljaaskela/velk) component object model. Provides GPU rendering, a UI framework, text rendering, and application infrastructure.

## Modules

### Application runtime ([velk-runtime](velk-runtime/))

The user-facing entry point. Wraps render context creation, window management, plugin loading, and the frame loop behind a single `Application` class. Supports both desktop (managed window via GLFW) and framework-driven (wrapped native surface, Android / embedded) modes.

### Rendering foundation ([velk-render](velk-render/))

Bindless GPU rendering abstraction:
* Minimal backend interface relying on shared, permanently bound buffers indexed by the shader, bindless textures, and push-constant-driven draw calls.
* Includes a Vulkan 1.2 backend (`velk::vk`) with bindless descriptors and dynamic rendering.

### Scene model + renderer ([velk-scene](velk-scene/))

Element / Scene primitives, scene graph, transforms, 3D visuals, and the renderer:
* `IElement` / `IScene` interfaces, JSON scene loading, BVH, dirty tracking.
* Render paths (Forward / Deferred / RayTrace) consuming the scene model and submitting draw calls via velk-render.
* Render-feeding traits (Camera, Light) and 3D visuals (Cube, Sphere, Mesh) live here alongside the transform traits (Trs, Matrix, LookAt, Orbit).

![rendering sample](docs/render/bistro.jpg)
glTF-converted Amazon Lumberyard Bistro scene rendering with velk-scene: 20 warm local lights with soft contact shadows. The panels on the right are G-buffer debug views.

### UI framework ([velk-ui](velk-ui/))

Declarative UI policy on top of velk-scene:
* Layout solver and constraint / layout traits (Stack, FixedSize), 2D visuals (Rect, RoundedRect, Texture), input (Click, Hover, Drag), gradient + render caches.
* Importer type extensions and value-type registrations consumed by the JSON scene loader.

#### Plugins
* Text rendering plugin ([velk_text](docs/plugins/text.md)) using analytic Bezier glyph coverage adapted from Eric Lengyel's [Slug](https://github.com/EricLengyel/Slug) reference shaders. No glyph atlas: outlines are baked once with FreeType, packed into GPU curve and band buffers, and shaded per-pixel with exact analytic coverage. Scale-independent: one font instance renders at any pixel size with no re-baking.
* Image plugin ([velk_image](docs/plugins/image.md)) decoding raster images via stb_image into bindless GPU textures.

## Documentation

User documentation lives at [`docs/`](docs/); see [`docs/README.md`](docs/README.md) for the index. Quick links:

* **[Getting started](docs/getting-started.md)** Minimal example walkthrough
* **[Runtime](docs/runtime/runtime.md)** Application setup, frame loop, both creation modes
* **[Scene](docs/ui/scene.md)** / **[Traits](docs/ui/traits.md)** / **[Input](docs/ui/input.md)** Scene + UI authoring
* **[Rendering](docs/render/rendering.md)** / **[Render backend](docs/render/render-backend.md)** Internals
* **[Text](docs/plugins/text.md)** / **[Image](docs/plugins/image.md)** Bundled plugins

## Project structure

```
velk-ui/
  CMakeLists.txt
  README.md               This file
  android/                Gradle project: builds the tree for Android and packages an APK
    velk-platform/        NDK entry point; its CMakeLists builds the whole velk-ui tree
    samples/simple/       NativeActivity app module wrapping the simple sample
  docs/                   User documentation (see docs/README.md for the index)
    getting-started.md    Minimal example walkthrough
    render/               Renderer internals
      rendering.md        IRenderer, prepare / present split, frame slots
      render-backend.md   GPU resource model, bindless design, IRenderBackend
      materials.md        Material authoring, eval bodies, shader includes
      mesh.md             IMesh / IMeshPrimitive / IMeshBuffer
      lighting.md         Lights, shadows, environment / IBL
    ui/                   Scene, traits, input, update cycle, performance
    runtime/              Application setup and frame loop
    plugins/              Bundled plugin docs (text, image, gltf)
  plugins/                Feature plugins (each compiled into its own dll)
    gltf/                 glTF 2.0 import
    image/                Raster image + HDR environment decoding
    text/                 FreeType + HarfBuzz shaping, analytic glyph coverage
  samples/                Desktop sample apps
    simple/               2D UI, render-to-texture, custom shader materials
    fluent/               3D scene, ray-traced path
    winding_test/         Front-face / depth convention regression check
  velk-render/            Rendering foundation
    include/velk-render/
      interface/          Abstract interfaces (backend, buffers, textures, materials)
      ext/                CRTP helpers (ext::Material, ext::GpuBuffer, ...)
      api/                User-facing typed wrappers
      frame/              Draw-call emission, shader sources, render graph
      render_path/        Forward / deferred / ray-trace path interfaces
    src/                  Implementation
    plugins/
      vk/                 Vulkan backend (velk::vk)
      rt/                 Software ray-tracing render path
  velk-runtime/           Application runtime: window management, plugin loading, frame loop
    plugins/
      glfw/               Desktop window + input
      android/            NativeActivity window, asset protocol, log sink
  velk-scene/             Scene model, transforms, BVH, batching, the renderer
  velk-ui/                UI framework: layout, 2D visuals, materials, input routing
```

Note the two Android directories are different layers: `velk-runtime/plugins/android/`
is C++ that runs on Android and builds one runtime plugin, while `android/` is the
Gradle build shell that compiles the whole repo and wraps it in an APK.

## Quick start

A minimal application which shows a fixed 64px*64px velk logo on screen.

```cpp
#include <velk-runtime/api/application.h>
#include <velk-scene/api/scene.h>

int main()
{
    // Initialize velk
    auto app = velk::create_app({});
    // Create a window
    auto window = app.create_window({.width = 1280, .height = 720, .title = "demo"});
    // Load a scene from a file
    auto scene = velk::create_scene("app://scenes/velk_logo.json");
    // Find the first Element with a Trait implementing ICamera and bind it to the window
    if (auto camera = scene.find_first<velk::ICamera>()) {
        app.add_view(window, camera);
    }
    // Application loop
    while (app.poll()) {
        app.update();
        app.present();
    }
}
```

velk_logo.json:
```json
{
  "version": 1,
  "objects": [
    { "id": "root", "class": "velk-scene.Element" },
    { "id": "camera", "class": "velk-scene.Element" },
    { "id": "logo", "class": "velk-scene.Element" }
  ],
  "hierarchies": {
    "scene": {
      "root": ["logo", "camera"]
    }
  },
  "attachments": [
    { "targets": ["camera"],  "class": "velk-scene.Camera" },
    { "targets": ["logo"],    "class": "velk-ui.FixedSize", "properties": { "width": "64px", "height": "64px" } },
    { "targets": ["logo"],    "class": "velk_image.ImageVisual", "properties": { "uri": "image:app://assets/logo.png" } }
  ]
}
```

Or, equivalently, build the same scene programmatically in C++ instead of loading from JSON:

```cpp
#include <velk-scene/api/camera.h>
#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
#include <velk-ui/api/trait/fixed_size.h>
#include <velk-ui/plugins/image/api/image_visual.h>

using namespace velk;

// Empty scene; no JSON file
auto scene = create_scene();

// Three elements ("objects")
auto root   = create_element();
auto camera = create_element();
auto logo   = create_element();

// Hierarchy: root has children ("hierarchies")
scene.set_root(root);
scene.add(root, logo);
scene.add(root, camera);

// Camera: Camera trait ("attachments": velk-scene.Camera)
camera.add_trait(trait::render::create_camera());

// logo: FixedSize 64x64 ("attachments": velk-ui.FixedSize)
logo.add_trait(ui::trait::layout::create_fixed_size(dim::px(64.f), dim::px(64.f)));

// logo: Visual trait to show the image ("attachments": velk_image.ImageVisual)
logo.add_trait(ui::trait::visual::create_image("image:app://assets/logo.png"));
```

Build & run:

![Application](./docs/minimal_app.png)

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
