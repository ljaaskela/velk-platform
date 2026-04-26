# Materials

Materials define how geometry is shaded. Every material provides a pipeline (compiled shader) and optionally GPU data that the shader reads via buffer device address.

There are two ways to create materials:

- **ext::Material** for application-defined materials where you control the shader and data layout directly
- **ShaderMaterial** for dynamic materials where inputs are discovered automatically from the shader

## Contents
- [Where materials live](#where-materials-live)
- [Shader includes](#shader-includes)
- [Default shaders](#default-shaders)
- [Shader cache](#shader-cache)
  - [How it works](#how-it-works)
  - [What is cached and what is not](#what-is-cached-and-what-is-not)
  - [Disabling the cache](#disabling-the-cache)
- [Pipeline options](#pipeline-options)
- [StandardMaterial](#standardmaterial)
  - [Quick start](#quick-start)
  - [Property model](#property-model)
  - [Textures and UV transforms](#textures-and-uv-transforms)
  - [Available properties](#available-properties)
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


## Where materials live

The slot a material plugs into depends on whether the visual is 2D or 3D.

**2D visuals (rect, rounded rect, text, image, texture)** reference a material through `IVisual2D::paint`. One material per visual. Typically set via the API wrapper:

```cpp
velk::ui::RectVisual rect = velk::ui::trait::visual::create_rect();
rect.set_paint(velk::material::create_gradient(color::red(), color::blue(), 45.f));
```

**3D visuals (cube, sphere, future mesh / glTF)** carry no paint slot. Materials live on `IMeshPrimitive::material` instead, one per primitive. Creating a cube with a standard material looks like:

```cpp
velk::CubeVisual cube_vis = velk::trait::visual::create_cube();
velk::Mesh mesh(ctx.build_cube());
mesh.set_material(0, velk::material::create_standard(
    color{0.95f, 0.7f, 0.4f, 1.f}, /*metallic=*/0.9f, /*roughness=*/0.15f));
cube_vis.set_mesh(mesh);
```

See [mesh](mesh.md) for the container / primitive / buffer model behind `IMesh` and [traits](../ui/traits.md) for `Visual2D` / `Visual3D`.

The renderer treats both paths uniformly: each `DrawEntry` emitted by a visual carries its own `material` pointer, and the batch builder resolves materials per entry.

## Shader includes

Shader includes are registered via `IRenderContext::register_shader_include()`. Any module can register its own include, and shaders reference them with `#include "name"`.

**velk.glsl** (provided by velk-render, registered automatically):
- `GlobalData` — frame globals buffer reference (view_projection, inverse_view_projection, viewport, cam_pos, BVH metadata + shape/node arrays).
- `VelkVertex3D` — the unified vertex layout (`vec3 position`, `vec3 normal`, `vec2 uv`, 32 B tight scalar packing).
- `VelkVbo3D` — buffer reference to a `VelkVertex3D` array.
- `velk_vertex3d(root)` — macro that fetches the current `gl_VertexIndex` from the bound VBO.
- `OpaquePtr` — 8-byte buffer_reference placeholder for typed pointer fields the shader doesn't dereference (e.g. the material-data pointer in a vertex shader that doesn't care about the material).
- `VELK_DRAW_DATA(InstancesType, VboType)` — macro expanding to the 32-byte DrawDataHeader fields (see [DrawData struct layout](#drawdata-struct-layout)).
- `velk_texture(id, uv)` — bindless texture sample helper.
- `BvhNode`, `RtShape`, `Ray`, `RayHit` — ray-trace types used by shadow / bounce shaders.

**velk-ui.glsl** (provided by velk-scene, registered on renderer init):
- `ElementInstance` — the universal per-instance record shared by every visual: `mat4 world_matrix`, `vec4 offset`, `vec4 size`, `vec4 color`, `uvec4 params`.
- `ElementInstanceData` — buffer_reference array of `ElementInstance`.
- `EvalContext` — everything a material eval body receives: `data_addr`, `texture_id`, `shape_param`, `uv`, `base`, `ray_dir`, `normal`, `hit_pos`.
- `MaterialEval` — the canonical eval output: `color`, `normal`, `metallic`, `roughness`, `emissive`, `occlusion`, specular fields, `lighting_mode`.
- `velk_default_material_eval()` — returns a `MaterialEval` pre-filled with spec-correct defaults; eval bodies build on top so new fields get sensible values without every material tracking them.

Other modules can register their own includes. The text plugin registers `velk_text.glsl` (curve/band/glyph buffers + coverage sampling); application code can add its own via `IRenderContext::register_shader_include()`.

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

## Pipeline options

Every `IMaterial` carries an optional `IMaterialOptions` attachment that controls the pipeline's rasterizer / depth / blend state (cull mode, front-face winding, depth test / write, alpha mode + cutoff, etc.). It is the single channel through which per-material pipeline state flows — `create_pipeline` / `compile_pipeline` read it, and `ext::Material` subscribes to its `on_options_changed` event to invalidate the cached pipeline handle and trigger a recompile on next draw.

The `Material` wrapper exposes three entry points:

```cpp
#include <velk-render/api/material/standard_material.h>

auto m = velk::material::create_standard();

// Access / create the attachment. Returns a usable MaterialOptions
// wrapper; creates the attachment on first call so reads and writes
// both work.
m.options().set_depth_test(velk::CompareOp::LessEqual);
m.options().set_cull_mode(velk::CullMode::Back);

// Non-creating existence check.
if (m.has_options()) { /* ... */ }

// Batched write: a single StateWriter scope (one on_changed fire,
// one pipeline invalidation) — prefer this when setting several
// fields at once.
m.set_options([](velk::IMaterialOptions::State& s) {
    s.cull_mode   = velk::CullMode::None;
    s.depth_test  = velk::CompareOp::LessEqual;
    s.depth_write = true;
});
```

Defaults are defined on the `IMaterialOptions` interface itself (back-face cull, clockwise front-face, depth test + write on, opaque alpha). Materials that want a non-default pipeline state attach one and write the fields they care about; materials that leave the attachment unset fall back to `PipelineOptions` struct defaults (2D-safe: no cull, no depth, alpha blend) — use `m.options()` once (without writes) to materialize the attachment with 3D-oriented interface defaults.

Never access `IObjectStorage` / `add_attachment` directly from user code to configure options — always go through `options()` / `set_options()`.

### Winding / culling defaults

Three things have to agree for back-face culling to work. Velk's defaults are:

| | Value | Meaning |
|---|---|---|
| Authoring convention | CCW-from-outside | Mesh triangles are wound counter-clockwise around the outward-facing normal, in world space. Matches glTF 2.0 and the built-in `get_cube` / `get_sphere` meshes. |
| `FrontFace` pipeline state | `Clockwise` | The GPU treats triangles that appear clockwise *in the framebuffer* as front-facing. |
| `CullMode` pipeline state | `Back` | Triangles the GPU classifies as back-facing are dropped. |

At first glance these look contradictory: the mesh is authored CCW, but the pipeline is configured for CW front faces. The reason they agree is that velk uses a Y-down world together with an un-flipped projection into Vulkan's Y-down framebuffer. Y-down on both sides preserves the coordinate handedness from world to screen, which means a triangle authored CCW around an outward normal (in world space, right-handed) lands on screen wound *clockwise* around that same normal direction. So "CW in framebuffer = outward-facing = front" is exactly what the pipeline should classify as front-facing.

If you're used to a Y-up world (e.g. OpenGL defaults), the convention inverts and `FrontFace::CounterClockwise` is the expected setting. Velk picks the opposite because everything above the shader is already Y-down end to end:
* UI layout
* world coordinates
* projection 

Inserting a Y-flip somewhere in the chain just to restore a `CounterClockwise` default would be a cosmetic fix with real cost in matrix math everywhere else.

**Overriding for opposite-winding imports.** Meshes exported from a left-handed DCC tool, or authored CW-from-outside, just need the opposite pipeline setting:

```cpp
m.options().set_front_face(velk::FrontFace::CounterClockwise);
```

## StandardMaterial

`StandardMaterial` is the framework's built-in physically-based material and the usual choice for 3D content. It implements the glTF 2.0 metallic-roughness workflow plus the `KHR_materials_specular`, `KHR_materials_emissive_strength`, and `KHR_texture_transform` extensions. A glTF asset imports straight into one `StandardMaterial` per primitive without a translation layer.

It's named `StandardMaterial` to match Three.js / Unity / Unreal / Godot conventions, where "standard" has become the accepted term for the default PBR material.

### Quick start

```cpp
#include <velk-render/api/material/standard_material.h>

auto mat = velk::material::create_standard(
    /*base color*/ velk::color{0.95f, 0.7f, 0.4f, 1.f},
    /*metallic*/   0.9f,
    /*roughness*/  0.15f);
```

The factory seeds the three most common parameters and materialises an `IMaterialOptions` attachment carrying the 3D-oriented pipeline defaults (back-face cull, depth test + write, opaque). Without that attachment a material falls back to `PipelineOptions` struct defaults (2D-safe: no cull, no depth, alpha blend) and 3D meshes render in submission order. Every `StandardMaterial` you construct via the factory gets this right by default.

Attaching the material depends on the visual kind (see [Where materials live](#where-materials-live)):

```cpp
// 2D: paint on the visual.
rect.set_paint(mat);

// 3D: material on the primitive.
velk::Mesh cube_mesh(ctx.build_cube());
cube_mesh.set_material(0, mat);
cube_vis.set_mesh(cube_mesh);
```

### Property model

Material inputs are not fixed fields on `StandardMaterial`. Each input group (base color, metallic-roughness, normal map, occlusion, emissive, specular) is a separate `IMaterialProperty` object attached to the material. Reading or writing a group goes through an accessor that creates the property lazily on first use:

```cpp
velk::StandardMaterial mat = velk::material::create_standard();

// Scalar accessors: read/write the effective value on the canonical property.
mat.set_base_color(velk::color::red());
mat.set_metallic(0.0f);
mat.set_roughness(0.35f);

// Typed wrapper accessors: reach the full surface of a property (textures,
// UV transform, class-specific fields like normal scale).
mat.base_color().set_factor(velk::color{1, 0.5f, 0.2f, 1});
mat.normal().set_scale(0.8f);
mat.emissive().set_strength(3.0f);  // HDR emissive
```

Why attachments rather than flat PROPs? Three reasons that all matter for framework scale:

1. **Overrides layer naturally.** Attaching a second property of the same class leaves the first as a dormant override; the most recently attached wins ("last-wins"). A glTF variant or a runtime theme swap doesn't have to mutate the original.
2. **glTF extensions don't bloat the base interface.** `KHR_materials_clearcoat` or `KHR_materials_sheen` land as new `IMaterialProperty` subclasses; `IStandardMaterial` itself doesn't grow.
3. **Every property is a first-class velk object.** It's animatable, bindable, serialisable via the importer, and discoverable by an editor.

### Textures and UV transforms

Every property carries a common base surface: a bound texture, a `TEXCOORD_N` index selecting which UV set to sample, and a `KHR_texture_transform` triplet (offset / rotation / scale). Setting them looks the same on every property:

```cpp
auto img = velk::ui::image::load_image("image:app://assets/leather.png");

mat.base_color().set_texture(img.as_surface());
mat.base_color().set_tex_coord(0);
mat.base_color().set_uv_offset({0.25f, 0.0f});
mat.base_color().set_uv_rotation(0.0f);  // radians
mat.base_color().set_uv_scale({2.0f, 2.0f});
```

The sampled texture multiplies the factor, matching glTF semantics. When no texture is bound the sampler returns white, so setting only the factor gives a flat-colored surface without any texture setup.

### Available properties

| Accessor | Property class | glTF / extension | Class-specific fields |
|---|---|---|---|
| `mat.base_color()` | `BaseColorProperty` | `pbrMetallicRoughness.baseColor*` | `factor` (color) |
| `mat.metallic_roughness()` | `MetallicRoughnessProperty` | `pbrMetallicRoughness.{metallic,roughness}*` | `metallic_factor`, `roughness_factor` |
| `mat.normal()` | `NormalProperty` | `normalTexture` + `normalScale` | `scale` |
| `mat.occlusion()` | `OcclusionProperty` | `occlusionTexture` + `occlusionStrength` | `strength` |
| `mat.emissive()` | `EmissiveProperty` | `emissiveTexture` + `emissiveFactor`, `KHR_materials_emissive_strength` | `factor` (color), `strength` (HDR multiplier) |
| `mat.specular()` | `SpecularProperty` | `KHR_materials_specular` | `factor`, `color_factor` |

Each accessor returns an empty wrapper when the property hasn't been attached yet; setters attach it lazily. Use the wrappers directly for texture binding and UV transforms, or the scalar shortcuts on `StandardMaterial` (`set_base_color` / `set_metallic` / `set_roughness`) for the most common parameters.

## Application-defined materials (ext::Material)

Use `ext::Material<T>` when you write the shader yourself and control the GPU data layout. The framework provides pipeline handle storage, lazy compilation, and `IMaterialOptions`-driven invalidation out of the box; the derived class only supplies a shading body plus its per-draw GPU data.

### Eval bodies

A material's shading logic is a single GLSL function:

```glsl
MaterialEval velk_eval_my_material(EvalContext ctx);
```

The framework composes the full forward fragment shader, the deferred G-buffer shader, and the ray-trace fill shader around this one body.

`EvalContext` carries everything the body might need — `globals` (pointer to `FrameGlobals`), `data_addr` (pointer to the material's per-draw GPU data), `texture_id`, `shape_param`, `uv`, `base` (instance tint), `ray_dir`, `normal`, `hit_pos`. `MaterialEval` is the canonical output: `color`, `normal`, `metallic`, `roughness`, `emissive`, `occlusion`, specular fields, `lighting_mode`. Both types come from the `velk-ui.glsl` include and are documented in that file.

Start from `velk_default_material_eval()` and overwrite only the fields that matter to the material:

```glsl
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer MyMaterialData {
    vec4  color;
    float intensity;
};

MaterialEval velk_eval_my_material(EvalContext ctx)
{
    MyMaterialData d = MyMaterialData(ctx.data_addr);

    MaterialEval e = velk_default_material_eval();
    e.color  = d.color * d.intensity;
    e.normal = ctx.normal;
    return e;
}
```

No `main()`, no `gl_Position`, no `frag_color`. The body is pure shading logic; the driver templates handle the rest.

### Reading scene data from an eval body

The whole point of `EvalContext` is that an eval never reaches out to `root.global_data` / `root.instance_data` / `root.vbo` directly. Those live on the driver-composed fragment shader, and if an eval referenced them it would stop being portable across forward / deferred / RT. Everything an eval plausibly needs is pre-resolved into `EvalContext` by the driver before it's called.

| Field | Type | Source | What it carries |
|---|---|---|---|
| `globals` | `GlobalData` | frame globals buffer | Buffer reference to `FrameGlobals`: `view_projection`, `inverse_view_projection`, `viewport`, `cam_pos`, and the scene BVH (`bvh_nodes`, `bvh_shapes`, metadata). Dereference fields directly (`ctx.globals.cam_pos.xyz`). |
| `data_addr` | `uint64_t` | `DrawDataHeader` material pointer | GPU address of the material's per-draw data buffer. Cast to your material's buffer_reference type. |
| `texture_id` | `uint`   | `DrawDataHeader.texture_id`      | Bindless slot of the draw's primary texture. 0 when no texture is bound. Sample via `velk_texture(ctx.texture_id, uv)`. Materials with multiple named textures (StandardMaterial, custom multi-texture materials) embed their own `uint32_t` texture id fields in the material struct reached through `ctx.data_addr`. |
| `shape_param` | `uint`  | `ElementInstance.params[0]`      | Per-shape material data — glyph index for text, slot index for future custom uses. |
| `uv`          | `vec2`  | fragment interpolation / RT hit  | 0..1 shape coordinates at the shading point. |
| `base`        | `vec4`  | `ElementInstance.color`          | Visual-level tint (color set on the 2D visual, or white on 3D). Multiply this into your result when the material should respect per-instance color. |
| `ray_dir`     | `vec3`  | view / ray direction             | Normalised direction from camera/ray origin to the hit point. Use for Fresnel, view-dependent shading. |
| `normal`      | `vec3`  | world-space shading normal       | The surface normal at the shading point, already transformed into world space. Overwrite if you compute a bumped normal. |
| `hit_pos`     | `vec3`  | world-space shading position     | The world-space position of the shading point. Use for light-to-surface vectors, triplanar mapping, effects tied to world coordinates. |

Most geometric needs are already folded into the world-space fields (`ctx.ray_dir`, `ctx.hit_pos`, `ctx.normal`), so `ctx.globals` is only needed when an eval truly wants camera position, viewport, or BVH state directly. The BVH pointers are there if you know what you're doing, but evals are not the place to trace secondary rays — that's the RT driver's job. If you find yourself reaching for `ctx.globals.bvh_*` from inside an eval, step back and consider whether the work belongs on the ray-trace path instead.

A more elaborate eval showing texture sampling, per-instance tint, and a view-dependent tweak:

```glsl
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer MyParams {
    vec4  tint;
    float fresnel_power;
    float roughness;
};

MaterialEval velk_eval_my(EvalContext ctx)
{
    MyParams p = MyParams(ctx.data_addr);

    vec4 tex = velk_texture(ctx.texture_id, ctx.uv);    // bindless sample
    vec3 n = normalize(ctx.normal);
    float fresnel = pow(1.0 - max(dot(n, -ctx.ray_dir), 0.0), p.fresnel_power);

    MaterialEval e = velk_default_material_eval();
    e.color     = ctx.base * p.tint * tex;              // combine instance + material + texture
    e.roughness = p.roughness;
    e.emissive  = vec3(fresnel);                        // rim highlight
    e.normal    = n;
    return e;
}
```

### C++ side

```cpp
#include <velk-render/ext/material.h>
#include <velk-render/gpu_data.h>

VELK_GPU_STRUCT MyParams
{
    ::velk::color color;
    float         intensity;
    // No manual padding needed: VELK_GPU_STRUCT ensures 16-byte alignment.
};

class MyMaterial : public velk::ext::Material<MyMaterial, IMyProps>
{
public:
    VELK_CLASS_UID(ClassId::Material::My, "MyMaterial");

    // IDrawData: per-draw GPU data.
    size_t get_draw_data_size() const override { return sizeof(MyParams); }

    ReturnValue write_draw_data(void* out, size_t size,
                                ITextureResolver* /*resolver*/) const override
    {
        if (auto state = read_state<IMyProps>(this)) {
            return set_material<MyParams>(out, size, [&](auto& p) {
                p.color     = state->color;
                p.intensity = state->intensity;
            });
        }
        return ReturnValue::Fail;
    }

    // IMaterial: eval body + function name.
    string_view get_eval_src() const override     { return my_eval_src; }
    string_view get_eval_fn_name() const override { return "velk_eval_my_material"; }
};
```

The base's inherited `get_vertex_src()` returns the shared element vertex shader, which emits the canonical varying set every driver expects. Override it only if the material needs a non-standard vertex layout (e.g. fullscreen env, custom instancing).

`set_material<Params>(out, size, fn)` zero-initialises the buffer, confirms `size == sizeof(Params)`, and invokes `fn` with a `Params&` reference. Keeps the shader-side and C++-side layouts aligned at the call site.

Materials that sample textures override `get_textures()` to return the `ISurface*`s in slot order; the renderer resolves each to a bindless `TextureId` and makes them reachable from the eval body via `ctx.texture_id` and the generated header. See `velk-render/src/standard_material.cpp` for a multi-texture example.

### Full fragment shaders (advanced)

For materials that genuinely need a full fragment shader — `ShaderMaterial` is the canonical example, since it hosts user-supplied shaders — implement `get_fragment_src()` (and `get_vertex_src()`) and leave `get_eval_src()` empty. The batch builder sees the full-fragment path and compiles straight from those sources via `ensure_pipeline()`, bypassing the eval-driver composition.

```cpp
uint64_t get_pipeline_handle(IRenderContext& ctx) override
{
    return ensure_pipeline(ctx, my_frag_src, my_vert_src);
}
```

Pre-compiled `IShader::Ptr` handles work too:

```cpp
return ensure_pipeline(ctx, my_compiled_frag, my_compiled_vert);
```

On the shader side, a full fragment shader owns its own `DrawData` block and can reach every field the renderer writes — frame globals, the per-instance array, the bound VBO, and the material pointer:

```glsl
layout(buffer_reference, std430) readonly buffer MyParams {
    vec4 tint;
};

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(ElementInstanceData, VelkVbo3D)  // globals, instances, texture_id, count, vbo
    MyParams material;                              // per-draw material pointer
};

layout(push_constant) uniform PC { DrawData root; };

// Reads that an eval body can't make, but a full-fragment can:
//   root.instance_data.data[i].world_matrix   // other instances in the draw
//   root.vbo.data[gl_VertexIndex]              // raw VBO fetch
//   root.material.tint                         // material without the ctx.data_addr cast
//
// (root.global_data is also available here, but eval bodies can reach
// the same data via ctx.globals, so it's not a differentiator.)
```

Vertex shaders declare the same block (or override `get_vertex_src()` to use the shared `element_vertex_src`). Fragment shaders that don't touch instances or the VBO can use `OpaquePtr` for those slots to keep the layout intact without declaring the types.

This path is rare in first-party code. Prefer the eval body unless you're writing a material that cannot fit the eval contract (e.g. post-processing effects operating on a fullscreen quad).

## Shader materials (ShaderMaterial)

Use `ShaderMaterial` when the shader comes from outside your application code: Loaded from a file, provided by a user, or otherwise not known at compile time. The material discovers its parameters automatically via shader reflection and exposes them as dynamic properties on an inputs object, so you can set values without knowing the data layout in advance.

### Creating a shader material

```cpp
#include <velk-render/api/material/shader_material.h>

constexpr velk::string_view my_frag = R"(
#version 450
#include "velk.glsl"

layout(buffer_reference, std430) readonly buffer MyParams {
    vec4  tint;
    float speed;
};

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(OpaquePtr, OpaquePtr)  // fragment shader doesn't touch instances / VBO
    MyParams material;                    // pointer to the per-draw material buffer
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

void main()
{
    MyParams p = root.material;  // dereference the 8-byte pointer at offset 48
    frag_color = p.tint * (0.5 + 0.5 * sin(v_uv.x * p.speed));
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

Both material types share the same per-draw layout. Every draw call passes a single GPU pointer via push constants to a `DrawData` struct whose first 48 bytes are the standard `DrawDataHeader`:

```cpp
// velk-render/gpu_data.h
VELK_GPU_STRUCT DrawDataHeader
{
    uint64_t globals_address;    // -> FrameGlobals
    uint64_t instances_address;  // -> per-instance array
    uint32_t texture_id;         // bindless index, 0 = none
    uint32_t instance_count;
    uint64_t vbo_address;        // -> bound VBO (VelkVbo3D)
    uint64_t uv1_address;        // -> TEXCOORD_1 stream or fallback
    uint32_t uv1_enabled;        // 0 = fallback (index 0), 1 = per-vertex
    uint32_t _pad0;
};
static_assert(sizeof(DrawDataHeader) == 48, ...);
```

| Offset | Field | Type | Size |
|--------|-------|------|------|
| 0  | `globals_address`   | uint64 (buffer_reference) | 8 |
| 8  | `instances_address` | uint64 (buffer_reference) | 8 |
| 16 | `texture_id`        | uint32 | 4 |
| 20 | `instance_count`    | uint32 | 4 |
| 24 | `vbo_address`       | uint64 (buffer_reference) | 8 |
| 32 | `uv1_address`       | uint64 (buffer_reference) | 8 |
| 40 | `uv1_enabled`       | uint32 | 4 |
| 44 | `_pad0`             | uint32 | 4 |
| **48** | **material data pointer** | **uint64 (buffer_reference)** | **8** |

On the GLSL side, declare these fields with the `VELK_DRAW_DATA(InstancesType, VboType)` macro (expanded from `velk.glsl`); the shared `element_vertex_src` shows the canonical pattern:

```glsl
layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(ElementInstanceData, VelkVbo3D)
    OpaquePtr material;   // pointer to the material's per-draw data
};
```

Material-specific data is **not inlined after the header** — it lives in a separate `IProgramDataBuffer` (one per material, reused across frames with dirty-tracking) and the 8-byte pointer at offset 48 addresses it. The `ext::Material` base handles the buffer lifecycle: `write_draw_data` fills a scratch buffer, the base diffs it against the previous frame, and only re-uploads on change. The shader dereferences the pointer to reach the material's fields:

```glsl
layout(buffer_reference, std430) readonly buffer MyMaterialData {
    vec4  color;
    float intensity;
};

MaterialEval velk_eval_my(EvalContext ctx) {
    MyMaterialData d = MyMaterialData(ctx.data_addr);   // ctx.data_addr is the material pointer
    // ...
}
```

The C++ `MyParams` struct and the GLSL `MyMaterialData` fields must match in layout. Use `VELK_GPU_STRUCT` on the C++ side and std430 on the GLSL side to guarantee they agree.

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
| `velk::ClassId::StandardMaterial` | `IMaterial`, `IStandardMaterial` | glTF 2.0 metallic-roughness PBR material plus `KHR_materials_specular`, `KHR_materials_emissive_strength`, `KHR_texture_transform`. Construct via `velk::material::create_standard(base, metallic, roughness)`. Inputs are attached `IMaterialProperty` objects (`BaseColorProperty`, `MetallicRoughnessProperty`, `NormalProperty`, `OcclusionProperty`, `EmissiveProperty`, `SpecularProperty`). See [StandardMaterial](#standardmaterial). |
| `velk::ClassId::ShaderMaterial` | `IMaterial` | Compiled GLSL shader material with reflected inputs. Construct via `velk::create_shader_material(ctx, frag_src, vert_src)`. |
| `velk::ClassId::Shader` | `IShader` | Compiled SPIR-V module produced by `IRenderContext::compile_shader()`. Used as a building block for custom pipelines. |

Plugin-provided materials live alongside their plugins:

| ClassId | Plugin | Description |
|---|---|---|
| `velk::ui::ClassId::Material::Gradient` | velk-ui | Linear gradient between two colors. |
| `velk::ui::ClassId::Material::Image` | velk_image | Texture sampler with tint. |
| `velk::ui::ClassId::Material::Environment` | velk_image | Skybox material for `IEnvironment`. |
| `velk::ui::ClassId::TextMaterial` | velk_text | Analytic Bezier glyph coverage shader. |
