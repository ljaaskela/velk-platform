# Mesh

Velk models 3D geometry in two layers, matching glTF 2.0 vocabulary exactly.

- `IMesh` inteface defines a container: an authored group of primitives with an aggregate bounding box. Carries no geometry or material directly.
- `IMeshPrimitive` defines one geometry + material unit (what glTF calls a "primitive", what Unity/Unreal call a "submesh"). Owns the vertex/index range, attribute layout, topology, bounds, and a material `ObjectRef`.
- `IMeshBuffer` defines the GPU-resident VBO + IBO storage. A single `IMeshBuffer` may back many primitives in the same mesh (glTF's standard layout) or be exclusive to one.

A multi-material model (e.g. a car body with paint, glass, trim) is one `IMesh` with multiple primitives, each carrying its own material. Per-primitive is the granularity renderers care about because a draw call binds one pipeline + one material.

## Buffers

`IMeshBuffer` holds VBO bytes followed by IBO bytes in one region (VBO at offset 0, IBO at `get_ibo_offset()`). A primitive without indices reports `get_ibo_size() == 0`; the backend skips the index bind and dispatches a non-indexed draw.

The region is a suballocation of the engine-wide mesh-word arena rather than a standalone allocation, so every mesh in the scene shares one GPU buffer and one bound descriptor. Shaders reach a mesh by word index, and the indexed draw binds the arena's backing buffer at the mesh's offset. `get_gpu_ref(buffer)` reports residency and yields the word base; it is `Kind::None` until the bytes are uploaded.

Two sibling primitives in the same mesh may return the same `IMeshBuffer::Ptr` from `get_buffer()`. Each primitive's `get_vertex_offset()` / `get_vertex_count()` / `get_index_offset()` / `get_index_count()` describe its range into the shared buffer. Callers key batching by buffer pointer and offsets, and the backend issues the right offset arguments to `vkCmdBindIndexBuffer` / the vertex-pulling shader.

## Building meshes

Meshes and primitives are authored artifacts. Construction goes through `IMeshBuilder`, owned by the `IRenderContext`:

```cpp
// Low-level: one primitive with its own exclusive buffer.
auto primitive = ctx.get_mesh_builder().build_primitive(
    attributes, vertex_stride,
    vertex_data, vertex_count,
    index_data, index_count,
    topology,
    bounds);

// Container wrapping one or more primitives.
IMeshPrimitive::Ptr prims[] = { primitive };
auto mesh = ctx.get_mesh_builder().build({ prims, 1 });

// Convenience: single-primitive mesh in one call.
auto mesh = ctx.get_mesh_builder().build(
    attributes, vertex_stride, vertex_data, vertex_count,
    index_data, index_count, topology, bounds);
```

For glTF import with shared buffers across primitives, allocate the `IMeshBuffer` directly, then construct each primitive referencing it with its own offsets and counts.

## Procedural primitives

`IMeshBuilder` also provides cached procedural shapes:

```cpp
auto cube = ctx.get_mesh_builder().get_cube(/*subdivisions=*/0);
auto sphere = ctx.get_mesh_builder().get_sphere(/*subdivisions=*/16);
```

Each call produces a fresh `IMesh` + fresh `IMeshPrimitive` so per-instance materials are independent. The underlying `IMeshBuffer` is cached by (shape, subdivisions), so repeated calls upload the GPU bytes exactly once.

The `RenderContext` API wrapper exposes shortcuts:

```cpp
auto ctx = app.render_context();
auto cube_mesh = ctx.build_cube();
auto sphere_mesh = ctx.build_sphere(32);
```

`get_unit_quad()` returns a shared singleton `IMesh` used by every 2D visual as its implicit geometry; its primitive carries no material and never will.

## Authoring materials on primitives

Materials are a property of the primitive, not the mesh. Using the `velk::Mesh` API wrapper:

```cpp
velk::Mesh cube_mesh(ctx.build_cube());
cube_mesh.set_material(0, velk::material::create_standard(
    velk::color{0.95f, 0.7f, 0.4f, 1.f}, /*metallic=*/0.9f, /*roughness=*/0.15f));
```

`set_material(idx, mat)` is sugar for `mesh.primitive(idx).set_material(mat)`. Each primitive's material is independent; setting material on primitive 0 never affects primitive 1.

To attach the mesh to a 3D visual:

```cpp
auto cube_vis = velk::trait::visual::create_cube();
cube_vis.set_mesh(cube_mesh);
cube.add_trait(cube_vis);
```

See [materials](materials.md) for how material pipelines compile and how per-primitive material flows into draw calls.

## Bounds

`IMesh::get_bounds()` returns the aggregate AABB spanning every primitive. It is computed lazily as the union of primitive bounds on first call and cached. Procedural shapes pre-populate the aggregate at construction because the extent is known analytically; callers never observe the lazy path for those. A mesh mutated after construction (not yet a supported case) would invalidate the cache.

Each primitive reports its own local-space bounds via `IMeshPrimitive::get_bounds()`. Importers should supply this from glTF's per-primitive `min`/`max` accessor fields directly.

## Levels of detail

An opaque mesh with more than one cluster is drawn at levels of detail, with nothing to set. On first use the primitive builds a hierarchy of clusters, each simplified from its children with shared edges kept in place. A compute pass then chooses, per instance, the clusters whose error on screen is at most one pixel and whose parent's error is more. One indirect draw is issued per cluster in use.

Blended meshes and deforming meshes are drawn in full.

Rays still trace the full mesh. A ray starting from a simplified surface is lifted by that surface's error, so the full mesh does not shadow it. Full detail is not lifted.

## Deforming meshes

`IMeshBuilder::build_deforming` copies a mesh for one element to deform. The copy owns its vertices, tree and clusters, and starts with the source's materials. It reads the source's geometry back if that has been uploaded, so a large mesh is better copied before its first frame. It returns `nullptr` if the source is not an indexed triangle list of `MeshVertex`.

A deformer is a trait on the same element. There are two kinds:

| Deformer | Runs on | Use for | Tree |
|---|---|---|---|
| `ext::MeshDeformer<T>`, implementing `IMeshDeformer::deform` | CPU, once when attached and after each `invoke_deform_changed` or property change | Any change of shape | Refit on the CPU, built again if refitting has made it too costly for rays |
| `ComputeMeshDeformer` | GPU, when its params change | Changes that stay near the rest shape | Refit on the GPU, with bounds the rest bounds grown by the maximum displacement |

```cpp
auto mesh = ctx.get_mesh_builder().build_deforming(source);
visual.set_mesh(velk::Mesh(mesh));

auto deformer = velk::trait::deform::create_compute_mesh();
deformer.set_deform(R"(
    VelkVertex3D velk_mesh_deform(VelkVertex3D rest, uint index, vec4 params)
    {
        rest.position.y += sin(params.x + rest.position.x) * 0.1;
        return rest;
    }
)", /*max_displacement=*/0.1f);
deformer.set_params({time, 0.f, 0.f, 0.f});
element.add_trait(deformer);
```

No vertex of a compute deformer may move further than the maximum displacement. Going past it is logged as an error, and rays may then miss the mesh.

## Relationship to Visual3D

3D visuals reference a mesh through `IVisual3D::mesh` (an `ObjectRef`). Procedural primitives like `CubeVisual` and `SphereVisual` lazily populate this slot on first render using the mesh builder when the caller hasn't set one explicitly. Once populated, the visual's mesh is observable via `Visual3D::get_mesh()` and its primitives' materials can be set per-instance.

Callers who want to configure material at authoring time (before the first frame) build the mesh up-front through the context:

```cpp
auto mesh = ctx.build_cube();
velk::Mesh(mesh).set_material(0, my_material);
cube_vis.set_mesh(mesh);
```

See [traits](../ui/traits.md) for the Visual2D / Visual3D split and how visuals hook into the element system.
