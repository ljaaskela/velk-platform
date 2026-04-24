# glTF plugin

The glTF plugin (`velk_gltf`) loads glTF 2.0 assets and turns them into velk element trees with `IMesh` / `StandardMaterial` / `ITexture` ready to render. It provides one resource decoder:

  * **`gltf:`** parses `.gltf` / `.glb` bytes into an `IGltfAsset` that can be instantiated into a scene.

Parsing uses [cgltf](https://github.com/jkuhlmann/cgltf) (single-header, MIT); MikkTSpace is vendored for later tangent-generation work. Image bytes embedded in `.glb` files are routed through the image plugin via the `memory://` resource protocol.

## Contents
- [Usage](#usage)
- [Loading a glTF asset](#loading-a-gltf-asset)
- [Instantiating into a scene](#instantiating-into-a-scene)
- [How glTF maps to velk](#how-gltf-maps-to-velk)
- [Supported subset](#supported-subset)
- [Classes](#classes)

## Usage

The plugin is loaded automatically by `velk::create_app()`. `initialize()` registers `GltfAsset` and `GltfDecoder`, and registers the decoder with the resource store under `"gltf"`.

If you're not using the runtime, load it manually:

```cpp
velk::instance().plugin_registry().load_plugin_from_path("velk_gltf.dll");
```

## Loading a glTF asset

```cpp
#include <velk-ui/plugins/gltf/interface/intf_gltf_asset.h>

auto asset = velk::instance().resource_store()
    .get_resource<velk::ui::IGltfAsset>("gltf:app://assets/my_model.glb");
```

URIs use the decoder form `gltf:<inner_uri>` — the inner URI is resolved by the normal protocol path (`app://`, `file://`, `memory://`). The decoder parses JSON + buffers, uploads GPU meshes and materials, and returns a cached `IGltfAsset`. Subsequent requests with the same URI return the same asset until the last reference drops (the usual resource-store persistence rules apply: `asset->set_persistent(true)` pins it).

An `IGltfAsset` is a **template** — it holds shared GPU data (one `IMesh` per glTF mesh, one `StandardMaterial` per glTF material, one `ITexture` per glTF texture) but no live element tree. Elements are built on demand via `instantiate()`.

## Instantiating into a scene

```cpp
auto store = asset->instantiate();
if (store) {
    scene.load(*store, parent_element);
}
```

`instantiate()` builds a fresh tree of `IElement`s mirroring the glTF scene's node graph. Each call produces a disjoint element tree — call it once per place you want to show the asset. The underlying `IMesh` / `StandardMaterial` / `ITexture` objects are **shared** across instantiations, so the GPU upload happens exactly once.

The returned `IStore` follows the same convention as the JSON importer: a hierarchy under the key `"hierarchy:scene"`. Passing it to `IScene::load(IStore&, IElement* parent)`:

- with `parent == nullptr`: replaces the scene's root with the imported hierarchy.
- with `parent != nullptr`: grafts the imported hierarchy as a subtree of `parent`. Useful for dropping an asset at an attachment point in an existing scene.

The imported root element is tagged with the source `IGltfAsset` as a velk attachment; element-side code can reach the asset via `get_attachment<IGltfAsset>(root)`.

glTF coordinates / units are preserved; a `BoxTextured.glb` unit cube lands as a 1-unit element. Wrap the load in a parent element with a `Trs` if you need to rescale into your scene's coordinate convention.

## How glTF maps to velk

| glTF | velk |
|---|---|
| `buffer`, `bufferView`, `accessor` | `IMeshBuffer` (one per glTF mesh; primitives share the VBO + IBO via byte ranges). |
| `mesh.primitive` (POSITION / NORMAL / TEXCOORD_0) | Interleaved into the standard 32 B `VelkVertex3D`. |
| `mesh.primitive.TEXCOORD_1` | Parallel UV1 `IMeshBuffer` (second stream, same cardinality as the VBO). Honoured per-property via `IMaterialProperty::tex_coord`. |
| `mesh.primitive.indices` | Uint32 IBO (regardless of the accessor's component type). |
| `image` + `sampler` + `texture` | `ITexture` (`ISurface`) via the image plugin. Embedded `.glb` images are registered under `memory://gltf-...` and routed through the `image:` decoder. Sampler wrap / filter / mipmap settings flow into the `IImage`'s `SamplerDesc`, honoured per-texture by the backend. |
| `material` (pbrMetallicRoughness, normal, occlusion, emissive, specular) | `StandardMaterial` + one attached `IMaterialProperty` per slot (`BaseColorProperty`, `MetallicRoughnessProperty`, `NormalProperty`, `OcclusionProperty`, `EmissiveProperty`, `SpecularProperty`). |
| `KHR_texture_transform` | `uv_offset` / `uv_rotation` / `uv_scale` on each `IMaterialProperty`. |
| `KHR_materials_specular` | `SpecularProperty` (factor + color factor + two optional textures). |
| `KHR_materials_emissive_strength` | `EmissiveProperty.strength`. |
| `node.translation` / `rotation` / `scale` | `Trs` trait (vec3, quat, vec3). `matrix` nodes are decomposed into TRS. |
| `node.mesh` | `MeshVisual` trait referencing the cached `IMesh`. |
| `scene` | One synthetic root `IElement` wrapping the scene's root nodes (glTF scenes can have multiple root nodes; velk hierarchies have one). |

A glTF file with multiple scenes uses `scene[data.scene]` (the default) if present, otherwise the first.

## Supported subset

**In scope today:**

- `.glb` (single file with embedded buffers and images).
- Static meshes, per-node TRS transforms, material tree with KHR_texture_transform / KHR_materials_specular / KHR_materials_emissive_strength.
- Any glTF `extensionsRequired` not on the safelist above causes the load to fail cleanly.
- Per-texture sampler wrap / filter / mipmap from glTF sampler records.

**Not yet supported:**

- External `.gltf` + `.bin` + sibling images — only `.glb`.
- Skinning, animations, morph targets, cameras, punctual lights.
- Draco / meshopt compression.
- KTX2 / Basis textures.
- MikkTSpace tangent generation when the asset ships a normal map without TANGENT (vendored, not yet invoked).
- Per-slot sRGB / linear texture color space routing.
- Material variants (`KHR_materials_variants`).

## Classes

| ClassId | Implements | Description |
|---|---|---|
| `velk::ui::ClassId::GltfAsset` | `IGltfAsset`, `IResource` | Loaded glTF 2.0 asset — holds parsed cgltf state and shared GPU objects (meshes, materials, textures). `instantiate()` builds fresh element trees referencing the shared data. |
| `velk::ui::ClassId::GltfDecoder` | `IResourceDecoder` | Decodes raw `.glb` / `.gltf` bytes into `GltfAsset`. Registered with the resource store as `"gltf"` so URIs of the form `gltf:<inner_uri>` route through it. |

The plugin's `PluginId::GltfPlugin` is loaded automatically by `velk::create_app()` and registers the two classes plus the decoder.
