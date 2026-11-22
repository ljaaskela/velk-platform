# Traits

Traits are the primary extension mechanism in velk-ui. An element on its own has only position, size, and z-index. Everything else (layout, appearance, input handling, transforms) comes from traits attached to it.

## Contents
- [Why traits](#why-traits)
- [Trait phases](#trait-phases)
- [Attaching traits](#attaching-traits)
- [CRTP base classes](#crtp-base-classes)
- [Layout traits](#layout-traits)
  - [Scroll view](#scroll-view)
  - [List view](#list-view)
- [Clipping](#clipping)
- [Transform traits](#transform-traits)
- [Visual traits](#visual-traits)
  - [Visual phase](#visual-phase)
- [Render traits](#render-traits)
- [Input traits](#input-traits)
- [Writing a custom trait](#writing-a-custom-trait)
- [Built-in trait classes](#built-in-trait-classes)
  - [Constraint phase](#constraint-phase)
  - [Transform phase](#transform-phase)
  - [Visual phase](#visual-phase)
  - [Material classes](#material-classes)
  - [Camera](#camera)


## Why traits

A traditional UI framework bakes behavior into a class hierarchy: `Button` inherits `Widget`, `ScrollView` inherits `Container`, etc. This creates rigid trees where adding a new combination of behaviors means creating a new class.

velk-ui uses composition instead. An element is a blank container. Attach a `RectVisual` to give it color. Attach a `Stack` to make it lay out its children. Attach a `Click` to make it respond to pointer events. These can be mixed freely at runtime without subclassing.

This maps directly to velk's object model: traits are velk objects attached to an element via `IObjectStorage`. They're discovered by interface (`ILayoutTrait`, `IVisual`, `IInputTrait`) so the solver, renderer, and input dispatcher each find only the traits they care about.

## Trait phases

Every trait implements `ITrait` and reports which phases of the element pipeline it participates in via `get_phase()`. The return value is a bitmask, so a single trait can participate in multiple phases.

| Phase | Purpose | Typical interface |
|-------|---------|-------------------|
| **Layout** | Measures and places children | `ILayoutTrait` |
| **Constraint** | Describes the element's own box | `ILayoutTrait` |
| **Transform** | Modifies the world matrix | `ITransformTrait` |
| **Visual** | Produces draw commands | `IVisual` |
| **Input** | Handles pointer, scroll, key events | `IInputTrait` |
| **Render** | Defines how the scene is observed | `ICamera` |

The phase determines *when* the trait runs, not *what interface* it must implement. For example, `ILayoutTrait` is used by both Layout and Constraint phases; a Stack trait returns `TraitPhase::Layout` while a FixedSize trait returns `TraitPhase::Constraint`. A trait could return both (`TraitPhase::Layout | TraitPhase::Constraint`) to participate in both phases.

Layout, Constraint, and Transform run inside the layout solver during `Scene::update()`. Visual and Render run during `renderer->render()`. Input runs synchronously when the input dispatcher receives platform events. See [Update cycle](update-cycle.md) for the full frame flow.

## Attaching traits

Traits are managed via `add_trait()` / `remove_trait()` on the Element wrapper:

```cpp
auto elem = velk::create_element();

auto stack = velk::ui::trait::layout::create_stack();
auto rect = velk::ui::trait::visual::create_rect();
auto click = velk::ui::trait::input::create_click();

elem.add_trait(stack);
elem.add_trait(rect);
elem.add_trait(click);

elem.remove_trait(rect);
auto found = elem.find_trait<IFixedSize>();  // nullptr if not attached
```

An element can have multiple traits, including multiple traits of the same phase (e.g. a Layout trait and a Constraint trait, or two Visuals).

**Note**: Under the hood traits are just attachments managed through `velk::IObjectStorage` interface. So the low level attachment interfaces work too, Element API wrapper just provides an easier-to-use path.

## CRTP base classes

Each trait category has a CRTP base in `velk::ui::ext` that provides sensible defaults:

| Base | Phase | Defaults |
|------|-------|----------|
| `ext::LayoutTrait<T, Phase, ...>` | Layout or Constraint | No-op describe, measure and arrange |
| `ext::Transform<T, ...>` | Transform | No-op transform |
| `ext::Visual2D<T, ...>` | Visual | 2D visual base (color, paint, visual phase). Fires `on_visual_changed` on any property change |
| `ext::Visual3D<T, ...>` | Visual | 3D visual base (mesh reference). Fires `on_visual_changed` on any property change |
| `ext::Input<T, ...>` | Input | All handlers return `Ignored` |
| `ext::Render<T, ...>` | Render | Base for render-phase traits (e.g. Camera) |

Subclass the appropriate base and override only what you need.

## Layout traits

Layout traits implement `ILayoutTrait` and control how elements are sized and positioned.

| Trait | Interface | Phase | Description |
|-------|-----------|-------|-------------|
| Stack | `IStack` | Layout | Places children one after another along an axis |
| FixedSize | `IFixedSize` | Constraint | Width, height and depth: fixed, auto or the space offered |
| Padding | `IPadding` | Constraint | Space between the element's edges and its children |
| Margin | `IMargin` | Constraint | Space around the element |
| Align | `IAlign` | Constraint | The element's own alignment, overriding its parent's |
| ScrollView | `IScrollView` | Layout | Scrolls and clips the children, with scroll bars |

```cpp
auto stack = velk::ui::trait::layout::create_stack();
stack.set_axis(1);       // vertical
stack.set_spacing(10.f);
stack.set_align(velk::Alignment::Center);

auto fs = velk::ui::trait::layout::create_fixed_size();
fs.set_size(velk::dim::px(200.f), velk::dim::auto_size());

auto padding = velk::ui::trait::layout::create_padding(velk::dim::px(8.f));
```

In a store file:

```json
{ "targets": ["label"], "class": "velk-ui.FixedSize", "properties": { "width": "auto", "height": "24px" } },
{ "targets": ["label"], "class": "velk-ui.Margin", "properties": { "left": "8px" } },
{ "targets": ["label"], "class": "velk-ui.Align", "properties": { "vertical": "center" } }
```

### Scroll view

`ScrollView` places each child at the start of the element, at least as large as the view and as large as it wants to be, and moves it by `scroll_x` and `scroll_y`. Give the content child an `auto` size on the axes it should scroll on. The view:

* clips the children to its content area (it is an `IClip`, see [Clipping](#clipping)),
* scrolls with the mouse wheel, `wheel_step` pixels per line, Shift for sideways; at the end of its content a wheel turn goes on to an enclosing view,
* shows a bar on an axis whose content does not fit. A bar is `bar_width` pixels, taken from the content's space. Its thumb has the visual's `color` and can be dragged; a click on the track pages.

The offsets are kept within the content. One attachment does the layout, the clipping, the input and the bars.

```json
{ "targets": ["panel"], "class": "velk-ui.FixedSize", "properties": { "width": "300px", "height": "220px" } },
{ "targets": ["panel"], "class": "velk-ui.ScrollView", "properties": { "color": { "r": 0.7, "g": 0.72, "b": 0.78, "a": 0.8 } } },
{ "targets": ["list"], "class": "velk-ui.FixedSize", "properties": { "width": "auto", "height": "auto" } }
```

### List view

`ListView` shows the top-level items of an item source (`IItemSource` from velk_data, for example an `ObjectList`) as rows of `row_height` pixels. The element is as tall as all its rows. Placed in a ScrollView, it creates rows only for the items in its visible part and reuses them as it scrolls.

A row is an instance of the store file `row_template`. Its root has a `velk-ui.ListRow` attachment with id `row`; the view writes the row's `item`, `index` and `selected` there, and the row's elements bind through it:

```json
{ "id": "row", "targets": ["root"], "class": "velk-ui.ListRow" },
{ "targets": ["label"], "class": "velk-text.TextVisual", "properties": { "text": { "bind": "row.item.label" } } }
```

A click selects the item under it (`selected`), and the selected row gets a background in the visual's `color`. `source` is set from code:

```cpp
auto items = create_object_list();
write_state<IListView>(view, [&](IListView::State& s) { set_object_ref(s.source, items); });
```

## Clipping

`velk-scene.Clip` (`IClip`) cuts an element and everything under it at the element's box: what lies outside is not drawn, to the pixel, and takes no pointer input. Nested clips intersect. `enabled` turns it off. `ScrollView` and `TextField` are clips themselves.

A clip element's own visuals are cut at its box, its descendants at `IClip::clip_rect`, which is the box for `Clip` and the area without the bars for `ScrollView`.

How it works: the batch builder gives each clipped instance the index of its clip rectangle, the rectangles are uploaded each frame, and the element vertex shader writes `gl_ClipDistance` against them. An element wholly outside its clip is not drawn at all. Limits:

* The rectangle is axis-aligned in world x and y, so it suits 2D content; a rotated clip clips to its bounds.
* Ray traced shadows and reflections ignore clipping.
* A render target's subtree (`RenderCache`) is not clipped.
* Up to 1024 clips are in use at once; more do not clip.
* Moving an element that a clip left out rebuilds the batches rather than taking the transform-only path.

### Sizes

Each axis of `FixedSize` is one of:

* none (`dim::none()`, the default): the space the parent offers.
* px (`"200px"` or `200`): fixed.
* pct (`"50%"`): a share of the space offered.
* auto (`"auto"`): fits the content, which is the largest of the children's sizes and the content size of the element's visuals. Text, image and texture visuals have a content size (`IContentSize`).

Padding and margin values are px or pct of the space offered on that axis. Padding moves the children, not the element's own visuals: a background fills the whole element, and text with padding around it goes on a child element.

### How the solver works

Each element's Constraint-phase traits describe its box (`describe()`): size, margin, padding and alignment. The solver runs one box model over it, so the order of the traits does not matter: margin outside, then the size, then padding around the children.

* **Measure** (bottom-up): an element reports the size it wants for the space offered, and per axis whether it takes whatever it is given. Content is measured only where the size is auto, or by a Layout-phase trait such as Stack.
* **Arrange** (top-down): the parent gives each child a rectangle. The child takes its margin off, sizes itself, and places itself by its alignment: its own `Align`, else the parent's. Then it arranges its children in its content rectangle.

A Layout-phase trait measures and places the children through `ILayoutContext`. Without one, each child is placed on its own in the parent's content rectangle, so children overlap: at its top-left corner plus the child's margin, sized by the child's `FixedSize` (filling the rectangle without one), unless the child sets its own alignment with `Align`.

`Stack` gives children with a fixed or auto size along its axis that size, and shares what is left between the children that take the space offered. Across the axis it places them by its `align` property (`fill` by default).

**Visible part.** Arranging an element also gives it the part of its box that shows: the parent's visible part cut to the element's box, and cut again by any `Clip` above it. A ScrollView's children therefore see its viewport, moved by the scroll position. A Layout-phase trait reads it in `arrange` with `ILayoutContext::visible()`, in the element's own space, for example to create only the rows that show.

**Measures are kept.** An element's content size is measured again only when the element or something under it changed, or it is offered different space (`ILayoutContext::changed()` tells a trait which). Scrolling arranges the content again without measuring it.

## Transform traits

Transform traits implement `ITransformTrait` and modify the element's world matrix after layout, before children are recursed.

| Trait | Interface | Description |
|-------|-----------|-------------|
| Trs | `ITrs` | Decomposed translate, rotate (Z), scale |
| Matrix | `IMatrix` | Raw 4x4 matrix multiply |
| LookAt | `ILookAt` | Orients the element to face a target element |
| Orbit | `IOrbit` | Positions and orients the element on a sphere around a target |

```cpp
auto trs = velk::trait::transform::create_trs();
trs.set_rotation(45.f);         // degrees around Z
trs.set_scale({0.5f, 0.5f});
elem.add_trait(trs);
```

LookAt and Orbit are particularly useful for cameras. Orbit positions the element at a given distance, yaw (horizontal angle), and pitch (vertical angle) from the target element's center, and orients it to face the target:

```cpp
auto orbit = velk::trait::transform::create_orbit();
orbit.set_target(scene.root());
orbit.set_distance(1200.f);
orbit.set_yaw(30.f);    // degrees
orbit.set_pitch(15.f);  // degrees
camera_element.add_trait(orbit);
```

In JSON:

```json
{ "targets": ["camera_3d"], "class": "velk-scene.Orbit",
  "properties": { "target": { "ref": "root", "type": "weak" }, "distance": 1200, "yaw": 30, "pitch": 15 } }
```

LookAt only orients the element (keeping its current position), while Orbit both positions and orients. Both reference their target via an ObjectRef property, which should use `"type": "weak"` to avoid keeping the target alive.

## Visual traits

Visuals implement `IVisual` and define how an element appears on screen. The renderer queries each element's visuals for draw commands during `render()`. An element can have multiple visuals (e.g. a background rect and a text label).

`IVisual` is the renderer-facing contract shared by every visual; it carries no authoring properties of its own. The authoring surface splits into two sub-interfaces that concrete visuals implement:

- **`IVisual2D`** adds `color`, `paint`, and `visual_phase`. 2D visuals fill the element's layout box with a color or material paint. Implemented by rect, rounded rect, text, image, and texture visuals.
- **`IVisual3D`** adds a `mesh` reference. 3D visuals draw one or more primitives from an `IMesh`; materials live on the primitives, not on the visual. Implemented by cube, sphere, and future mesh / glTF visuals.

| Trait | Interface | Description |
|-------|-----------|-------------|
| RectVisual | `IVisual2D` | Solid color rectangle filling element bounds |
| RoundedRectVisual | `IVisual2D` | Rounded rectangle with SDF corners |
| TextVisual | `IVisual2D`, `ITextVisual` | Shaped text rendered as glyph quads (text plugin) |
| CubeVisual | `IVisual3D`, `IPrimitiveShape`, `IAnalyticShape` | Procedural axis-aligned cube |
| SphereVisual | `IVisual3D`, `IPrimitiveShape`, `IAnalyticShape` | Procedural UV sphere |
| InstancedMeshVisual | `IVisual3D`, `IInstancedMeshVisual` | Meshes drawn many times on one element. See [instancing](../render/instancing.md) |

2D visuals are authored through the `color` and `paint` properties:

```cpp
auto rect = velk::ui::trait::visual::create_rect();
rect.set_color({0.9f, 0.2f, 0.2f, 1.f});

auto text = velk::ui::trait::visual::create_text();
text.set_font(font);
text.set_text("Hello!");
```

Assigning a material to `paint` overrides the default solid-color fill with a custom shader or gradient.

3D visuals are authored through a mesh reference; materials live on primitives. Procedural primitives lazily populate the mesh on first render, but most callers build it up-front so per-primitive materials can be set at authoring time:

```cpp
auto cube_vis = velk::trait::visual::create_cube();
velk::Mesh cube_mesh(ctx.build_cube());
cube_mesh.set_material(0, velk::material::create_standard(
    color{0.95f, 0.7f, 0.4f, 1.f}, /*metallic=*/0.9f, /*roughness=*/0.15f));
cube_vis.set_mesh(cube_mesh);
```

See [mesh](../render/mesh.md) for the container / primitive / buffer model and [materials](../render/materials.md) for the authoring options.

### Visual phase

2D visuals carry a `visual_phase` property (on `IVisual2D`) that controls when they draw relative to the element's children:

| Phase | Description |
|-------|-------------|
| `BeforeChildren` | Draws before children (default). Use for backgrounds, fills. |
| `AfterChildren` | Draws after children. Use for borders, overlays, focus rings. |

```cpp
auto border = velk::ui::trait::visual::create_rect();
border.set_color({1.f, 1.f, 1.f, 0.3f});
border.set_visual_phase(VisualPhase::AfterChildren);
elem.add_trait(border);
```

In JSON:

```json
{ "targets": ["panel"], "class": "velk-ui.RectVisual",
  "properties": { "visual_phase": "after_children", "color": { "r": 1, "g": 1, "b": 1, "a": 0.3 } } }
```

Multiple visuals on the same element are grouped by phase. All `BeforeChildren` visuals draw in attachment order before any child visuals, then all `AfterChildren` visuals draw in attachment order after all descendants.

3D visuals don't carry a phase; they depth-sort through the depth buffer rather than a painter's algorithm.

## Render traits

Render traits affect how the scene is observed, identified by `TraitPhase::Render`. Currently there is one such trait implemented by `ClassId::Camera` The renderer uses the camera's view-projection matrix to transform the scene for display on a surface.

| Trait | Interface | Description |
|-------|-----------|-------------|
| Camera | `ICamera` | Orthographic or perspective projection with zoom and scale |

The camera is attached to an element in the scene hierarchy. The element's world transform provides the camera position. The renderer binds a camera element to a surface via `add_view()`.

```cpp
auto camera_elem = velk::create_element();
camera_elem.add_trait(velk::trait::render::create_camera());
scene.add(scene.root(), camera_elem);

renderer->add_view(camera_elem, surface);
```

In JSON, the camera is defined as a trait on an element:

```json
{ "id": "camera", "class": "velk-scene.Element" }
```

```json
{ "targets": ["camera"], "class": "velk-scene.Camera", "properties": { "zoom": 1.0 } }
```

Camera properties:

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `projection` | `Projection` | `Ortho` | Projection type (`ortho` or `perspective`) |
| `zoom` | `float` | `1.0` | View zoom (2.0 = zoomed in, 0.5 = zoomed out) |
| `scale` | `float` | `1.0` | Render resolution relative to surface |
| `fov` | `float` | `60.0` | Vertical FOV in degrees (perspective only) |
| `near_clip` | `float` | `0.1` | Near clipping plane |
| `far_clip` | `float` | `1000.0` | Far clipping plane |

## Input traits

Input traits implement `IInputTrait` and handle pointer, scroll, and keyboard events. They are dispatched by the `InputDispatcher` using a hit-test + intercept + bubble model. See [Input](input.md) for the full dispatch pipeline.

| Trait | Interface | Description |
|-------|-----------|-------------|
| Click | `IClick` | Fires `on_click` on pointer down+up within bounds |
| Hover | `IHover` | Tracks `hovered` state, fires `on_hover_changed` |
| Drag | `IDrag` | Tracks drag gestures with start/move/end events |

```cpp
auto click = velk::ui::trait::input::create_click();
click.on_click().add_handler([]() { /* clicked */ });
elem.add_trait(click);

auto hover = velk::ui::trait::input::create_hover();
hover.on_hover_changed().add_handler([&]() {
    bool over = hover.is_hovered();
});
elem.add_trait(hover);
```

## Writing a custom trait

1. Define an interface with `VELK_INTERFACE` for any properties/events your trait exposes.
2. Add a `ClassId` constant in your plugin header.
3. Subclass the appropriate `ext::` base.
4. Register the type in your plugin's `initialize()`.

```cpp
// Interface
class IMyTrait : public velk::Interface<IMyTrait> {
public:
    VELK_INTERFACE(
        (PROP, float, threshold, 0.5f),
        (EVT, on_triggered)
    )
};

// Implementation
class MyTrait : public velk::ui::ext::Input<MyTrait, IMyTrait> {
public:
    VELK_CLASS_UID(ClassId::MyTrait, "MyTrait");

    InputResult on_pointer_event(PointerEvent& event) override {
        // custom logic...
        return InputResult::Ignored;
    }
};
```

## Built-in trait classes

ClassIds for the traits velk-ui ships out of the box, organized by phase. Input traits are documented separately in [Input](input.md).

### Constraint phase

Constraints refine an element's size during layout solving. Multiple can be attached to the same element.

| ClassId | Implements | Description |
|---|---|---|
| `velk::ui::ClassId::Constraint::Stack` | `ILayoutTrait` | Lays out children along a single axis with optional spacing. Properties: `axis`, `spacing`, `h_align`, `v_align`. |
| `velk::ui::ClassId::Constraint::ScrollView` | `ILayoutTrait`, `IScrollView`, `IClip`, `IInputTrait`, `IVisual2D` | Scrolls and clips its children, with scroll bars. Properties: `scroll_x`, `scroll_y`, `bar_width`, `wheel_step`. See [Scroll view](#scroll-view). |
| `velk::ClassId::Clip` | `IClip` | Clips an element and its descendants to its box. See [Clipping](#clipping). |
| `velk::ui::ClassId::Constraint::FixedSize` | `ILayoutTrait` | Clamps an element to a fixed `width` and/or `height`. Either dimension can be left unset to inherit from a parent layout. |

### Transform phase

Transforms run after layout has assigned the base world matrix. Each transform trait modifies the element's `world_matrix` in place.

| ClassId | Implements | Description |
|---|---|---|
| `velk::ui::ClassId::Transform::Trs` | `ITransformTrait` | Decomposed translate / rotate (Z) / scale. Convenient for animation. |
| `velk::ui::ClassId::Transform::Matrix` | `ITransformTrait` | Raw 4×4 matrix transform. Use when the decomposed form isn't enough. |
| `velk::ui::ClassId::Transform::LookAt` | `ITransformTrait` | Orients the element to face a target element. |
| `velk::ui::ClassId::Transform::Orbit` | `ITransformTrait` | Positions and orients the element on a sphere around a target. Properties: `target`, `distance`, `yaw`, `pitch`. Used for orbit cameras. |

### Visual phase

Visuals produce draw entries. The renderer queries each element's visuals during `prepare()`.

| ClassId | Implements | Description |
|---|---|---|
| `velk::ui::ClassId::Visual::Rect` | `IVisual2D` | Solid color rectangle filling the element bounds. |
| `velk::ui::ClassId::Visual::RoundedRect` | `IVisual2D` | Rounded rectangle with SDF corners. Properties: `corner_radius`. |
| `velk::ui::ClassId::Visual::Texture` | `IVisual2D`, `ITextureVisual` | Texture sampler filling the element bounds. |
| `velk::ui::ClassId::Visual::Cube` | `IVisual3D`, `IPrimitiveShape`, `IAnalyticShape` | Procedural axis-aligned cube. |
| `velk::ui::ClassId::Visual::Sphere` | `IVisual3D`, `IPrimitiveShape`, `IAnalyticShape` | Procedural UV sphere. |
| `velk::ui::ClassId::Visual::InstancedMesh` | `IVisual3D`, `IInstancedMeshVisual` | Meshes drawn many times on one element. |

Plugin-provided visuals: `velk::ui::ClassId::Visual::Text` (text plugin), `velk::ui::ClassId::Visual::Image` (image plugin) — see the plugin docs.

### Material classes

How a material attaches depends on the visual kind. 2D visuals reference a single material through `IVisual2D::paint`; setting paint overrides the solid-color fill defined by `color`. 3D visuals carry no paint slot — materials live on `IMeshPrimitive::material`, one per primitive — so a multi-primitive mesh can mix materials. See [materials](../render/materials.md) for both paths.

| ClassId | Implements | Description |
|---|---|---|
| `velk::ui::ClassId::Material::Gradient` | `IMaterial` | Built-in linear gradient material. Properties: `start_color`, `end_color`, `angle`. |

Plugin-provided materials: `velk::ui::ClassId::Material::Image`, `velk::ui::ClassId::Material::Environment` (image plugin), `velk::ui::ClassId::TextMaterial` (text plugin). For arbitrary GLSL, use `velk::ClassId::ShaderMaterial` from velk-render — see [Materials](../render/materials.md).

### Camera

| ClassId | Implements | Description |
|---|---|---|
| `velk::ui::ClassId::Render::Camera` | `ICamera` | Camera trait. Defines projection (ortho or perspective), zoom, scale, fov, near/far clip, and the optional environment. Attached to a camera element that the renderer references via `add_view()`. |
