# Scene

A `Scene` is the top-level container for UI. It owns a tree of elements, runs layout each frame, and pushes visual changes to a renderer.

## Hierarchy

Scene extends velk's `ClassId::Hierarchy`, which provides a general-purpose parent/child tree of `IObject` pointers. In velk-ui, those objects are always elements. The `Scene` API wrapper inherits `velk::Hierarchy`, so all hierarchy operations (add, remove, replace, iterate) work directly on the scene.

```cpp
auto scene = velk::ui::create_scene();

auto root = velk::ui::create_element();
auto child = velk::ui::create_element();

scene.set_root(root);
scene.add(root, child);

auto r = scene.root();                // returns Element
auto c = scene.child_at(root, 0);     // returns Element
auto p = scene.parent_of(child);      // returns Element
```

Scene overrides the velk `Hierarchy` node accessors to return `Element` instead of `velk::Node`, so there is no need to cast.

## Elements

An `Element` is the fundamental building block. It holds position, size, and z-index properties. An element has no visual appearance on its own; that comes from traits.

```cpp
auto elem = velk::ui::create_element();
elem.set_position({10.f, 20.f, 0.f});
elem.set_size({200.f, 100.f});
elem.set_z_index(1);  // draw on top of siblings with lower z-index
```

`Element` inherits `velk::Node`, so hierarchy navigation works from any element:

```cpp
auto parent = elem.get_parent();
auto children = elem.get_children();
elem.for_each_child<IElement>([](IElement& child) { /* ... */ });
```

## Traits

Traits are attachments that give elements behavior: layout, appearance, input handling, transforms. An element on its own has only position, size, and z-index; everything else comes from traits.

```cpp
elem.add_trait(some_constraint);
elem.add_trait(some_visual);
elem.remove_trait(some_visual);
auto found = elem.find_trait<IFixedSize>();
```

See [Traits](traits.md) for the full guide: trait phases, built-in traits (layout, transform, visual, input), CRTP bases, and how to write custom traits.

## Geometry and rendering

The scene's layout bounds are set explicitly, decoupled from any renderer or surface:

```cpp
scene.set_geometry(velk::aabb::from_size({800.f, 600.f}));
```

The scene does not know about the renderer. Instead, the renderer pulls state from the scene during `render()` via `consume_state()`, which returns a `SceneState` containing:

- `visual_list`: all elements in z-sorted draw order
- `redraw_list`: elements whose visuals changed since the last consume
- `removed_list`: elements that were detached (kept alive until consumed)

To connect a scene to a renderer, attach it to a surface:

```cpp
renderer->attach(surface, scene);
```

See [Update cycle](update-cycle.md) for the full frame flow.

## Loading from JSON

Scenes can be loaded from JSON files via the velk resource store:

```cpp
auto scene = velk::ui::create_scene("app://scenes/my_scene.json");
```

The JSON format declares objects, hierarchy, and trait attachments:

```json
{
  "version": 1,
  "objects": [
    { "id": "root", "class": "velk-ui.Element" },
    { "id": "child", "class": "velk-ui.Element" }
  ],
  "hierarchies": {
    "scene": { "root": ["child"] }
  },
  "attachments": [
    { "targets": ["root"], "class": "velk-ui.Stack", "properties": { "axis": 1 } },
    { "targets": ["child"], "class": "velk-ui.RectVisual", "properties": { "color": { "r": 1, "g": 0, "b": 0 } } }
  ]
}
```

Available class names: `velk-ui.Element`, `velk-ui.Stack`, `velk-ui.FixedSize`, `velk-ui.Trs`, `velk-ui.Matrix`, `velk-ui.RectVisual`, `velk-ui.RoundedRectVisual`, `velk-ui.Font`, `velk_text.TextVisual`.
