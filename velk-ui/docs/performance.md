# Performance design

velk-ui is designed around a few deliberate choices that keep memory compact, cache-friendly, and scalable. This document explains the reasoning.

## Single element type

All UI nodes regardles of their type are implemented by the same class (`ClassId::Element`). There is no `Button`, `Panel`, `Label` in the class hierarchy. This means velk's type registry allocates a single `ObjectHive` for all elements, giving dense, contiguous storage with no per-subclass fragmentation. Creating and iterating thousands of elements stays cache-friendly because they all live in the same page-allocated pool.

Behavior differences between elements come entirely from traits (see below), not from the element type itself.

## Flat hierarchy

The scene hierarchy is velk's general-purpose `ClassId::Hierarchy`: a flat structure of object pointers with parent/child relationships. There are no deep wrapper objects or per-node allocations beyond the pointer entries themselves. This keeps the hierarchy lightweight and makes operations like add, remove, and reparent cheap pointer swaps.

The visual list (z-sorted draw order) is a separate flat `ClassId::Hierarchy` rebuilt only when `DrawOrder` is dirty. Layout traversal walks the hierarchy top-down once per frame when `Layout` is dirty.

## Traits

Layout constraints and visuals are not baked into the element. They are separate objects attached as traits via velk's attachment mechanism. An element with no visual has zero visual overhead. An element with no constraint uses no layout memory beyond what the solver reads from the element's position/size state.

This also means a single object can serve as both a constraint and a visual (if it has both traits), and elements can have multiple visuals composited together, all without any special-casing in the element or the hierarchy.

Traits are discovered from the elements in the hierarachy at solve/render time, not stored in typed slots. This keeps the element class minimal and allows new trait types to be added without changing the element or scene.

## Batched dirty processing

Property changes during a frame are not processed immediately. Elements accumulate `DirtyFlags` as properties change, and the scene collects them into a single set of flags. The `update()` pass processes everything at once:

- Layout is solved once, even if multiple elements changed size
- The draw list is rebuilt once, even if multiple z-indices changed
- The renderer pulls all changes in a single `consume_state()` call

Combined with velk's deferred property writes, bulk updates (e.g. animating 100 elements) result in exactly one layout pass and one renderer upload per frame.

## Pointer-based GPU backend

The render backend is designed to minimize CPU overhead per draw call and avoid GPU-side indirection.

**No CPU-side resource binding.** Traditional backends spend CPU time per draw call binding vertex buffers, updating descriptor sets, and setting uniforms. The pointer-based model reduces this to a single `vkCmdPushConstants` (8 bytes) per draw. All other data is already in GPU memory, reachable via pointer dereference.

**No vertex input overhead.** There are no VAOs, no vertex buffer binds, no vertex attribute descriptions. Pipelines have empty vertex input state. Shaders read instance data directly from GPU buffers via `buffer_reference` pointers.

**One descriptor bind per frame.** The bindless texture array is bound once at the start of each frame. Individual draw calls reference textures by index, with no per-draw descriptor updates.

**Bump allocator for per-frame data.** The renderer writes all per-frame data (instances, draw headers, material params) sequentially into a double-buffered GPU buffer using a simple offset bump. No per-frame allocations, no staging copies, no command buffer transfers. The CPU writes directly to persistently mapped GPU memory.

**Minimal pipeline state.** A pipeline is just compiled SPIR-V. No vertex input state, no descriptor set layouts, no pipeline layout objects beyond the single shared layout. Pipeline creation is fast because there is almost no state to specialize on.

These choices mean the per-draw-call cost on the CPU side is dominated by the `vkCmdPushConstants` + `vkCmdDraw` calls themselves, which is the theoretical minimum. On the GPU side, the pointer dereference for draw data is a single memory load from L2-cached memory, comparable to a traditional uniform buffer read.

## Comparison with ECS

velk-ui's architecture is adjacent to the Entity Component System pattern but makes different tradeoffs. Understanding where it aligns and diverges helps explain the performance characteristics.

### Similarities

| Concept | ECS | velk-ui |
|---------|-----|---------|
| Entity | Lightweight ID | `ClassId::Element` (single class for all UI nodes) |
| Component | Data attached to entity | Trait attached to element (various classes implementing `ITrait`) |
| System | Global function iterating all components of a type | Phase-specific pass (layout solver, renderer, input dispatch) |
| Composition over inheritance | Behavior comes from attached components, not entity subclass | Behavior comes from attached traits, not element subclass |

Like ECS, velk-ui avoids deep class hierarchies. A button is not a Button class; it is an Element with Click, RectVisual, and TextVisual traits attached. Adding or removing behavior is attaching or detaching trait objects.

### Key differences

**Storage model: ObjectHive vs. columnar arrays.** In a classic ECS (e.g. EnTT, flecs), components of the same type are stored in dense, contiguous arrays (Structure of Arrays). This enables linear iteration over all components of a given type with minimal cache misses.

Velk uses per-class ObjectHives: page-allocated pools where all instances of a class live in dense pages. This is AoS (Array of Structures) rather than SoA. All elements live in one hive, all RectVisual traits in another, etc. This gives good locality when creating and destroying objects (allocation is a pool bump) and when iterating objects of the same class. However, iterating "all visuals attached to all elements" requires per-element attachment traversal rather than a flat array scan.
* That said, because all instances of a class live in the same ObjectHive, the type registry could provide direct iteration over all instances of a given trait class (e.g. "all PhysicsTrait objects") without touching elements at all. This is a potential optimization path for system-style workloads that need to process all components of a type.

For use cases that need raw SoA-style storage without the velk object model overhead, `RawHive<T>` provides the same page-allocated dense pool for plain POD structs. Velk uses this internally (e.g. `ObjectStorage` metadata is allocated from a `RawHive`). A consumer that needs ECS-style dense component arrays for a hot path can use `RawHive<MyStruct>` directly while still participating in the velk ecosystem.

**Trait discovery vs. archetype queries.** ECS frameworks provide O(1) archetype-based queries: "give me all entities with Transform + Velocity" returns a view over dense storage. Velk discovers traits by walking an element's attachment list (`IObjectStorage::get_attachment()`). This is a linear scan per element (typically 2-5 attachments), not a global query.

The current UI workload is change-driven: most frames, most elements don't change. Rather than scanning all elements every frame, velk tracks which elements are dirty and only processes those. 
* The layout solver only runs when `DirtyFlags::Layout` is set. 
* The renderer only rebuilds draw commands for elements in the redraw list. 
* Dirty tracking makes per-element trait discovery acceptable because it happens for a small subset of elements each frame.

**Shared attachments.** Most ECS frameworks enforce a 1:1 mapping between entity and component: each entity has its own instance of each component type. Velk's attachment model allows the same trait object to be attached to multiple elements. 
* For example, a `FixedSize` constraint with `width=200, height=40` can be shared across all elements that need that size, instead of creating a separate instance for each. 
* This reduces memory and makes bulk changes trivial: updating the shared trait's properties updates all elements that use it.

**Property change notifications vs. system-scans.** A pure ECS has no built-in change detection. Systems typically scan all components every frame, or use generation counters / dirty flags as an add-on. Velk's property system fires `on_changed` events, and elements accumulate dirty flags automatically. This drives the incremental update model: property change -> dirty flag -> solver/renderer processes only what changed.

**Object identity and interfaces.** ECS entities are typically opaque IDs with no methods. Velk objects have interface-based polymorphism (`interface_cast<IVisual>(attachment)`), reference counting, and the full velk property/event/function system. This enables features like data bindings between properties, event-driven UI interactions, and the importer system, which don't map naturally to a pure ECS model.

### What this means for performance

* **Creation/destruction:** ObjectHive pool allocation is comparable to ECS archetype allocation. Both avoid heap fragmentation and give dense storage.
* **Steady-state (most frames):** Velk's dirty-flag model means near-zero CPU work when nothing changes. An ECS that scans all entities every frame does more work on idle frames but may be faster when everything changes at once.
* **Bulk updates (e.g. animating 1000 elements):** The dirty flag model coalesces multiple property changes into a single layout pass. Deferred property writes batch further. The per-element trait discovery cost scales with the number of dirty elements, not the total element count.
* **Rendering:** The renderer iterates the visual list (a flat array in draw order) and accesses the element cache (a hash map). This is less cache-friendly than a pure ECS iterating a contiguous component array, but the actual bottleneck is GPU submission, not CPU-side iteration. With sub-millisecond prepare times at 1000+ elements, the overhead of per-element lookups is not material.
