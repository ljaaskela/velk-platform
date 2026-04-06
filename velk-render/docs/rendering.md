# Rendering

This document covers how frames are prepared, submitted, and presented. For the GPU data model and backend architecture, see [Render Backend Architecture](render-backend-architecture.md). For the full frame loop including scene updates, see [Update Cycle](../../velk-ui/docs/update-cycle.md).

## prepare / present split

Rendering is split into two phases:

| Phase | Method | Thread | Work |
|-------|--------|--------|------|
| **Prepare** | `renderer->prepare(desc)` | Main thread | Consume scene state, rebuild draw commands, write GPU buffers. Returns an opaque `Frame` handle. |
| **Present** | `renderer->present(frame)` | Any thread | Submit draw calls to the backend (`begin_frame`, `submit`, `end_frame`). Blocks on vsync. |

The convenience method `renderer->render()` calls `present(prepare({}))` for the simple single-threaded case.

### Threading model

`present()` blocks on vsync (typically 16-17ms at 60Hz). During that time the main thread could be running the next `velk::instance().update()` and `prepare()`.

The renderer does not create threads. The application decides the threading strategy:

**Single-threaded** (simplest):
```cpp
while (running) {
    glfwPollEvents();
    velk::instance().update();
    renderer->render();
}
```

**Threaded** (overlapping prepare and present):
```cpp
// Main thread
while (running) {
    glfwPollEvents();
    velk::instance().update();
    Frame frame = renderer->prepare();
    // Hand frame to render thread
    render_queue.push(frame);
}

// Render thread
while (running) {
    Frame frame = render_queue.pop();
    renderer->present(frame);
}
```

**Platform-driven** (e.g. Android):
```cpp
// Main thread: called by app framework
void on_update() {
    velk::instance().update();
    Frame frame = renderer->prepare();
    pending_frame = frame;
}

// Render thread: called by platform (GLSurfaceView, Choreographer)
void on_draw_frame() {
    renderer->present(pending_frame);
}
```

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
2. For each view matching the `FrameDesc`:
   a. Check for surface resize; update backend if needed
   b. `scene->consume_state()` to get the redraw/removed lists
   c. Evict removed elements from the draw command cache
   d. `rebuild_commands()` for dirty elements (query `IVisual` attachments)
   e. Upload dirty textures (e.g. glyph atlas updates)
   f. `rebuild_batches()` if batches are dirty (group by pipeline + texture)
   g. Write instance data, draw headers, and material params to the GPU staging buffer
   h. Build the `DrawCall` array
3. Store draw calls per surface in the frame slot
4. Return the `Frame` handle

## What present() does internally

1. Discard older unpresented frames targeting the same surfaces
2. For each surface in the frame:
   a. `backend->begin_frame(surface_id)`: acquire swapchain image
   b. `backend->submit(draw_calls)`: record into command buffer
   c. `backend->end_frame()`: submit GPU work and present
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
