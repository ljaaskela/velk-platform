# Rendering

This document is the **internal reference** for `IRenderer` and the prepare/submit pipeline. For everyday use, the runtime sets up the renderer for you and you call `app.update()` / `app.present()` instead of touching the renderer directly — see [runtime.md](../runtime/runtime.md). Read this when you need to drive the renderer manually, when you want to understand what `app.prepare()` and `app.submit()` do internally, or when you're implementing a custom render path.

For the GPU data model and backend architecture, see [render-backend.md](render-backend.md). For what `velk::instance().update()` does (which runs before any rendering), see [update-cycle.md](../ui/update-cycle.md).

## Contents
- [Views: renderer, surfaces, and cameras](#views-renderer-surfaces-and-cameras)
  - [One scene, multiple surfaces](#one-scene-multiple-surfaces)
  - [One surface, multiple cameras](#one-surface-multiple-cameras)
  - [Multiple scenes](#multiple-scenes)
  - [Relationship diagram](#relationship-diagram)
- [prepare / present split](#prepare-present-split)
  - [Per-frame GPU buffers](#per-frame-gpu-buffers)
  - [Threading model](#threading-model)
- [FrameDesc: selective rendering](#framedesc-selective-rendering)
- [Frame slots and back-pressure](#frame-slots-and-back-pressure)
- [Frame skipping](#frame-skipping)
- [Multi-rate rendering](#multi-rate-rendering)
- [What prepare() does internally](#what-prepare-does-internally)
- [What present() does internally](#what-present-does-internally)
- [Performance profiling](#performance-profiling)
- [Classes](#classes)


## Views: renderer, surfaces, and cameras

The renderer draws scenes onto surfaces through **views**. A view is a pairing of a camera element and a surface:

```cpp
auto renderer = velk::ui::create_renderer(*render_ctx);
renderer->add_view(camera_element, surface);
```

When using the runtime, `app.add_view(window, camera)` does the equivalent — it pulls the surface from the window and forwards to `renderer->add_view`.

**Surfaces** (`ISurface`) represent render targets. A surface maps to a backend swapchain with a `surface_id`. It has width and height properties but no knowledge of scenes or cameras. Surfaces are created via `IRenderContext::create_surface()`.

**Camera elements** are regular scene elements with an `ICamera` trait attached. The camera provides the view-projection matrix for rendering. The camera's element also provides the scene: `camera_element->get_scene()` is how the renderer finds which scene to draw.

**Scenes** own the element hierarchy, layout solver, and dirty tracking. They are passive during rendering: the renderer pulls state via `scene->consume_state()`.

### One scene, multiple surfaces

A scene can be rendered to multiple surfaces by adding multiple views with cameras from the same scene:

```cpp
renderer->add_view(main_camera, monitor_surface);
renderer->add_view(main_camera, projector_surface);
```

Each surface gets its own swapchain and presentation timing. The scene is consumed once per prepare; both surfaces share the same draw commands (rebuild happens once) but get separate GPU submissions.

### One surface, multiple cameras

Multiple cameras can render to the same surface (e.g. a split-screen or picture-in-picture setup):

```cpp
renderer->add_view(camera_left, surface);
renderer->add_view(camera_right, surface);
```

Each camera provides a different view-projection matrix. The renderer processes them sequentially within a single frame, each producing its own set of draw calls for the same surface.

### Multiple scenes

Cameras from different scenes can coexist in the same renderer:

```cpp
renderer->add_view(game_camera, main_surface);     // game scene
renderer->add_view(hud_camera, main_surface);       // HUD scene (overlay)
renderer->add_view(minimap_camera, minimap_surface); // minimap scene
```

Each camera's `get_scene()` returns its own scene. The renderer consumes state from each scene independently.

### Relationship diagram

```
IRenderer
 ├── View: camera_a + surface_1  ──► Scene A (via camera_a->get_scene())
 ├── View: camera_b + surface_1  ──► Scene B (via camera_b->get_scene())
 └── View: camera_c + surface_2  ──► Scene A (via camera_c->get_scene())

Surface 1 ──► Backend swapchain (surface_id=1)
Surface 2 ──► Backend swapchain (surface_id=2)
```

Views are registered with `add_view()` and removed with `remove_view()`. The `FrameDesc` passed to `prepare()` can filter which surfaces and cameras to include in a given frame (see [Selective rendering](#framedesc-selective-rendering) below).

## prepare / present split

Rendering is split into two phases:

| Phase | Method | Thread | Work |
|-------|--------|--------|------|
| **Prepare** | `renderer->prepare(desc)` | Main thread | Consume scene state, rebuild draw commands, write GPU buffers, and record the frame's command buffers (the whole frame is recorded here). Returns an opaque `Frame` handle. |
| **Present** | `renderer->present(frame)` | Any thread | Submit the already-recorded frame to the GPU and present it (`submit_frame`). Records nothing itself; blocks on vsync. |

The convenience method `renderer->render()` calls `present(prepare({}))` for the simple single-threaded case.

### Per-frame GPU buffers

Each frame slot owns its own GPU staging buffer. When `prepare()` writes instance data, draw headers, and material params, it writes into the slot's buffer, not a shared one. This means a prepared frame's GPU data is never overwritten by a subsequent `prepare()` call. The buffer remains valid and untouched until `present()` submits its draw calls and recycles the slot.

The staging buffers are small (starting at 256 KB, growing on demand) because they only hold per-frame metadata: 
* `DrawDataHeader` structs (32 bytes each)
* inline instance data (32-48 bytes per quad) and 
* material parameters. 

Heavy data like textures and persistent mesh buffers lives in separate GPU allocations outside the frame buffer. Even a complex frame with thousands of draw entries typically uses under 1 MB.

Globals (view-projection matrix, viewport) are written to a separate persistent buffer that is updated in-place during `prepare()`. This is safe because the values are only read by the GPU during `submit()`, which happens after `prepare()` completes.

### Threading model

`present()` blocks on vsync (typically 16-17ms at 60Hz). During that time the main thread could be running the next `velk::instance().update()` and `prepare()`.

The renderer does not create threads. The application decides the threading strategy:

- **Single-threaded**: call `renderer->render()` (or `app.present()`) which does prepare + present sequentially.
- **Threaded**: prepare on the main thread, send the `Frame` handle to a render thread, present from there.
- **Platform-driven**: e.g. on Android, prepare from the framework's update callback and present from `onDrawFrame` on the render thread.

The runtime layer wraps these patterns — see [runtime.md](../runtime/runtime.md) for `app.prepare()` / `app.submit()` and how to split them across threads. The frame slot system below is what makes this safe regardless of the threading strategy.

## FrameDesc: selective rendering

`prepare()` accepts a `FrameDesc` that controls which surfaces and cameras to render:

```cpp
struct ViewDesc
{
    ISurface::Ptr surface;
    vector<IElement::Ptr> cameras;  // Empty = all cameras for this surface
};

struct FrameDesc
{
    vector<ViewDesc> views;  // Empty = all registered views
};
```

An empty `FrameDesc` (the default) renders all registered views. Specifying surfaces or cameras filters the work.

## Frame slots and back-pressure

The renderer manages a pool of frame slots. Each `prepare()` claims a slot and fills it with draw calls. `present()` submits the slot and recycles it.

If all slots are occupied (prepare is outpacing present), `prepare()` blocks until a slot becomes available. This provides natural back-pressure without unbounded memory growth.

The pool size is configurable at runtime:

```cpp
renderer->set_max_frames_in_flight(2);  // default is 3, minimum 1
```

Lower values reduce latency (fewer pre-rendered frames) at the cost of potentially stalling prepare when present is slow. Higher values allow more overlap but increase input-to-display latency.

## Frame skipping

`present(frame)` presents that frame and silently discards all older unpresented frames that target the same surfaces, recycling their slots. This means:

- No frame leaks: stale frames are cleaned up automatically
- Skipping frames is a normal operation, not an error (i.e. the app can decide that an intermediate frame is stale)
- Independent surfaces are not affected: presenting frame on `surface1` does not discard pending frames on `surface2` if they have been prepared separately.

## Multi-rate rendering

Different surfaces can update at different frequencies. Include multiple surfaces in a single `prepare()` call when they need to update together, so that `present()` submits them back-to-back without blocking between surfaces:

```cpp
// This example renders main_surface on every frame but secondary_surface only when time_for_60Hz is true

if (time_for_60hz) {
    // Both surfaces update: one prepare, one present, one block
    auto f = renderer->prepare({{main_surface}, {secondary_surface}});
    renderer->present(f);
} else {
    // Only the main display updates this tick
    auto f = renderer->prepare({{main_surface}});
    renderer->present(f);
}
```

A single `prepare()` call can target any combination of surfaces. The resulting frame contains draw calls for all of them, and `present()` submits them in sequence within a single call.

## What prepare() does internally

1. Claim a frame slot from the pool (block if none free)
2. `backend->begin_frame()`: wait on the slot's GPU fence, start the primary command buffer
3. For each view matching the `FrameDesc`:
   a. Check for surface resize / native-window swap; update the backend if needed (`resize_surface` / `recreate_surface`)
   b. `scene->consume_state()` to get the redraw/removed lists
   c. Evict removed elements from the draw command cache
   d. `rebuild_commands()` for dirty elements (query `IVisual` attachments)
   e. Upload dirty textures (e.g. glyph atlas updates)
   f. `rebuild_batches()` if batches are dirty (group by pipeline + texture)
   g. Write instance data, draw headers, and material params to the GPU staging buffer
   h. Build the `DrawCall` array and emit the view's passes into the frame graph
4. Compile + execute the frame graph (records the primary command buffer), then `backend->close_frame()`
5. Return the `Frame` handle — the frame is now fully recorded

## What present() does internally

1. Discard older unpresented frames targeting the same surfaces
2. `backend->submit_frame()`: acquire the swap image, run the composite-to-swap blit, submit GPU work, and present
3. Recycle the frame slot

## Performance profiling

The renderer is instrumented with `VELK_PERF_SCOPE` at key stages. With stats collection enabled (on by default), accumulated timing data is printed at shutdown:

```
[PERF]   renderer.prepare              med=  0.007ms  p95=  0.007ms  ...
[PERF]   renderer.rebuild_commands     med=  0.002ms  p95=139.026ms  ...
[PERF]   renderer.rebuild_batches      med=  0.008ms  p95=  0.008ms  ...
[PERF]   renderer.build_draw_calls     med=  0.004ms  p95=  0.004ms  ...
[PERF]   renderer.present              med=  6.927ms  p95=  7.147ms  ...
[PERF]   renderer.begin_frame          med=  6.825ms  p95=  7.024ms  ...
```

Custom perf scopes can be added with `#include <velk/api/perf.h>`:

```cpp
VELK_PERF_SCOPE("my_operation");
```

Stats can be queried programmatically via `instance().perf_log().get_stats()`.

## Classes

ClassIds for the rendering layer's main types. The runtime constructs these on first window creation; manual construction is only needed when bypassing the runtime.

| ClassId | Implements | Description |
|---|---|---|
| `velk::ClassId::RenderContext` | `IRenderContext` | Owns the render backend, surface factory, shader compiler, pipeline registry. Construct via `velk::create_render_context(config)`. |
| `velk::ClassId::Surface` | `ISurface` | Render target with `width`, `height`, `update_rate`, `target_fps` properties. Created by `IRenderContext::create_surface(SurfaceConfig)`. The actual swapchain is built lazily when the renderer's `add_view` first sees the surface. |
| `velk::ClassId::Renderer` | `IRenderer`, `IRendererInternal` | Scene renderer. Walks views, builds batches, writes GPU buffers, submits to the backend. Construct via `velk::ui::create_renderer(ctx)`. |

For shader materials see [Materials](materials.md). For the lower-level GPU interface (`IRenderBackend`, buffers, pipelines, bindless textures) see [Render backend](render-backend.md).
