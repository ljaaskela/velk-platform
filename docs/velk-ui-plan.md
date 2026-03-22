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
- **IElement updated**: `position` (vec3), `size` (velk::size), `color` (velk::color), `local_transform` (RPROP mat4), `world_matrix` (RPROP mat4), `z_index` (int32_t)
- **Importer struct support** (velk repo): JSON importer deserializes vec2/vec3/vec4/size/rect/color from both arrays and named-field objects. Color alpha defaults to 1.0 if omitted
- **3D size type** (velk repo): `velk::size` now has width, height, depth
- **Round 2a done**: Scene, Stack, FixedSize, LayoutSolver, constraint import handler, dirty tracking, renderer integration
- **Text plugin**: FreeType + HarfBuzz integration, Font class, GlyphAtlas
- **Round 2a.5 done**: Visual representation model, text rendering

### Layout model

Single `Element` type (poolable/hiveable), behavior defined by attachments.

**Defaults**: elements fill their parent. Siblings overlap (no implicit layout).

**Transforms**: each element has two 4x4 matrices:
- **World matrix**: computed by the solver during `apply`. Accumulates parent chain positioning (`parent.world * local`). For the common case (no rotation/scale) this is just a translation.
- **Local matrix** ("transform"): user-specified offset/rotation/scale, applied on top of layout. Does not affect layout of siblings or parent. Read-only (RPROP); written by the solver or future transform constraints.

Constraints (Stack, Grid, etc.) work in parent-local space -- they set each child's position relative to the parent origin. The solver accumulates world matrices as it recurses: `child.world = parent.world * child_layout_position * child.local_transform`. The renderer reads the final world matrix.

**Attachments**: velk already supports attaching any object to any object via `IObjectStorage`. The layout system finds `IConstraint` attachments on each element.

**`Constraint` struct** (in `velk_ui` namespace):

```cpp
struct Constraint
{
    velk::aabb bounds;
};
```

Input: "here's your available space." Output: "here's what I'm using." Kept as a struct (not bare `aabb`) for future extensibility (e.g. adjustment flags).

**Single interface** `IConstraint` (using `velk::aabb`, `velk::size` from `velk/api/math_types.h`):

```cpp
class IConstraint : public velk::Interface<IConstraint>
{
public:
    virtual ConstraintPhase get_phase() const = 0;
    virtual Constraint measure(const Constraint& c, IElement& element, velk::IHierarchy* hierarchy) = 0;
    virtual void apply(const Constraint& c, IElement& element, velk::IHierarchy* hierarchy) = 0;
};
```

Layout-phase constraints receive the hierarchy so they can walk children; constraint-phase constraints ignore it.

**dim type** (DLL-safe POD):

```cpp
struct dim
{
    float value{};
    DimUnit unit{DimUnit::None};

    static constexpr dim none() { return {0.f, DimUnit::None}; }
    static constexpr dim fill() { return {100.f, DimUnit::Pct}; }
    static constexpr dim zero() { return {0.f, DimUnit::Px}; }
    static constexpr dim px(float v) { return {v, DimUnit::Px}; }
    static constexpr dim pct(float v) { return {v, DimUnit::Pct}; }
};
```

- `none`: not specified -- the constraint skips this axis. `dim::none()` is the "don't touch" sentinel
- `px`: absolute pixels, resolved as-is. Unclamped -- negative values extend in the opposite direction
- `pct`: fraction of available space in that axis, resolved during `measure`. Unclamped -- 2.0 = 200%, -1.0 = mirrored/opposite direction
- `em`: deferred until text shaping lands (will resolve relative to font size)
- Element default is `{1.f, pct}` (fill parent, i.e. 100%). Individual constraint properties can default to `dim::none()` meaning "don't constrain this axis"

The solver resolves `dim` values during `measure` using `Constraint::bounds`:
- `px` -> value as-is
- `pct` -> `value * available_extent`

JSON still uses human-readable `"50%"` -- the parser converts to 0.5 at import time.

**Unbounded space**: infinity lives in the `Constraint::bounds` aabb, not in dim. A scroll container passes `FLT_MAX` on its scroll axis. When `Pct` resolves against `FLT_MAX`, the result is `FLT_MAX`, meaning "no size imposed, let content decide." An element with no content and no explicit size in an unbounded axis is zero-sized. dim itself never represents infinity -- it's always a concrete value+unit.

dims in JSON: `"10px"`, `"50%"`, or bare number (defaults to `px`).

**Ordering**: the solver collects all `IConstraint` attachments on an element, sorts by phase (`Layout` first, then `Constraint`), and within the same phase runs them in attachment order. This gives a predictable pipeline without rigid sub-priorities.

**Two-phase solve**: `measure` computes desired size, `apply` writes final bounds into element state via `velk::write_state<IElement>()`. Constraints that need multiple passes over children (e.g. Stack measuring all children before positioning them) do so internally within their own `measure`/`apply`. If a common multi-pass pattern emerges across constraints, it can be generalized into the solver. The difference between a constraint and a layout is just scope:

Layout phase (touches self and children):
- `Stack`: walks children via hierarchy, divides space along axis (vertical/horizontal). Fills parent by default (like all elements). Children fill their allocated slot. May run multiple measure passes internally to stabilize sizes (e.g. redistribute freed space from fixed-size children)
- `Grid`: arranges children in rows and columns (future). Fills parent by default
- `FitContent`: opt-in shrink-wrap. Measures children, takes their bounding box as own size. For elements without a layout like Stack/Grid -- just "be as big as my children need"

Constraint phase (touches self only):
- `FixedSize`: clamps to a `dim` width/height (e.g. `"200px"`, `"50%"`). Either axis can be `None` to leave it untouched
- `Margin`: shrinks available rect by `dim` left/top/right/bottom margins
- `Padding`: shrinks available rect for children by `dim` left/top/right/bottom. Like margin but inward -- the element's own size is unchanged, but children see reduced space
- `Alignment`: positions within available space (horizontal, vertical)
- `MinMax`: enforces `dim` min/max width/height bounds

`FitContent` runs in the Layout phase so its result can be further constrained by Constraint-phase entries (e.g. `MinMax` clamping the fitted size).

The solver finds `IConstraint` on each element. One interface, the solver doesn't care what kind of constraint it's dealing with. `Constraint` struct is extensible for future fields.

**Constraint-specific interfaces**: many constraints also implement their own interface (e.g. `IFixedSize`, `IStack`) exposing configuration properties. `IConstraint` is the common interface the solver uses; the constraint-specific interface is how the user or importer configures it.

**Overflow**: children are allowed to overflow their parent's bounds. This is intentional -- e.g. a list item with `scale=1.5` transform must visually exceed the parent. Clipping is an opt-in rendering concern, not a layout concern.

**Visual tree vs logical tree**: Scene maintains a z-sorted `visual_list_` derived from the logical tree. The layout system uses the **logical tree** (parent/child ownership, event routing). The visual list controls draw order. Most of the time the visual list mirrors the logical tree order, but they diverge when z-index overrides are present.

**Z-index**: IElement has a `z_index` property (int, default 0). Within a parent, children are drawn sorted by z-index, with child-list order as tiebreaker. This avoids manual tree manipulation -- e.g. a selected list item just sets `z_index = 1` to draw above siblings. When the element's z_index changes, Element marks `DirtyFlags::ZOrder`, which triggers a rebuild of the visual list. Z-index can be animated or data-bound like any property.

**Key classes**:

- **Scene** (implements `IScene`): looks like an `IHierarchy` to the outside. Internally owns `logical_` hierarchy. Forwards `IHierarchy` methods to it (using `velk::Hierarchy` wrapper where possible, raw `IHierarchy*` for methods returning `IObject::Ptr`). Maintains the dirty list. Runs the layout solver only when `DirtyFlags::Layout` is present. Pushes visual changes to the renderer.

```cpp
class IScene : public velk::Interface<IScene, velk::IHierarchy>
{
public:
    virtual void load(velk::IStore& store) = 0;
    virtual void set_renderer(IRenderer* renderer) = 0;
    virtual void set_viewport(const velk::aabb& viewport) = 0;
    virtual void update() = 0;
    virtual void notify_dirty(IElement& element, DirtyFlags flags) = 0;
    virtual velk::array_view<IElement*> get_visual_list() = 0;
};
```

- **ISceneObserver**: interface that Element implements instead of `IHierarchyAware`. Scene calls these when it adds/removes elements from the hierarchy. Element stores a raw `IScene*` (lifetime guaranteed while attached) for dirty notifications. On attach, Element marks itself `Layout | Visual` dirty so the solver runs on the first frame.

```cpp
class ISceneObserver : public velk::Interface<ISceneObserver>
{
public:
    virtual void on_attached(IScene& scene) = 0;
    virtual void on_detached(IScene& scene) = 0;
};
```

- **Scene frame data**: two flat vectors maintained by Scene:
  - `visual_list_` (`vector<IElement*>`): all elements in draw order, rebuilt by z-sorted traversal when ZOrder or hierarchy changes
  - `dirty_elements_` (`vector<IElement*>`): elements with any `DirtyFlags` since last frame (each element notifies only once per frame via the dirty accumulator). All dirty elements are pushed to the renderer via `update_visuals`, not just Visual-dirty ones, because draw commands depend on element size which changes during layout

- **LayoutSolver**: internal to Scene. Runs measure/apply passes and computes world matrices in a single top-down recursion. Only invoked when at least one element has `DirtyFlags::Layout`.

- **Renderer**: `IRenderer::add_visual(const IElement::Ptr&)` / `remove_visual(VisualId)` for element enter/leave. `update_visuals(velk::array_view<IElement*> changed)` called by Scene each frame with all dirty elements. Renderer rebuilds draw command cache for changed elements and re-uploads GPU data. Two pipelines: untextured FillRect and textured TexturedQuad. No tree knowledge, no per-property subscriptions.

**Frame loop**:
1. Scene processes dirty list: consumes flags from all dirty elements, collects them as changes
2. If any element has `Layout`, run solver (top-down recursion, measure/apply, world matrix accumulation)
3. If `ZOrder` or hierarchy changed, rebuild `visual_list_`
4. Push all dirty elements to renderer via `update_visuals`
5. Renderer rebuilds draw command cache for changed elements, builds instance buffers from cache + world_matrix, uploads and draws

When Scene adds/removes an element, it calls `interface_cast<ISceneObserver>(element)->on_attached(*this)` (or `on_detached`). The element stores the `IScene*` and uses it for dirty notifications. When a property changes, the element calls `scene->notify_dirty(*this, flags)`.

**Dirty flags**:

```cpp
enum class DirtyFlags : uint8_t
{
    Layout  = 1 << 0,   // size/position/constraint changed, needs relayout
    Visual  = 1 << 1,   // color/opacity changed, needs redraw
    ZOrder  = 1 << 2    // z_index changed, needs z-resort
};
```

**Dirty tracking**: Element accumulates `DirtyFlags` locally in `pending_dirty_`. In `on_property_changed`, it OR's the new flag into the accumulator. If the accumulator was previously zero (first dirty this frame), it pushes itself onto the Scene's dirty vector. Otherwise it just accumulates locally -- no duplicate entry. Scene's dirty list is a `velk::vector<IElement*>` with no dedup logic. Scene iterates the vector, consumes each element's flags, then clears the list.

**Change detection**: Element implements `IMetadataObserver`, which provides an `on_property_changed(IProperty&)` callback fired whenever any property on the object changes. From this callback, Element determines the appropriate `DirtyFlags` (position/size/local_transform -> Layout, z_index -> ZOrder) and accumulates them as described above. Visual dirty comes from IVisual `on_visual_changed` events, not from element properties. No per-property event subscriptions needed.

**JSON and import**: constraints live in a dedicated `"ui-constraints"` section, visuals in `"ui-visuals"`. Both use the same import handler pattern: the velk-ui plugin registers handlers (implementing `IImporterExtension`) that parse their section, create objects, and attach them to target elements.

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

**IElement** (implemented):
- `position` (`PROP vec3`): layout position in parent-local space, set by the solver
- `size` (`PROP velk::size`): layout dimensions (width, height, depth), set by the solver or constraints
- `local_transform` (`RPROP mat4`, identity default): transform relative to parent, written by solver
- `world_matrix` (`RPROP mat4`, identity default): computed by solver = `parent.world * translate(position) * local_transform`. Read-only; the solver writes it via `write_state`
- `z_index` (`PROP int32_t`, default 0): draw order among siblings

Note: `color` was removed from IElement in Round 2a.5. Visual appearance is now defined by IVisual attachments.

The renderer reads `world_matrix` + `size` for positioning. It reads IVisual attachments for visual content.

**Iterating constraints**: `IObjectStorage::find_attachment<IConstraint>()` returns a single match. To collect all `IConstraint` attachments, iterate via `attachment_count()` / `get_attachment(i)` and `interface_cast<IConstraint>()` each one.

**JSON format for constraints** (`"ui-constraints"` section, handled by `ConstraintImportHandler`):

```json
{
    "version": 1,
    "objects": [
        { "id": "root", "class": "velk-ui.Element" },
        { "id": "child1", "class": "velk-ui.Element" },
        { "id": "child2", "class": "velk-ui.Element" },
        { "id": "child3", "class": "velk-ui.Element" }
    ],
    "hierarchies": {
        "scene": {
            "root": ["child1", "child2", "child3"]
        }
    },
    "ui-constraints": [
        { "target": "root", "type": "Stack", "properties": { "axis": 1, "spacing": 10 } },
        { "target": "child1", "type": "FixedSize", "properties": { "height": "100px" } },
        { "target": "child2", "type": "FixedSize", "properties": { "height": "150px" } },
        { "target": "child3", "type": "FixedSize", "properties": { "height": "100px" } }
    ],
    "ui-visuals": [
        { "target": "child1", "type": "Rect", "properties": { "color": { "r": 1.0, "g": 0.2, "b": 0.2 } } },
        { "target": "child2", "type": "Rect", "properties": { "color": { "r": 0.2, "g": 0.3, "b": 0.9 } } },
        { "target": "child3", "type": "Rect", "properties": { "color": { "r": 0.1, "g": 0.8, "b": 0.2 } } }
    ]
}
```

The handler parses dimension strings (`"100px"`, `"50%"`, bare number = px) into value+unit and sets them on the constraint via its specific interface (IFixedSize, IMargin, etc.).

**JSON format for visuals** (`"ui-visuals"` section, handled by `VisualImportHandler`):

```json
{
    "ui-visuals": [
        { "target": "child1", "type": "Rect", "properties": { "color": { "r": 0.9, "g": 0.2, "b": 0.2 } } }
    ]
}
```

### Visual representation model

An element's visual appearance is defined by **IVisual attachments**, following the same pattern as constraints. IElement is purely about layout and hierarchy. Visual rendering is a separate concern composed via attachments.

**IVisual** (in velk-ui):

```cpp
class IVisual : public velk::Interface<IVisual>
{
public:
    VELK_INTERFACE(
        (PROP, velk::color, color, {}),
        (EVT, on_visual_changed)
    )
    virtual velk::vector<DrawCommand> get_draw_commands(const velk::rect& bounds) = 0;
};
```

- `color`: base color. Meaning depends on visual type (fill color for rect, text tint for text)
- `on_visual_changed`: fired when visual state changes. Elements subscribe when the visual is attached and mark themselves `DirtyFlags::Visual`
- `get_draw_commands(bounds)`: produces draw commands in element-local space. The renderer applies `world_matrix`. The renderer caches the result and only re-calls when `DirtyFlags::Visual` is set

**Draw commands**: POD structs in a flat array. Cache-friendly, GPU-friendly, easy to batch.

```cpp
enum class DrawCommandType : uint8_t { FillRect, TexturedQuad };

struct DrawCommand
{
    DrawCommandType type;
    velk::rect bounds;
    velk::color color;
    float u0, v0, u1, v1;   // texture UVs (zeroed for FillRect)
};
```

**Transforms are separate**: draw commands are in element-local space. The renderer applies `world_matrix`. Element moves only update transforms; cached draw commands are reused. Visual changes rebuild draw commands; transforms are unchanged.

**ITextureProvider**: separate interface for visuals that need textures (e.g. glyph atlases). The renderer checks for it alongside IVisual, uploads pixel data to a GPU texture, and caches the handle.

```cpp
class ITextureProvider : public velk::Interface<ITextureProvider>
{
public:
    virtual const uint8_t* get_pixels() const = 0;
    virtual uint32_t get_texture_width() const = 0;
    virtual uint32_t get_texture_height() const = 0;
    virtual bool is_texture_dirty() const = 0;
    virtual void clear_texture_dirty() = 0;
};
```

**Dirty notification**: IVisual fires `on_visual_changed` whenever its state changes. Element subscribes to this event when the visual is attached (via `ISceneObserver::on_attached`) and unsubscribes when detached. The handler marks the element `DirtyFlags::Visual` via `scene_->notify_dirty()`. The visual never touches the scene directly.

**Visual types** (implemented):
- **RectVisual**: solid color rectangle. Produces one FillRect command filling the element bounds. In velk-ui core plugin
- **TextVisual**: shaped text rendered as textured glyph quads. Implements IVisual + ITextureProvider + ITextVisual. Uses IFont + GlyphAtlas internally. In text plugin. Exposes `set_text(string_view, IFont&)` via the `ITextVisual` interface

**GlRenderer**: two rendering pipelines. Pass 1: untextured FillRect instances (instanced quads with color). Pass 2: textured TexturedQuad instances with atlas bound (instanced quads with color + UVs). The renderer no longer subscribes to individual element properties; all dirty notification flows through Element -> Scene -> `update_visuals()`.

**Decisions made during Round 2a.5**:
- `color` removed from IElement entirely (not deprecated, removed). Visual appearance is exclusively via IVisual attachments
- The renderer caches draw commands per element but rebuilds instance buffers from cache + world_matrix every frame. This keeps the architecture simple and handles both visual and layout changes without complex partial-update logic
- Scene pushes ALL dirty elements to the renderer (not just Visual-dirty), because draw commands depend on element size (passed as bounds to `get_draw_commands`), which changes during layout
- Scene registers new elements with the renderer immediately on attach (not just during `load()`), so programmatically added elements work correctly
- The EVT accessor on IVisual returns `velk::Event` (a value wrapper), not `IEvent::Ptr`. Use `.invoke()` / `.add_handler()`, not `->invoke()`
- `ITextVisual` interface added to text plugin's public headers to expose `set_text()` without depending on the concrete TextVisual class
- `IFont::init_default()` added so fonts can be created and initialized without knowing about embedded font data
- Embedded Inter Regular font data re-sourced from Google Fonts CDN (the original embedded data was corrupted HTML)

**App flow with Scene**:
1. Import JSON -> objects + hierarchy + constraints (via import handler)
2. Create Scene, set renderer and viewport, call `scene->load(store)`
3. Load replicates imported hierarchy, attaches elements (triggering Layout+Visual dirty), registers visuals with renderer
4. Each frame: `velk.update()` -> plugin `post_update` -> `scene->update()` (solver runs only if layout dirty)
5. Renderer uploads dirty slots and draws

**Implementation rounds** (incremental, each ending with something testable):

Round 1: **Foundation types** ~~DONE~~
- `mat4`, `aabb` in velk `math_types.h` with operators, translate/scale factories, min/max accessors
- Math type operators and named constants on all types
- `velk::size` extended to 3D (width, height, depth)
- `IElement` updated: position (vec3), size, color, local_transform, world_matrix, z_index
- Importer: struct type deserialization from JSON objects and arrays
- GL renderer updated for new IElement layout

Round 2a: **Scene + Core Constraints + Solver + Importer** ~~DONE~~
- `dim`, `Constraint` structs in velk-ui; `register_type<dim>()` in plugin init
- `IConstraint` interface (with `IHierarchy*` parameter for layout constraints)
- `DirtyFlags` enum (Layout, Visual, ZOrder)
- `ISceneObserver` interface (on_attached/on_detached, Element marks Layout+Visual dirty on attach)
- `IScene` interface (inherits IHierarchy) + `Scene` class (owns `logical_` hierarchy, dirty vector, visual list)
- Element dirty tracking: `pending_dirty_` accumulator, pushes to Scene's vector on first dirty per frame
- `IStack`, `IFixedSize` interfaces (VELK_INTERFACE)
- `Stack`, `FixedSize` implementations (each implements both `IConstraint` and its specific interface)
- `LayoutSolver` (internal to Scene): top-down recursion, measure/apply, world matrix accumulation. Only runs when Layout dirty
- `ConstraintImportHandler` for `"ui-constraints"` JSON section
- `IRenderer::add_visual` takes `const IElement::Ptr&` (typed, not IObject)
- `GlRenderer::update_visuals` receives changed elements from Scene each frame
- Per-config DLL copy (Debug copies Debug velk.dll, Release copies Release)
- Register all new types in velk-ui plugin; plugin `post_update` drives Scene updates
- Testable: import a scene with stack + fixed-size constraints, run solver, verify element positions/world matrices via test app

Round 2a.5: **Visual Representation + Text Rendering** ~~DONE~~
- `DrawCommand` POD struct and `DrawCommandType` enum in types.h
- `IVisual` interface (color PROP, on_visual_changed EVT, get_draw_commands virtual)
- `ITextureProvider` interface (pixel data, dimensions, dirty tracking)
- `RectVisual` implementation (single FillRect command, fires on_visual_changed on color change)
- `TextVisual` implementation (IVisual + ITextureProvider + ITextVisual, GlyphAtlas, shaped glyph quads)
- `ITextVisual` public interface in text plugin (exposes set_text without concrete class dependency)
- `VisualImportHandler` for `"ui-visuals"` JSON section (creates RectVisual from color data)
- `color` removed from IElement; visual appearance exclusively via IVisual attachments
- Element subscribes to IVisual `on_visual_changed` events in `on_attached`, marks self Visual-dirty
- GlRenderer rewritten: two pipelines (untextured FillRect, textured TexturedQuad), draw command caching, atlas texture management, no per-property subscriptions
- Scene updated: pushes all dirty elements to renderer, registers elements on attach
- `IFont::init_default()` added, embedded Inter Regular font data fixed
- JSON scenes updated: `"ui-visuals"` section replaces color on elements
- Test app creates programmatic text element ("Hello, Velk!") with TextVisual
- Testable: 3 colored rects + "Hello, Velk!" text visible on screen

Round 2b: **Additional Constraints**
- `IMargin`, `IPadding`, `IAlignment`, `IMinMax`, `IFitContent` interfaces
- `Margin`, `Padding`, `Alignment`, `MinMax`, `FitContent` implementations
- Register new types, extend import handler
- Testable: scenes using the full constraint set

Round 3: **Demo polish**
- Additional test scenes
- Testable: multiple visual types rendered together

Round 4: **Unit tests**
- Test framework setup
- Solver tests: verify element positions/sizes/world matrices for various constraint combinations
- Scene dirty tracking tests
- Constraint tests (Stack multi-pass, FixedSize, Margin, etc.)

### Ahead

- **2D renderer features**: rounded rects, gradients, clipping, custom shaders
- **Text features**: line wrapping, multi-line, text alignment, font size/weight variants
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
