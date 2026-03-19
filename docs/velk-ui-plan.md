# velk-ui Plan

## Overall sequencing

| Step | Status |
|------|--------|
| 1. Importer plugin (velk repo) | **DONE** (merged, 710 tests) |
| 2. velk-ui repo | **In progress** |
| 3. Editor | Deferred |

## Step 2: velk-ui

### Done

- **Repo scaffolding**: CMakeLists.txt, README, .gitignore
- **Core UI library** (`velk-ui/`): `IRenderer`, `IElement`, `Element` plugin
- **GL renderer plugin** (`plugins/render/gl/`): OpenGL backend implementing `IRenderer`
- **Test app** (`app/`): GLFW window, loads plugins, imports a scene, runs render loop
- **Third-party deps**: GLAD 2, GLFW 3.4 vendored
- **Math types** (velk repo): `mat4` added with column-major layout, identity default, multiply, translate/scale factories. Operators and named constants (`zero()`, `one()`, `identity()`, etc.) on all math types
- **IElement updated**: `position` (vec3), `size` (velk::size), `color` (velk::color), `local_transform` (mat4), `world_matrix` (RPROP mat4), `z_index` (int32_t)
- **Importer struct support** (velk repo): JSON importer deserializes vec2/vec3/vec4/size/rect/color from both arrays and named-field objects. Color alpha defaults to 1.0 if omitted
- **3D size type** (velk repo): `velk::size` now has width, height, depth

### Layout model

Single `Element` type (poolable/hiveable), behavior defined by attachments.

**Defaults**: elements fill their parent. Siblings overlap (no implicit layout).

**Transforms**: each element has two 4x4 matrices:
- **World matrix**: computed by the solver during `apply`. Accumulates parent chain positioning (`parent.world * local`). For the common case (no rotation/scale) this is just a translation.
- **Local matrix** ("transform"): user-specified offset/rotation/scale, applied on top of layout. Does not affect layout of siblings or parent.

Constraints (Stack, Grid, etc.) work in parent-local space — they set each child's position relative to the parent origin. The solver accumulates world matrices as it recurses: `child.world = parent.world * child_layout_position * child.local_transform`. The renderer reads the final world matrix.

**Attachments**: velk already supports attaching any object to any object via `IObjectStorage`. The layout system finds `IConstraint` attachments on each element.

**`Constraint` struct** (in `velk_ui` namespace):

```cpp
struct Constraint
{
    velk::aabb available;
};
```

**Single interface** `IConstraint` (using `velk::aabb`, `velk::size` from `velk/api/math_types.h`):

```cpp
class IConstraint : public velk::Interface<IConstraint>
{
public:
    enum class Phase { Layout, Constraint };

    virtual Phase get_phase() const = 0;
    virtual Constraint measure(const Constraint& c, IElement& element) = 0;
    virtual void apply(const Constraint& c, IElement& element) = 0;
};
```

**dim type** (DLL-safe POD):

```cpp
struct dim
{
    enum class unit { none, px, pct };

    float value = 1.f;
    unit type = unit::pct;

    static constexpr dim none() { return {0.f, unit::none}; }
    static constexpr dim fill() { return {1.f, unit::pct}; }
    static constexpr dim zero() { return {0.f, unit::px}; }
};
```

- `none`: not specified — the constraint skips this axis. `dim::none()` is the "don't touch" sentinel
- `px`: absolute pixels, resolved as-is. Unclamped — negative values extend in the opposite direction
- `pct`: fraction of available space in that axis, resolved during `measure`. Unclamped — 2.0 = 200%, -1.0 = mirrored/opposite direction
- `em`: deferred until text shaping lands (will resolve relative to font size)
- Element default is `{1.f, pct}` (fill parent, i.e. 100%). Individual constraint properties can default to `dim::none()` meaning "don't constrain this axis"

The solver resolves `dim` values during `measure` using `Constraint::available`:
- `px` -> value as-is
- `pct` -> `value * available_extent`

JSON still uses human-readable `"50%"` — the parser converts to 0.5 at import time.

**Unbounded space**: infinity lives in the `Constraint::available` aabb, not in dim. A scroll container passes `FLT_MAX` on its scroll axis. When `Pct` resolves against `FLT_MAX`, the result is `FLT_MAX`, meaning "no size imposed, let content decide." An element with no content and no explicit size in an unbounded axis is zero-sized. dim itself never represents infinity — it's always a concrete value+unit.

dims in JSON: `"10px"`, `"50%"`, or bare number (defaults to `px`).

**Ordering**: the solver collects all `IConstraint` attachments on an element, sorts by phase (`Layout` first, then `Constraint`), and within the same phase runs them in attachment order. This gives a predictable pipeline without rigid sub-priorities.

**Two-phase solve**: `measure` computes desired size, `apply` writes final bounds into element state via `velk::write_state<IElement>()`. Constraints that need multiple passes over children (e.g. Stack measuring all children before positioning them) do so internally within their own `measure`/`apply`. If a common multi-pass pattern emerges across constraints, it can be generalized into the solver. The difference between a constraint and a layout is just scope:

Layout phase (touches self and children):
- `Stack`: walks children via hierarchy, divides space along axis (vertical/horizontal). Can shrink to fit children or use explicit size
- `Grid`: arranges children in rows and columns (future). Same shrink-to-fit option
- `FitContent`: wraps the element's bounding box around its children. For elements without a layout like Stack/Grid — just "be as big as my children need." Measures children, takes their bounding box as own size

Constraint phase (touches self only):
- `FixedSize`: clamps to a `dim` width/height (e.g. `"200px"`, `"50%"`). Either axis can be `None` to leave it untouched
- `Margin`: shrinks available rect by `dim` left/top/right/bottom margins
- `Padding`: shrinks available rect for children by `dim` left/top/right/bottom. Like margin but inward — the element's own size is unchanged, but children see reduced space
- `Alignment`: positions within available space (horizontal, vertical)
- `MinMax`: enforces `dim` min/max width/height bounds

`FitContent` runs in the Layout phase so its result can be further constrained by Constraint-phase entries (e.g. `MinMax` clamping the fitted size).

The solver finds `IConstraint` on each element. One interface, the solver doesn't care what kind of constraint it's dealing with. `Constraint` struct is extensible for future fields.

**Constraint-specific interfaces**: many constraints also implement their own interface (e.g. `IFixedSize`, `IStack`) exposing configuration properties. `IConstraint` is the common interface the solver uses; the constraint-specific interface is how the user or importer configures it.

**Overflow**: children are allowed to overflow their parent's bounds. This is intentional — e.g. a list item with `scale=1.5` transform must visually exceed the parent. Clipping is an opt-in rendering concern, not a layout concern.

**Visual tree vs logical tree**: velk supports the same objects in multiple hierarchies. The layout system uses the **logical tree** (parent/child ownership, event routing). A separate **visual tree** controls draw order and z-ordering. Most of the time the visual tree mirrors the logical tree, but they can diverge when z-index overrides are present.

**Z-index**: IElement has a `z_index` property (int, default 0). Within a parent, children are drawn sorted by z-index, with child-list order as tiebreaker. This avoids manual tree manipulation — e.g. a selected list item just sets `z_index = 1` to draw above siblings. When the element's z_index changes, it calls `hierarchy->notify_dirty(DirtyFlags::ZOrder)`, which triggers a re-sort of that parent's children in the visual tree. No full tree rebuild needed. Z-index can be animated or data-bound like any property.

**Key classes**:

- **Scene** (implements `IScene`): owns the logical tree (a `velk::Hierarchy`), forwards `IHierarchy` methods to it. Maintains the dirty list. Runs the layout solver on layout-dirty elements. Does not know about rendering.

```cpp
class IScene : public velk::Interface<IScene, velk::IHierarchy>
{
public:
    virtual void notify_dirty(IElement& element, DirtyFlags flags) = 0;
};
```

- **VisualTree**: maintains a z-sorted mirror of the logical tree. Listens to dirty notifications from Scene. Produces incremental changesets (add/remove/update) for the renderer. Handles z-order re-sorting, and future optimizations (culling, occlusion).

- **LayoutSolver**: internal to Scene. Runs measure/apply passes and computes world matrices in a single top-down recursion.

- **Renderer**: consumes changesets from VisualTree. Manages GPU buffers. No tree or element knowledge — just render data.

**Frame loop**:
1. Scene processes layout-dirty elements → runs solver → elements have updated world matrices/sizes
2. VisualTree processes dirty list → diffs against current state → produces changeset (add/remove/update visuals, z-order re-sorts)
3. Renderer applies changeset to GPU buffers → draws

The Scene subscribes to the logical hierarchy's modification events (add/remove). When an element is added, the tree calls into the element (e.g. `IElement::attached(IScene*)`) so the element stores a reference to the tree. When a property changes, the element calls `tree->notify_dirty(*this, flags)` to put itself on the dirty list.

**Dirty flags**:

```cpp
enum class DirtyFlags : uint8_t
{
    Layout  = 1 << 0,   // size/position/constraint changed, needs relayout
    Visual  = 1 << 1,   // color/opacity changed, needs redraw
    ZOrder  = 1 << 2    // z_index changed, needs z-resort
};
```

The frame loop checks the dirty list: if any element has `Layout`, run the solver first, then pass all dirty elements to VisualTree for changeset generation.

**Change detection**: Element implements `IMetadataObserver`, which provides an `on_property_changed(IProperty&)` callback fired whenever any property on the object changes. From this callback, Element calls `scene->notify_dirty(*this, flags)` to put itself on the dirty list. No per-property event subscriptions needed.

**JSON and import**: constraints live in a dedicated `"ui-constraints"` section (or similar) in the scene JSON. The velk-ui plugin implements `IImporterExtension` to handle this section, creating constraint objects and attaching them to their target elements.

**Constraint-specific interfaces** (all use `VELK_INTERFACE` for animatability/binding):

`IStack`:

```cpp
class IStack : public velk::Interface<IStack>
{
public:
    VELK_INTERFACE(
        (PROP, uint8_t, axis, 1),       // 0 = horizontal, 1 = vertical
        (PROP, float, spacing, 0.f)     // gap between children in px
    )
};
```

`IFixedSize`:

```cpp
class IFixedSize : public velk::Interface<IFixedSize>
{
public:
    VELK_INTERFACE(
        (PROP, dim, width, dim::none()),
        (PROP, dim, height, dim::none())
    )
};
```

`IMargin`:

```cpp
class IMargin : public velk::Interface<IMargin>
{
public:
    VELK_INTERFACE(
        (PROP, dim, left, dim::none()),
        (PROP, dim, top, dim::none()),
        (PROP, dim, right, dim::none()),
        (PROP, dim, bottom, dim::none())
    )
};
```

**IElement** (already implemented):
- `position` (`PROP vec3`): layout position in parent-local space, set by the solver
- `size` (`PROP velk::size`): layout dimensions (width, height, depth), set by the solver or constraints
- `color` (`PROP velk::color`): RGBA color
- `local_transform` (`PROP mat4`, identity default): user-specified offset/rotation/scale, does not affect layout
- `world_matrix` (`RPROP mat4`, identity default): computed by solver = `parent.world * translate(position) * local_transform`. Read-only; the solver writes it via `write_state`
- `z_index` (`PROP int32_t`, default 0): draw order among siblings

The renderer reads `world_matrix` + `size` for positioning and sizing.

**Iterating constraints**: `IObjectStorage::find_attachment<IConstraint>()` returns a single match. To collect all `IConstraint` attachments, iterate via `attachment_count()` / `get_attachment(i)` and `interface_cast<IConstraint>()` each one.

**JSON format for constraints** (`"ui-constraints"` section, handled by `IImporterExtension`):

```json
{
    "version": 1,
    "objects": [
        { "id": "root", "class": "velk-ui.Element" },
        { "id": "child1", "class": "velk-ui.Element",
          "properties": { "color": { "r": 1.0, "g": 0.2, "b": 0.2 } } },
        { "id": "child2", "class": "velk-ui.Element",
          "properties": { "color": { "r": 0.2, "g": 0.3, "b": 0.9 } } },
        { "id": "child3", "class": "velk-ui.Element",
          "properties": { "color": { "r": 0.1, "g": 0.8, "b": 0.2 } } }
    ],
    "hierarchies": {
        "scene": {
            "root": ["child1", "child2", "child3"]
        }
    },
    "ui-constraints": [
        { "target": "root", "type": "stack", "axis": "vertical", "spacing": 10 },
        { "target": "child1", "type": "fixed-size", "height": "100px" },
        { "target": "child2", "type": "fixed-size", "height": "150px" },
        { "target": "child2", "type": "margin", "left": "20px", "right": "20px" },
        { "target": "child3", "type": "fixed-size", "height": "100px" }
    ]
}
```

The extension parses dimension strings (`"100px"`, `"50%"`, bare number = px) into value+unit and sets them on the constraint via its specific interface (IFixedSize, IMargin, etc.).

**App flow with Scene**:
1. Import JSON -> objects + hierarchy + constraints (via importer extension)
2. Create Scene, populate hierarchy from import result
3. Each frame: `scene.solve_layout(viewport_aabb)` -> updates element positions/sizes and world matrices
4. Pass elements to renderer (add_visual on first frame, dirty tracking handles updates)
5. Render loop: `scene.solve_layout(...)`, `renderer.render()`

**Implementation rounds** (incremental, each ending with something testable):

Round 1: **Foundation types** ~~DONE~~
- `mat4`, `aabb` in velk `math_types.h` with operators, translate/scale factories, min/max accessors
- Math type operators and named constants on all types
- `velk::size` extended to 3D (width, height, depth)
- `IElement` updated: position (vec3), size, color, local_transform, world_matrix, z_index
- Importer: struct type deserialization from JSON objects and arrays
- GL renderer updated for new IElement layout

Round 2: **Scene + Constraints + Solver + VisualTree + Importer**
- `dim`, `Constraint` structs in velk-ui; `register_type<dim>()` in plugin init
- `IConstraint` interface
- `DirtyFlags` enum (Layout, Visual, ZOrder)
- `IScene` interface + `Scene` class (owns `velk::Hierarchy`, dirty list)
- `VisualTree`: z-sorted mirror of logical tree, produces changesets for renderer
- `IStack`, `IFixedSize`, `IMargin` interfaces (VELK_INTERFACE)
- `Stack`, `FixedSize`, `Margin` implementations (each implements both `IConstraint` and its specific interface)
- `LayoutSolver` (internal to Scene): top-down recursion, measure/apply, world matrix accumulation
- `IImporterExtension` for `"ui-constraints"` JSON section
- Register all new types in velk-ui plugin
- Testable: import a scene with constraints, run solver, verify element positions/world matrices are correct

Round 3: **Renderer integration + Demo**
- Update `GlRenderer` to consume VisualTree changesets and read `world_matrix` for positioning
- Update test app to create Scene and use the new flow
- New test scene JSON: vertical stack of three colored elements
- Testable: visible vertical stack rendered on screen

### Ahead

- **2D renderer features**: instanced quads, rounded rects, gradients, clipping, custom shaders
- **Text shaping**: freetype + harfbuzz
- **Style system**: TBD
- **Event/input model**: hit testing, focus, keyboard/mouse
- **Vulkan backend**: plugins/render/vk/

## Step 3: Editor (deferred)

- Editor for creating UI scene files in velk serialization format
- Could self-host with velk-ui or start with Dear ImGui
- Web target via Emscripten is a goal
- Exporter lives here (needs editorial intent: dirty tracking, default omission)

## Guiding principle

Keep velk (with plugins) small and embeddable. Heavy dependencies (renderer, text shaping, windowing) go in velk-ui, not velk.
