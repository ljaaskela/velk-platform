# Scene

A `Scene` is the top-level container for UI. It owns a tree of elements, runs layout each frame, and pushes visual changes to a renderer.

## Hierarchy

Scene extends velk's `ClassId::Hierarchy`, which provides a general-purpose parent/child tree of `IObject` pointers. In velk-ui, those objects are always elements. The `Scene` API wrapper inherits `velk::Hierarchy`, so all hierarchy operations (add, remove, replace, iterate) work directly on the scene.

```cpp
auto scene = velk_ui::create_scene();

auto root = velk_ui::create_element();
auto child = velk_ui::create_element();

scene.set_root(root);
scene.add(root, child);

auto r = scene.root();                // returns Element
auto c = scene.child_at(root, 0);     // returns Element
auto p = scene.parent_of(child);      // returns Element
```

Scene overrides the velk `Hierarchy` node accessors to return `Element` instead of `velk::Node`, so there is no need to cast.

## Elements

An `Element` is the fundamental building block. It holds position, size, transform, and z-index properties. An element has no visual appearance on its own; that comes from traits.

```cpp
auto elem = velk_ui::create_element();
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

Traits are attachments that give elements behavior. All traits implement `ITrait` and can be managed via `add_trait()` / `remove_trait()`:

```cpp
elem.add_trait(some_constraint);
elem.add_trait(some_visual);
elem.remove_trait(some_visual);
auto found = elem.find_trait<IFixedSize>();
```

Currently there are two categories of traits:

### Constraints

Constraints control how elements are sized and positioned during layout.

| Class | Interface | Description |
|-------|-----------|-------------|
| `Stack` | `IStack` | Arranges children along an axis with spacing |
| `FixedSize` | `IFixedSize` | Clamps width and/or height to a fixed value |

```cpp
auto stack = velk_ui::constraint::create_stack();
stack.set_axis(1);       // vertical
stack.set_spacing(10.f);

auto fs = velk_ui::constraint::create_fixed_size();
fs.set_size(velk_ui::dim::px(200.f), velk_ui::dim::px(100.f));
```

Dimensions can be absolute (`dim::px(100.f)`) or relative to parent (`dim::pct(0.5f)`). Use `dim::none()` to leave an axis unconstrained.

### Visuals

Visuals define how an element appears on screen. An element can have multiple visuals.

| Class | Interface | Description |
|-------|-----------|-------------|
| `RectVisual` | `IVisual` | Solid color rectangle filling the element bounds |
| `TextVisual` | `ITextVisual` | Shaped text rendered as glyph quads |

```cpp
auto rect = velk_ui::visual::create_rect();
rect.set_color({0.9f, 0.2f, 0.2f, 1.f});

auto text = velk_ui::visual::create_text();
text.set_font(font);
text.set_text("Hello!");
text.set_color(velk::color::white());
```

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
auto scene = velk_ui::create_scene("app://scenes/my_scene.json");
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

Available class names: `velk-ui.Element`, `velk-ui.Stack`, `velk-ui.FixedSize`, `velk-ui.RectVisual`, `velk-ui.Font`, `velk-ui.TextVisual`.
