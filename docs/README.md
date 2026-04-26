# velk-platform documentation

User documentation for [velk-platform](../README.md): the application runtime, the UI framework, the rendering pipeline, and the bundled plugins.

If this is your first time, start with [Getting started](getting-started.md) for a minimal example, then read [runtime/runtime.md](runtime/runtime.md) for the high-level overview.

## Runtime

The user-facing entry point. Application setup, window management, frame loop, performance overlay.

| Document | Description |
|---|---|
| [Runtime](runtime/runtime.md) | `Application`, `Window`, both creation modes (managed / framework-driven), frame loop, performance overlay, plugin loading |

## Scene + UI

What you put inside a window: scenes, elements, traits, input. The scene model + renderer + transform / camera / light / 3D-visual traits live in `velk-scene`; layout, input, and 2D visuals live in `velk-ui` on top of it. The split is mostly invisible at the authoring level — JSON class names are the only place it surfaces (e.g. `velk-scene.Element`, `velk-ui.Stack`).

| Document | Description |
|---|---|
| [Scene](ui/scene.md) | Scene hierarchy, elements, geometry, JSON loading |
| [Traits](ui/traits.md) | The trait system: phases, layout / constraint / transform / visual / input traits, CRTP bases |
| [Input](ui/input.md) | Input dispatcher, hit testing, dispatch model, built-in traits, custom input traits |
| [Update cycle](ui/update-cycle.md) | What `velk::instance().update()` does internally: dirty flags, layout solver, trait phases |
| [Performance](ui/performance.md) | Performance design choices: single element type, flat hierarchy, batched dirty processing |

## Render

The lower-level rendering layer. Most apps don't touch this directly — the runtime sets it up.

| Document | Description |
|---|---|
| [Rendering](render/rendering.md) | Internal renderer reference: views, prepare/submit split, frame slots, multi-rate rendering, threading model |
| [Render backend](render/render-backend.md) | GPU data model, bindless shader interface, Vulkan implementation details |
| [Mesh](render/mesh.md) | `IMesh` / `IMeshPrimitive` / `IMeshBuffer` container-primitive-buffer model, procedural and authored meshes |
| [Materials](render/materials.md) | Built-in materials and shader materials with dynamic inputs; where materials attach on 2D vs 3D visuals |

## Plugins

Bundled feature plugins. Loaded automatically by `create_app()`.

| Document | Description |
|---|---|
| [Text](plugins/text.md) | Font loading, text rendering with analytic Bezier coverage (no glyph atlas) |
| [Image](plugins/image.md) | Image and HDR environment loading, image visuals, skybox materials |

## Reference

For per-class / per-method documentation, build doxygen from the velk and velk-platform sources. Each public header is annotated.

## Design notes

Internal scratch / planning material — not user documentation — lives at [`../design-notes/`](../design-notes/) at the repo root.
