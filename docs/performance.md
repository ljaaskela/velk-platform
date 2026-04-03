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
