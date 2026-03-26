# Update cycle

velk-ui's frame loop is driven by `velk::instance().update()`, which triggers the scene's layout solver, followed by `renderer->render()` which pulls changes and draws.

## Frame loop

A typical main loop:

```cpp
while (running) {
    glfwPollEvents();
    velk::instance().update();  // drives scene update via plugin post_update
    renderer->render();         // pulls scene state, batches, draws
    glfwSwapBuffers(window);
}
```

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
        Solver->>Solver: measure + apply constraints
        Scene->>Scene: mark all elements for redraw
    end
    Scene->>Scene: clear dirty flags

    App->>Renderer: render()
    Renderer->>Scene: consume_state()
    Scene-->>Renderer: SceneState (visual_list, redraw_list, removed_list)
    alt has changes or resize
        Renderer->>Renderer: rebuild draw commands for redraw_list
        Renderer->>Renderer: pack instance data into RenderBatch[]
    end
    Renderer->>Backend: begin_frame(surface_id)
    Renderer->>Backend: submit(batches)
    Renderer->>Backend: end_frame()
    App->>App: swap buffers
```

The velk-ui plugin hooks into velk's update cycle via `post_update()`. For each live scene, it calls `Scene::update()`, which processes dirty flags accumulated since the last frame.

The renderer is passive and pull-based. It calls `scene->consume_state()` during `render()` to get the current visual list and any changes since the last frame.

### Dirty flags

Changes are tracked with `DirtyFlags`:

| Flag | Trigger | Effect |
|------|---------|--------|
| `Layout` | Element position/size changed, scene geometry changed | Re-runs the layout solver, marks all elements for redraw |
| `Visual` | Visual property changed (color, text, paint, etc.) | Element added to redraw list |
| `DrawOrder` | Element z-index changed, hierarchy modified | Rebuilds the z-sorted visual list |

Flags accumulate between frames. A single `update()` processes all pending changes at once.

### Scene update steps

1. **Collect element dirty flags**: each element that was notified of a property change has its flags consumed and merged into the scene's dirty flags. Elements with visual changes are added to the redraw list.
2. **Rebuild draw list** (if `DrawOrder` is set): elements are collected in z-sorted order.
3. **Layout solve** (if `Layout` is set): the solver walks the hierarchy top-down, calling `measure()` and `apply()` on each element's constraints. This writes final position and size into element state. All elements are marked for redraw since transforms changed.
4. **Clear flags**: all dirty flags are reset for the next frame.

### Renderer steps (during render())

1. **Check surface resize**: if the surface dimensions changed, update the backend and mark batches dirty.
2. **Consume scene state**: pull `SceneState` from each attached scene.
3. **Process removals**: evict cached draw commands for removed elements.
4. **Rebuild draw commands**: for each element in the redraw list, query visuals for draw commands, resolve materials to pipeline keys.
5. **Rebuild batches** (if dirty): pack instance data into `RenderBatch` structs grouped by pipeline/format/texture.
6. **Submit to backend**: `begin_frame`, `submit(batches)`, `end_frame`.

On clean frames (nothing changed), steps 3-5 are skipped and the renderer re-submits cached batches.

### Scene geometry

Layout bounds are set explicitly on the scene, decoupled from any renderer or surface:

```cpp
scene.set_geometry(velk::aabb::from_size({800.f, 600.f}));
```

To handle window resize, update both the scene geometry and the surface dimensions:

```cpp
static void on_resize(GLFWwindow* window, int width, int height)
{
    scene->set_geometry(velk::aabb::from_size({
        static_cast<float>(width), static_cast<float>(height)}));
    velk::write_state<velk_ui::ISurface>(surface, [&](velk_ui::ISurface::State& s) {
        s.width = width;
        s.height = height;
    });
}
```

The scene will re-solve layout on the next `update()`. The renderer detects the surface dimension change during `render()` and rebuilds batches.

### Deferred updates

Property changes can be deferred via velk's `Deferred` flag. Deferred writes are batched and applied during `velk::instance().update()`, before the scene processes them. This is useful for bulk property changes that should trigger only one layout pass.
