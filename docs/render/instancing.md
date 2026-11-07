# Instancing

`InstancedMeshVisual` draws meshes many times on one element, for counts where an element per instance is too expensive.

```cpp
auto forest = velk::trait::visual::create_instanced_mesh();
forest.set_mesh(tree_a);                         // mesh 0
uint32_t tree_b_index = forest.add_mesh(tree_b); // further meshes, such as species

uint32_t slot = forest.add_instance(transform, tree_b_index);
forest.add_instances(transforms);                // many at once, using mesh 0
forest.set_transform(slot, moved);
forest.remove_instance(slot);

element.add_trait(forest);
```

Each instance has a slot, which is its stable index until it is removed. Removed slots are reused. Transform traits on the element move all instances together.

## What is drawn

A compute pass culls instances each frame, by frustum and by size on screen, and writes the list of those to draw. Rays see instances through the scene BVH whether or not they are drawn.

| Setting | Default | Effect |
|---|---|---|
| `set_draw_distance(distance)` | 0, no limit | Instances further than this are not drawn. Rays still see them. |
| `set_far_form(enabled)` | on | Instances too small on screen are averaged into the pixels they cover, and wide rays average over them too, instead of each being drawn and tested. Applies to the deferred path. |

Each instance is drawn at the [level of detail](mesh.md#levels-of-detail) its size on screen needs.

## Motion

`set_motion` moves instances on the GPU, for motion that stays near where each instance was placed, such as wind in trees.

```cpp
forest.set_motion(R"(
    mat4 velk_instance_motion(mat4 rest, uint slot, vec4 params)
    {
        rest[3].x += sin(params.x + float(slot)) * 0.5;
        return rest;
    }
)", /*max_displacement=*/0.5f);

forest.set_motion_params({time, 0.f, 0.f, 0.f});
```

The function runs for every instance on each frame the params or placements change. It does no CPU work per instance and changes no tree, and rays and raster see the same result.

No point of a mesh may move further than `max_displacement`, since each instance's bounds in the BVH are grown by that much. Going past it is logged as an error, and rays may then miss the instance. An empty string removes the motion.

For larger moves use `set_transform`, which updates the BVH.

## Memory budget

`set_residency_budget(bytes)` limits the GPU memory the visual's instances use. 0, the default, means no limit.

Instances are grouped into partitions of nearby instances. Partitions nearest the camera stay resident, and a partition beyond the budget frees its instance data. It keeps the top of its tree, whose leaves hold up to 32 instances, and those leaves are drawn and traced as an average over their volume. As the camera moves, partitions swap in nearest first, at most 8 per update, so a fast camera can arrive before they do.

The budget applies with the far form on and no motion function, and to the deferred path.

The budget covers instance data, draw records and tree nodes, about 290 bytes per resident instance. Transforms stay in CPU memory for every instance.

With `VELK_AUTO_GPU_STATS=1` set, the log reports each arena's use at auto exit.

## Limits

- The RT path's primary rays do not draw instances, since they walk a flat list of shapes. Its bounces and shadows do see them.
- Under a render target, which has no GPU culling, every instance draws as mesh 0.
- AO rays pass through unloaded partitions.
- A low budget leaves a visible edge where drawn instances meet the average.
