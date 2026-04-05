# Materials

Materials define how geometry is shaded. Every material provides a pipeline (compiled shader) and optionally GPU data that the shader reads via buffer device address.

There are two ways to create materials:

- **ext::Material** for application-defined materials where you control the shader and data layout directly
- **ShaderMaterial** for dynamic materials where inputs are discovered automatically from the shader

## Shader includes

Shader includes are registered via `IRenderContext::register_shader_include()`. Any module can register its own include, and shaders reference them with `#include "name"`.

**velk.glsl** (provided by velk-render):
- `Globals` — frame globals (projection matrix, viewport)
- `Ptr64` — dummy pointer type for skipping 8-byte fields in fragment shaders
- `velk_unit_quad(vertex_index)` — unit quad position from triangle strip index
- Buffer reference extensions

**velk-ui.glsl** (provided by velk-ui):
- `RectInstance`, `TextInstance` — standard 2D UI instance data structs
- `RectInstances`, `TextInstances` — buffer_reference arrays

Other modules can register their own includes (e.g. a 3D module could register `velk-3d.glsl` with mesh instance types).

A shader typically needs to declare its `DrawData` struct (header + material fields) and `main()`. Common types come from includes.

## Application-defined materials (ext::Material)

Use `ext::Material<T>` when you write both the shader and the material class. You define the GPU data struct, implement `gpu_data_size()` and `write_gpu_data()`, and the framework passes your data straight to the shader. This should be the default option for any shader code.

### C++ side

```cpp
#include <velk-render/ext/material.h>
#include <velk-render/gpu_data.h>

VELK_GPU_STRUCT MyParams
{
    float color[4];
    float intensity;
    // No manual padding needed: VELK_GPU_STRUCT ensures 16-byte alignment
};

class MyMaterial : public velk::ext::Material<MyMaterial, IMyProps>
{
public:
    VELK_CLASS_UID(...);

    uint64_t get_pipeline_handle(IRenderContext& ctx) override
    {
        return ensure_pipeline(ctx, my_frag_src, my_vert_src);
    }

    size_t gpu_data_size() const override { return sizeof(MyParams); }

    void write_gpu_data(void* out, size_t) const override
    {
        auto state = read_state<IMyProps>(this);
        if (!state) { return; }
        auto& p = *static_cast<MyParams*>(out);
        p = {};
        p.color[0] = state->color.r;
        // ...
        p.intensity = state->intensity;
    }
};
```

The `ext::Material` base provides `ensure_pipeline()` which lazily compiles the shader on first use and caches the pipeline handle.

### Shader side

```glsl
// my_material_vert.glsl
#version 450
#include "velk.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    Globals globals;
    RectInstances instances;
    uint texture_id;
    uint instance_count;
    uint _pad0;
    uint _pad1;
    // Material data starts here (offset 32), matching MyParams
    vec4 color;
    float intensity;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec2 v_local_uv;

void main()
{
    vec2 q = velk_unit_quad(gl_VertexIndex);
    RectInstance inst = root.instances.data[gl_InstanceIndex];
    vec2 world_pos = inst.pos + q * inst.size;
    gl_Position = root.globals.projection * vec4(world_pos, 0.0, 1.0);
    v_local_uv = q;
}
```

```glsl
// my_material_frag.glsl
#version 450
#include "velk.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    Ptr64 globals;
    Ptr64 instances;
    uint texture_id;
    uint instance_count;
    uint _pad0;
    uint _pad1;
    vec4 color;
    float intensity;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) in vec2 v_local_uv;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = root.color * root.intensity;
}
```

The C++ `MyParams` struct and the GLSL `DrawData` fields after the header must match in layout. Using `VELK_GPU_STRUCT` on the C++ side and std430 on the GLSL side ensures they agree.

## Shader materials (ShaderMaterial)

Use `ShaderMaterial` when the shader comes from outside your application code: Loaded from a file, provided by a user, or otherwise not known at compile time. The material discovers its parameters automatically via shader reflection and exposes them as dynamic properties on an inputs object, so you can set values without knowing the data layout in advance.

### Creating a shader material

```cpp
#include <velk-render/api/material/shader.h>

constexpr velk::string_view my_frag = R"(
#version 450
#include "velk.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    Ptr64 globals;
    Ptr64 instances;
    uint texture_id;
    uint instance_count;
    uint _pad0;
    uint _pad1;
    vec4 tint;
    float speed;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = root.tint * (0.5 + 0.5 * sin(v_uv.x * root.speed));
}
)";

// Create and configure
auto sm = velk::create_shader_material(ctx, my_frag, my_vert);
sm.input<velk::color>("tint").set_value({1, 0.5f, 0, 1});
sm.input<float>("speed").set_value(10.f);
```

### How it works

1. `create_shader_material` compiles the GLSL to SPIR-V and creates a pipeline
2. The SPIR-V is reflected to find fields in the `DrawData` struct after the standard 6 header fields (32 bytes)
3. For each discovered field, a dynamic property is created on the material's inputs object
4. `input<T>("name")` returns a typed `Property<T>` accessor for the named parameter

### The inputs object

ShaderMaterial has an `inputs` property (ObjectRef) that points to a velk Object holding the shader parameters as dynamic properties. This integrates with the standard velk property system:

- Properties are animatable via velk's animation system
- Properties are bindable via velk's binding system
- Properties can be set from JSON via the importer
- The editor can discover and display them via IMetadata

### Pre-populating inputs

The inputs object can exist before the shader loads. When the shader is compiled, reflection matches existing properties by name and type, preserving their values. New parameters get default values.

This enables workflows like:

1. Define material parameters in a JSON scene file
2. Load the shader later (e.g. from a .glsl file)
3. Existing parameter values are preserved

## DrawData struct layout

Both material types follow the same GPU data layout. The shader receives a pointer to a `DrawData` struct via push constants. The first 32 bytes are the standard header:

| Offset | Field | Type | Size |
|--------|-------|------|------|
| 0 | globals | buffer_reference (pointer) | 8 |
| 8 | instances | buffer_reference (pointer) | 8 |
| 16 | texture_id | uint | 4 |
| 20 | instance_count | uint | 4 |
| 24 | _pad0 | uint | 4 |
| 28 | _pad1 | uint | 4 |
| **32** | **material data starts** | | |

Material-specific fields follow at offset 32. The `gpu_data_size()` / `write_gpu_data()` methods control what goes there. Use `VELK_GPU_STRUCT` on the C++ side to ensure 16-byte alignment matching std430.

## Supported parameter types

| GLSL type | velk type | Size |
|-----------|-----------|------|
| `float` | `float` | 4 |
| `vec2` | `vec2` | 8 |
| `vec3` | `vec3` | 12 |
| `vec4` | `color` | 16 |
| `mat4` | `mat4` | 64 |
| `int` | `int32_t` | 4 |
| `uint` | `uint32_t` | 4 |
