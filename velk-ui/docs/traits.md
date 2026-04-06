# Traits

Traits are the primary extension mechanism in velk-ui. An element on its own has only position, size, and z-index. Everything else (layout, appearance, input handling, transforms) comes from traits attached to it.

## Why traits

A traditional UI framework bakes behavior into a class hierarchy: `Button` inherits `Widget`, `ScrollView` inherits `Container`, etc. This creates rigid trees where adding a new combination of behaviors means creating a new class.

velk-ui uses composition instead. An element is a blank container. Attach a `RectVisual` to give it color. Attach a `Stack` to make it lay out its children. Attach a `Click` to make it respond to pointer events. These can be mixed freely at runtime without subclassing.

This maps directly to velk's object model: traits are velk objects attached to an element via `IObjectStorage`. They're discovered by interface (`ILayoutTrait`, `IVisual`, `IInputTrait`) so the solver, renderer, and input dispatcher each find only the traits they care about.

## Trait phases

Every trait implements `ITrait` and reports which phases of the element pipeline it participates in via `get_phase()`. The return value is a bitmask, so a single trait can participate in multiple phases.

| Phase | Purpose | Typical interface |
|-------|---------|-------------------|
| **Layout** | Walks children, divides space | `ILayoutTrait` |
| **Constraint** | Refines own size | `ILayoutTrait` |
| **Transform** | Modifies the world matrix | `ITransformTrait` |
| **Visual** | Produces draw commands | `IVisual` |
| **Input** | Handles pointer, scroll, key events | `IInputTrait` |
| **Render** | Defines how the scene is observed | `ICamera` |

The phase determines *when* the trait runs, not *what interface* it must implement. For example, `ILayoutTrait` is used by both Layout and Constraint phases; a Stack trait returns `TraitPhase::Layout` while a FixedSize trait returns `TraitPhase::Constraint`. A trait could return both (`TraitPhase::Layout | TraitPhase::Constraint`) to participate in both phases.

Layout, Constraint, and Transform run inside the layout solver during `Scene::update()`. Visual and Render run during `renderer->render()`. Input runs synchronously when the input dispatcher receives platform events. See [Update cycle](update-cycle.md) for the full frame flow.

## Attaching traits

Traits are managed via `add_trait()` / `remove_trait()` on the Element wrapper:

```cpp
auto elem = velk::ui::create_element();

auto stack = velk::ui::constraint::create_stack();
auto rect = velk::ui::visual::create_rect();
auto click = velk::ui::input::create_click();

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
| `ext::Layout<T, Phase, ...>` | Layout or Constraint | No-op measure/apply |
| `ext::Transform<T, ...>` | Transform | No-op transform |
| `ext::Visual<T, ...>` | Visual | Fires `on_visual_changed` on any property change |
| `ext::Input<T, ...>` | Input | All handlers return `Ignored` |
| `ext::Render<T, ...>` | Render | Base for render-phase traits (e.g. Camera) |

Subclass the appropriate base and override only what you need.

## Layout traits

Layout traits implement `ILayoutTrait` and control how elements are sized and positioned. The solver calls `measure()` in a top-down pass to compute desired sizes, then `apply()` to write final positions.

| Trait | Interface | Phase | Description |
|-------|-----------|-------|-------------|
| Stack | `IStack` | Layout | Arranges children along an axis with spacing |
| FixedSize | `IFixedSize` | Constraint | Clamps width and/or height to a fixed value |

```cpp
auto stack = velk::ui::constraint::create_stack();
stack.set_axis(1);       // vertical
stack.set_spacing(10.f);

auto fs = velk::ui::constraint::create_fixed_size();
fs.set_size(velk::ui::dim::px(200.f), velk::ui::dim::px(100.f));
```

Dimensions can be absolute (`dim::px(100.f)`) or relative to parent (`dim::pct(0.5f)`). Use `dim::none()` to leave an axis unconstrained.

## Transform traits

Transform traits implement `ITransformTrait` and modify the element's world matrix after layout, before children are recursed.

| Trait | Interface | Description |
|-------|-----------|-------------|
| Trs | `ITrs` | Decomposed translate, rotate (Z), scale |
| Matrix | `IMatrix` | Raw 4x4 matrix multiply |
| LookAt | `ILookAt` | Orients the element to face a target element |
| Orbit | `IOrbit` | Positions and orients the element on a sphere around a target |

```cpp
auto trs = velk::ui::transform::create_trs();
trs.set_rotation(45.f);         // degrees around Z
trs.set_scale({0.5f, 0.5f});
elem.add_trait(trs);
```

LookAt and Orbit are particularly useful for cameras. Orbit positions the element at a given distance, yaw (horizontal angle), and pitch (vertical angle) from the target element's center, and orients it to face the target:

```cpp
auto orbit = velk::ui::transform::create_orbit();
orbit.set_target(scene.root());
orbit.set_distance(1200.f);
orbit.set_yaw(30.f);    // degrees
orbit.set_pitch(15.f);  // degrees
camera_element.add_trait(orbit);
```

In JSON:

```json
{ "targets": ["camera_3d"], "class": "velk-ui.Orbit",
  "properties": { "target": { "ref": "root", "type": "weak" }, "distance": 1200, "yaw": 30, "pitch": 15 } }
```

LookAt only orients the element (keeping its current position), while Orbit both positions and orients. Both reference their target via an ObjectRef property, which should use `"type": "weak"` to avoid keeping the target alive.

## Visual traits

Visuals implement `IVisual` and define how an element appears on screen. The renderer queries each element's visuals for draw commands during `render()`. An element can have multiple visuals (e.g. a background rect and a text label).

| Trait | Interface | Description |
|-------|-----------|-------------|
| RectVisual | `IVisual` | Solid color rectangle filling element bounds |
| RoundedRectVisual | `IVisual` | Rounded rectangle with SDF corners |
| TextVisual | `ITextVisual` | Shaped text rendered as glyph quads (text plugin) |

```cpp
auto rect = velk::ui::visual::create_rect();
rect.set_color({0.9f, 0.2f, 0.2f, 1.f});

auto text = velk::ui::visual::create_text();
text.set_font(font);
text.set_text("Hello!");
```

Visuals can optionally reference an `IMaterial` (via the `paint` property) to override the default fill with a custom shader or gradient.

### Visual phase

Each visual has a `visual_phase` property that controls when it draws relative to the element's children:

| Phase | Description |
|-------|-------------|
| `BeforeChildren` | Draws before children (default). Use for backgrounds, fills. |
| `AfterChildren` | Draws after children. Use for borders, overlays, focus rings. |

```cpp
auto border = velk::ui::visual::create_rect();
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

## Render traits

Render traits affect how the scene is observed, identified by `TraitPhase::Render`. Currently there is one such trait implemented by `ClassId::Camera` The renderer uses the camera's view-projection matrix to transform the scene for display on a surface.

| Trait | Interface | Description |
|-------|-----------|-------------|
| Camera | `ICamera` | Orthographic or perspective projection with zoom and scale |

The camera is attached to an element in the scene hierarchy. The element's world transform provides the camera position. The renderer binds a camera element to a surface via `add_view()`.

```cpp
auto camera_elem = velk::ui::create_element();
camera_elem.add_trait(velk::ui::create_camera());
scene.add(scene.root(), camera_elem);

renderer->add_view(camera_elem, surface);
```

In JSON, the camera is defined as a trait on an element:

```json
{ "id": "camera", "class": "velk-ui.Element" }
```

```json
{ "targets": ["camera"], "class": "velk-ui.Camera", "properties": { "zoom": 1.0 } }
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
auto click = velk::ui::input::create_click();
click.on_click().add_handler([]() { /* clicked */ });
elem.add_trait(click);

auto hover = velk::ui::input::create_hover();
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
