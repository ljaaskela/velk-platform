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

### Layout model

Single `Element` type (poolable/hiveable), behavior defined by attachments.

**Defaults**: elements fill their parent. Siblings overlap (no implicit layout).

**Attachments**: velk already supports attaching any object to any object via `IObjectStorage`. The layout system finds `IConstraint` attachments on each element.

**`Constraint` struct** (in `velk_ui` namespace):

```cpp
struct Constraint
{
    velk::rect available;
};
```

**Single interface** `IConstraint` (using `velk::rect`, `velk::size` from `velk/api/math_types.h`):

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

**Ordering**: the solver collects all `IConstraint` attachments on an element, sorts by phase (`Layout` first, then `Constraint`), and within the same phase runs them in attachment order. This gives a predictable pipeline without rigid sub-priorities.

**Two-phase solve**: `measure` computes desired size, `apply` writes final bounds into element state via `velk::write_state<IElement>()`. The difference between a constraint and a layout is just scope:

- `FixedSize` (phase: `Constraint`): `measure` returns its fixed size, `apply` writes it. Only touches self.
- `Margin` (phase: `Constraint`): `measure` adds margins around child measure, `apply` shrinks rect and delegates. Only touches self.
- `Stack` (phase: `Layout`): `measure` walks children (via hierarchy), calls their `measure`, sums along axis. `apply` divides space and calls children's `apply`. Touches self and children.

The solver finds `IConstraint` on each element. One interface, the solver doesn't care what kind of constraint it's dealing with. `Constraint` struct is extensible for future fields.

**Implementation order**:
1. `IConstraint` interface in velk-ui
2. `Stack`, `FixedSize`, `Margin` (all implement `IConstraint`)
3. Layout solver: starts at root with viewport rect, calls `measure`/`apply` recursively via `IObjectStorage::find_attachment<IConstraint>()`
4. Renderer reads computed bounds from `IElement` state
5. Test scene: vertical stack of three elements

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
