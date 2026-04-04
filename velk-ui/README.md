# velk-ui

UI framework for [velk-platform](../README.md). Namespace: `velk::ui`.

Declarative scene loading from JSON, programmatic element creation, trait-based composition (constraints, visuals, transforms, input), and a scene renderer that submits draw calls to [velk-render](../velk-render/).

## Source structure

| Directory | Description |
|-----------|-------------|
| `include/velk-ui/interface/` | Core interfaces: IScene, IElement, IVisual, IRenderer, IConstraint, ITrait |
| `include/velk-ui/api/` | Convenience wrappers: Scene, Element, Visual, Trait, material and input factories |
| `include/velk-ui/ext/` | CRTP base classes for implementing traits and visuals |
| `src/` | Implementations: elements, scene, layout solver, visuals, materials, renderer, input |

## Key concepts

* **Element**: The basic building blocks. One type for all elements in the Scene hierarchy. Behavior comes from traits.
* **Trait**: Attached to elements to provide layout, transform, visual, and input behavior. Phases run in order: Layout, Transform, Visual, Input.
* **Visual**: Produce draw entries. The renderer collects them, batches by pipeline, writes GPU buffers, and submits draw calls to the render backend.
* **Material**: Defines how a visual is drawn. A material provides a custom shader pipeline and GPU data.
* **Scene**: Extends Velk's `ClassId::Hierarchy`. Owns the element hierarchy, run the layout solver, and track dirty state. The renderer pulls changes via `consume_state()`.

## Documentation

| Document | Description |
|----------|-------------|
| [Getting started](docs/getting-started.md) | Scene loading, programmatic API, and the two ways to build UI |
| [Scene](docs/scene.md) | Scene hierarchy, elements, geometry, JSON format |
| [Traits](docs/traits.md) | Trait system: phases, layout, transform, visual, and input traits |
| [Input](docs/input.md) | Input dispatcher, hit testing, event dispatch, built-in input traits |
| [Update cycle](docs/update-cycle.md) | Frame loop, dirty flags, layout solving, and rendering |
| [Performance](docs/performance.md) | Design choices: single element type, flat hierarchy, traits, batched updates |
