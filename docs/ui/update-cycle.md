# Update cycle

This document is the **internal reference** for what happens inside `velk::instance().update()` when the scene tick runs: dirty flag accumulation, the layout solver, trait phases, and how the renderer pulls scene state during `prepare()`. For the user-level frame loop (poll/update/present), see [runtime.md](../runtime/runtime.md).

## Contents
- [Trait phases](#trait-phases)
  - [Solver pipeline (per element, top-down)](#solver-pipeline-per-element-top-down)
- [What happens during a frame](#what-happens-during-a-frame)
- [Dirty flags](#dirty-flags)
- [Scene update steps](#scene-update-steps)
- [Renderer steps](#renderer-steps)
  - [prepare()](#prepare)
  - [present(frame)](#presentframe)
- [Selective and multi-rate rendering](#selective-and-multi-rate-rendering)
- [Scene geometry](#scene-geometry)
- [Deferred updates](#deferred-updates)


## Trait phases

Every trait attached to an element belongs to one of following phases. See [Traits](traits.md) for the full guide on each category.

| Phase | Interface | Runs during | Purpose |
|-------|-----------|-------------|---------|
| **Input** | `IInputTrait` | Input dispatch | Handles pointer, scroll, key events. E.g. Click, Hover, Drag |
| **Layout** | `ILayoutTrait` | Scene update | Walks children, divides space. E.g. Stack |
| **Constraint** | `ILayoutTrait` | Scene update | Refines own size. E.g. FixedSize |
| **Transform** | `ITransformTrait` | Scene update | Modifies the world matrix. E.g. Trs, Matrix |
| **Visual** | `IVisual` | Renderer | Produces draw commands. E.g. RectVisual, TextVisual |
| **Input** | `IInputTrait` | Scene update | Input handling. Does not participate in the layout solver |
| **Render** | `none` | Renderer | Render observation. Defines a view into the scene. E.g. Camera |

Input runs synchronously when the platform delivers events (typically via the runtime's window dispatcher — see [Input](input.md)). Layout, Constraint, and Transform run inside `Scene::update()` via the layout solver. Visual runs inside `renderer->prepare()` when the renderer queries each element's visuals for draw commands.

### Solver pipeline (per element, top-down)

For each element in the hierarchy, the solver runs:

1. **Measure** (Layout + Constraint traits, sorted by phase): each `ILayoutTrait` refines the available bounds. Layout traits may query children. Constraint traits clamp or adjust size.
2. **Write size**: the solver writes the measured size into the element's state.
3. **Apply** (Layout + Constraint traits, same order): each trait writes final position/size. Layout traits position children.
4. **Compute world matrix**: `parent_world * translate(position)`.
5. **Transform** (all `ITransformTrait` attachments): each modifies the world matrix in place (rotation, scale, skew).
6. **Recurse** into children with the updated world matrix.

## What happens during a frame

```mermaid
sequenceDiagram
    participant App as Application
    participant Velk as velk::instance()
    participant Plugin as VelkUiPlugin
    participant Scene
    participant Solver as LayoutSolver
    participant Renderer as IRenderer
    participant Backend as IRenderBackend

    App->>Velk: update()
    Velk->>Velk: flush deferred property writes
    Velk->>Plugin: post_update()
    Plugin->>Scene: update()
    Scene->>Scene: collect element dirty flags
    alt DrawOrder dirty
        Scene->>Scene: rebuild z-sorted visual list
    end
    alt Layout dirty
        Scene->>Solver: solve(hierarchy, geometry)
        Solver->>Solver: measure + apply (Layout, Constraint)
        Solver->>Solver: compute world matrix
        Solver->>Solver: run transform traits
        Scene->>Scene: mark all elements for redraw
    end
    Scene->>Scene: clear dirty flags

    App->>Renderer: prepare()
    Renderer->>Backend: begin_frame()
    Renderer->>Scene: consume_state()
    Scene-->>Renderer: SceneState (visual_list, redraw_list, removed_list)
    alt has changes or resize
        Renderer->>Renderer: query IVisual draw commands for redraw_list
        Renderer->>Renderer: pack instance data into RenderBatch[]
    end
    Renderer->>Renderer: build DrawCall array, write GPU buffers
    Renderer->>Backend: execute graph + close_frame (record primary)
    Renderer-->>App: Frame handle

    App->>Renderer: present(frame)
    Renderer->>Backend: submit_frame() (acquire, blit, submit, present)
```

The velk-ui plugin hooks into velk's update cycle via `post_update()`. For each live scene, it calls `Scene::update()`, which processes dirty flags accumulated since the last frame.

The renderer is passive and pull-based. It calls `scene->consume_state()` during `prepare()` to get the current visual list and any changes since the last frame.

## Dirty flags

Changes are tracked with `DirtyFlags`:

| Flag | Trigger | Effect |
|------|---------|--------|
| `Layout` | Element position/size changed, scene geometry changed | Re-runs the layout solver, marks all elements for redraw |
| `Visual` | Visual property changed (color, text, paint, etc.) | Element added to redraw list |
| `DrawOrder` | Element z-index changed, hierarchy modified | Rebuilds the z-sorted visual list |

Flags accumulate between frames. A single `update()` processes all pending changes at once.

## Scene update steps

1. **Collect element dirty flags**: each element that was notified of a property change has its flags consumed and merged into the scene's dirty flags. Elements with visual changes are added to the redraw list.
2. **Rebuild draw list** (if `DrawOrder` is set): elements are collected in z-sorted order.
3. **Layout solve** (if `Layout` is set): the solver walks the hierarchy top-down. For each element it runs the trait phases (Layout, Constraint, Transform) as described above. All elements are marked for redraw since transforms may have changed.
4. **Clear flags**: all dirty flags are reset for the next frame.

## Renderer steps

### prepare()

1. **begin_frame**: wait on the slot's GPU fence and start the primary command buffer.
2. **Check surface resize / native-window swap**: if the surface dimensions changed (or the platform window was recreated on suspend/resume), update the backend (`resize_surface` / `recreate_surface`) and mark batches dirty.
3. **Consume scene state**: pull `SceneState` from each attached scene.
4. **Process removals**: evict cached draw commands for removed elements.
5. **Rebuild draw commands**: for each element in the redraw list, query `IVisual` attachments for draw commands, resolve materials to pipeline keys.
6. **Rebuild batches** (if dirty): pack instance data into `RenderBatch` structs grouped by pipeline/format/texture.
7. **Write GPU buffers**: write instance data, draw headers, and material params into the staging buffer. Build the `DrawCall` array.
8. **Record the frame**: compile + execute the frame graph into the primary command buffer, then `close_frame`. The returned `Frame` is fully recorded.

On clean frames (nothing changed), steps 4-6 are skipped and the renderer re-submits cached batches.

### present(frame)

`present()` records nothing — the frame was fully recorded in `prepare()`. It calls `submit_frame`, which acquires the swap image, blits each surface's composite into its swapchain, submits the GPU work, and presents. Keeping all recording in `prepare()` and only the GPU submit + present here lets the two phases run on separate threads.

Older unpresented frames targeting the same surfaces are silently discarded.

## Selective and multi-rate rendering

The `prepare()` method accepts a `FrameDesc` that controls which surfaces and cameras to render:

```cpp
// Render everything (default)
auto frame = renderer->prepare();

// Render only one surface
auto frame = renderer->prepare({{surface_a}});

// Render specific cameras on a surface
auto frame = renderer->prepare({{surface_a, {cam1, cam2}}});

// Render two surfaces
auto frame = renderer->prepare({{surface_a}, {surface_b}});
```

This enables multi-rate rendering where different surfaces update at different frequencies. Each surface's frames are independently sequential:

```cpp
if (time_for_60hz) {
    // Both surfaces update: one prepare, one present
    auto f = renderer->prepare({{main_surface}, {secondary_surface}});
    renderer->present(f);
} else {
    // Only the main display updates this tick
    auto f = renderer->prepare({{main_surface}});
    renderer->present(f);
}
```

## Scene geometry

Layout bounds are set explicitly on the scene, decoupled from any renderer or surface:

```cpp
scene.set_geometry(velk::aabb::from_size({800.f, 600.f}));
```

When using the runtime, `app.add_view(window, camera)` sets the scene geometry from the window size and listens for `on_resize` to keep them synchronized — you don't have to do this yourself. If you're driving rendering manually (no runtime), update both the scene geometry and the ISurface dimensions when the window resizes; the renderer detects the surface dimension change during `prepare()` and rebuilds batches, and the scene re-solves layout on the next `update()`.

## Deferred updates

Property changes can be deferred via velk's `Deferred` flag. Deferred writes are batched and applied during `velk::instance().update()`, before the scene processes them. This is useful for bulk property changes that should trigger only one layout pass.
