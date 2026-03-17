# velk-ui

UI rendering framework and test application built on the [Velk](https://github.com/user/velk) component object model.

## Structure

| Directory | Description |
|-----------|-------------|
| `app/` | Test application: initializes GLFW, loads plugins, imports a scene, runs a render loop |
| `velk-ui/` | Core UI library: `IRenderer`, `IElement`, `Element` plugin |
| `plugins/render/gl/` | OpenGL renderer plugin (`velk_gl`) implementing `IRenderer` |
| `scenes/` | Scene definitions in JSON (loaded by the Velk importer) |
| `glad/` | OpenGL loader (GLAD 2) |
| `third_party/` | GLFW 3.4 (vendored) |

## Dependencies

* **Velk** framework (`../velk/`), built separately
* **GLFW 3.4** for windowing
* **GLAD 2** for OpenGL loading
* CMake 3.14+, MSVC 2019 (C++17)

## Building

Build Velk first if it has changed:

```bash
cd ../velk
cmake -B build -G "Visual Studio 16 2019" -A x64 -T v142
cmake --build build --config Release
```

Then build velk-ui:

```bash
cmake -B build -G "Visual Studio 16 2019" -A x64 -T v142
cmake --build build --config Release
```

## Running

```bash
./build/bin/Release/velk_ui_app.exe
```

The app loads `scenes/hello.json`, creates elements with position/size/color properties, and renders them with the GL plugin.
