# Materials

Materials define how geometry is shaded. Every material provides a pipeline (compiled shader) and optionally GPU data that the shader reads via buffer device address.

There are two ways to create materials:

- **ext::Material** for application-defined materials where you control the shader and data layout directly
- **ShaderMaterial** for dynamic materials where inputs are discovered automatically from the shader

## Contents
- [Shader includes](#shader-includes)
- [Default shaders](#default-shaders)
- [Shader cache](#shader-cache)
  - [How it works](#how-it-works)
  - [What is cached and what is not](#what-is-cached-and-what-is-not)
  - [Disabling the cache](#disabling-the-cache)
- [Application-defined materials (ext::Material)](#application-defined-materials-extmaterial)
  - [C++ side](#c-side)
  - [Shader side](#shader-side)
- [Shader materials (ShaderMaterial)](#shader-materials-shadermaterial)
  - [Creating a shader material](#creating-a-shader-material)
  - [How it works](#how-it-works)
  - [The inputs object](#the-inputs-object)
  - [Pre-populating inputs](#pre-populating-inputs)
- [DrawData struct layout](#drawdata-struct-layout)
- [Supported parameter types](#supported-parameter-types)
- [Classes](#classes)


## Shader includes

Shader includes are registered via `IRenderContext::register_shader_include()`. Any module can register its own include, and shaders reference them with `#include "name"`.

**velk.glsl** (provided by velk-render):
- `GlobalData` — frame globals (projection matrix, viewport)
- `Ptr64` — dummy pointer type for skipping 8-byte fields in fragment shaders
- `velk_unit_quad(vertex_index)` — unit quad position from triangle strip index
- `VELK_DRAW_DATA(InstancesType)` — standard DrawData header fields with padding
- Buffer reference extensions

**velk-ui.glsl** (provided by velk-ui):
- `RectInstance`, `TextInstance` — standard 2D UI instance data structs
- `RectInstanceData`, `TextInstanceData` — buffer_reference arrays

Other modules can register their own includes (e.g. a 3D module could register `velk-3d.glsl` with mesh instance types).

A shader typically needs to declare its `DrawData` struct (header + material fields) and `main()`. Common types come from includes.

## Default shaders

The UI renderer registers default vertex and fragment shaders via `IRenderContext`. These are used when a material or `compile_pipeline` call omits a shader:

**Default vertex shader** outputs:
- `location 0`: `v_color` (vec4) from the instance color
- `location 1`: `v_local_uv` (vec2) as the 0..1 quad coordinate

**Default fragment shader**: passes through `v_color` as a solid fill.

Most materials only need to provide a fragment shader. The default vertex shader handles quad positioning, color, and UV passthrough.

## Shader cache

`IRenderContext::compile_shader()` consults an on-disk cache before invoking shaderc. shaderc compilations are around 70 ms each on a typical desktop, and a small UI scene easily compiles 15+ shaders, so eliminating that on warm runs cuts roughly a second off first-frame latency.

### How it works

Cached SPIR-V blobs live under the `shader_cache://` resource scheme, which `RenderContext::init()` registers as a `FileProtocol` pointing at `<cwd>/shader_cache/`. Each blob is a binary file named `<16-hex-key>.spv`.

The cache key for a shader combines:

- A 64-bit hash of the GLSL source. Built-in shaders pass this as a `constexpr make_hash64(source)` constant via the optional `key` parameter to `compile_shader()`. User shaders (e.g. anything passed to `create_shader_material()`) leave the parameter as 0 and the runtime hashes the source on the fly. The runtime hash cost is negligible (microseconds) at typical shader sizes.
- A stage discriminator so a vertex shader and fragment shader with identical source never collide.
- A 64-bit hash of all currently registered shader includes (sorted by name). Folding this into every per-shader key means any change to a virtual include such as `velk.glsl` or `velk-ui.glsl` automatically invalidates entries that depend on it. Old entries become orphans rather than corrupt cache hits, and the next compile rewrites them under the new key.

There is no version file and no bulk wipe — invalidation is implicit in the key.

### What is cached and what is not

Both built-in and user-supplied shaders go through the same cache. There is no special path for `create_shader_material()`: the same `compile_shader()` call site sees both, and both benefit on the second run. Cold runs (no cache, or cache wiped) still pay the full shaderc cost.

### Disabling the cache

Set the environment variable `VELK_SHADER_CACHE_DISABLED=1` to bypass the cache entirely. Useful when bisecting shaderc / driver issues. Deleting the `shader_cache/` directory also forces a clean rebuild on the next run.

## Application-defined materials (ext::Material)

Use `ext::Material<T>` when you control the shader and data layout directly. Define the GPU data struct, implement `gpu_data_size()` and `write_gpu_data()`, and the framework passes the data straight to the shader. This should be the default option for any shader code.

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
        // Only the fragment shader is needed; the default vertex shader is used.
        return ensure_pipeline(ctx, my_frag_src);
    }

    size_t gpu_data_size() const override { return sizeof(MyParams); }

    void write_gpu_data(void* out, size_t size) const override
    {
        if (auto state = read_state<IMyProps>(this)) {
            set_material<MyParams>(out, size, [&](auto& p) {
                p.color[0] = state->color.r;
                // ...
                p.intensity = state->intensity;
            });
        }
    }
};
```

The `ext::Material` base provides `ensure_pipeline()` which lazily compiles the shader on first use and caches the pipeline handle. When called with only a fragment source, the registered default vertex shader is used.

To provide a custom vertex shader (e.g. for non-standard instance layouts), pass both sources:

```cpp
return ensure_pipeline(ctx, my_frag_src, my_vert_src);
```

Or pass pre-compiled `IShader::Ptr` handles:

```cpp
return ensure_pipeline(ctx, my_compiled_frag, my_compiled_vert);
```

### Shader side

A material only needs a fragment shader. The default vertex shader provides `v_color` at location 0 and `v_local_uv` at location 1:

```glsl
// my_material_frag.glsl
#version 450
#include "velk.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(Ptr64)
    vec4 color;
    float intensity;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 1) in vec2 v_local_uv;
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
#include <velk-render/api/material/shader_material.h>

constexpr velk::string_view my_frag = R"(
#version 450
#include "velk.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(Ptr64)
    vec4 tint;
    float speed;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = root.tint * (0.5 + 0.5 * sin(v_uv.x * root.speed));
}
)";

// Create and configure (vertex shader is optional, uses default)
auto sm = velk::create_shader_material(ctx, my_frag);
sm.input<velk::color>("tint").set_value({1, 0.5f, 0, 1});
sm.input<float>("speed").set_value(10.f);
```

### How it works

1. `create_shader_material` compiles the GLSL to `IShader` handles via `compile_shader()` and links them into a pipeline via `create_pipeline()`. If no vertex source is given, the registered default vertex shader is used
2. The vertex shader SPIR-V is reflected to find fields in the `DrawData` struct after the standard 6 header fields (32 bytes)
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
| 0 | global_data | buffer_reference (pointer) | 8 |
| 8 | instance_data | buffer_reference (pointer) | 8 |
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

## Classes

| ClassId | Implements | Description |
|---|---|---|
| `velk::ClassId::ShaderMaterial` | `IMaterial` | Compiled GLSL shader material with reflected inputs. Construct via `velk::create_shader_material(ctx, frag_src, vert_src)`. |
| `velk::ClassId::Shader` | `IShader` | Compiled SPIR-V module produced by `IRenderContext::compile_shader()`. Used as a building block for custom pipelines. |

Plugin-provided materials live alongside their plugins:

| ClassId | Plugin | Description |
|---|---|---|
| `velk::ui::ClassId::Material::Gradient` | velk-ui | Linear gradient between two colors. |
| `velk::ui::ClassId::Material::Image` | velk_image | Texture sampler with tint. |
| `velk::ui::ClassId::Material::Environment` | velk_image | Skybox material for `IEnvironment`. |
| `velk::ui::ClassId::TextMaterial` | velk_text | Analytic Bezier glyph coverage shader. |
