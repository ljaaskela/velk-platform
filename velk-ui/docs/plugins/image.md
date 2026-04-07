# Image plugin

The image plugin (`velk_image`) decodes raster images (png, jpg, bmp, tga, gif, hdr, psd) into GPU textures and exposes them as `IImage` resources via the resource store. It also provides `ImageMaterial` for sampling textures in custom visuals and `ImageVisual` as a one-line "draw an image at this URI" convenience.

Image decoding is done with [stb_image](https://github.com/nothings/stb) (single-header, public domain), vendored under the plugin.

## Contents

- [Usage](#usage)
- [Loading an image](#loading-an-image)
  - [URIs and decoders](#uris-and-decoders)
  - [Persistence](#persistence)
  - [Failed loads](#failed-loads)
- [Drawing an image](#drawing-an-image)
  - [ImageVisual](#imagevisual)
  - [ImageMaterial](#imagematerial)
- [JSON declaration](#json-declaration)
- [Lifetime and the renderer](#lifetime-and-the-renderer)
- [Reference](#reference)
  - [IImage](#iimage)
  - [Image](#image)
  - [ImageDecoder](#imagedecoder)
  - [IImageMaterial / ImageMaterial](#iimagematerial--imagematerial)
  - [IImageVisual / ImageVisual](#iimagevisual--imagevisual)

## Usage

Load the plugin like any other:

```cpp
velk::instance().plugin_registry().load_plugin_from_path("velk_image.dll");
```

The plugin's `initialize()` registers the four classes (`Image`, `ImageDecoder`, `ImageMaterial`, `ImageVisual`) and registers `ImageDecoder` with the resource store under the name `"image"`. After load, any code can fetch images by URI.

## Loading an image

```cpp
#include <velk-ui/plugins/image/api/image.h>

auto img = velk::ui::Image::load("image:app://images/logo.png");
if (img.is_loaded()) {
    // ready to bind
}
```

A second call with the same URI returns the **same** underlying `IImage` while at least one consumer holds it. The decoded image lives in the resource store's dedup cache. When the last reference drops, the cache slot becomes a dead `weak_ptr` and the next call reloads.

`Image` is a thin C++ wrapper around `IImage::Ptr`. It is implicitly convertible back to `IImage::Ptr`, and `as_texture()` returns the same object as `ITexture::Ptr` (since the concrete `Image` class implements both interfaces).

### URIs and decoders

The image plugin registers an `IResourceDecoder` named `"image"`. Image URIs use the decoder form `image:<inner_uri>`, where `<inner_uri>` is any normal protocol URI:

```
image:app://images/logo.png      # via app:// (working directory)
image:file://C:/assets/logo.png  # via file://
image:assets://logo.png          # via a custom assets:// protocol
```

The resource store splits on the first `:`, finds the `image` decoder, resolves the inner URI through the protocol path to get raw bytes (`IFile`), runs the decoder, and returns the resulting `IImage`. See [resources](../../../velk/docs/resources.md) for the full decoder model.

### Persistence

By default, an image lives only as long as a consumer holds it. To pin an image in the cache so it survives reference drops:

```cpp
auto img = velk::ui::Image::load("image:app://images/logo.png");
img.set_persistent(true);
```

The next call to `Image::load` (or any other `get_resource`) reconciles the cache: pinned images stay alive even when no other reference exists. Unpin by calling `set_persistent(false)`; the next cache touch drops the strong ref.

This is most useful for assets you want available throughout the app's lifetime (logos, common icons, default avatars).

### Failed loads

If decoding fails partway through (valid file, corrupt payload), `decode()` returns a non-null `Image` with `status() == ImageStatus::Failed` and a zero GPU handle. This is intentional:

- Many visuals referencing the same broken URI cause one log line and one cached failure entry, not N retries per frame.
- The failure is cached like a successful image. Drop all references and the next request retries the load (the slot becomes a dead `weak_ptr`).
- `ImageVisual` and `ImageMaterial` check the status before binding: a failed image renders nothing rather than crashing.

If the inner protocol returns nothing at all (URI scheme unknown, file missing in some implementations), `get_resource` returns `nullptr` directly. There is nothing to cache.

## Drawing an image

### ImageVisual

The shortcut. Set a URI, get an image on screen:

```cpp
#include <velk-ui/plugins/image/api/image_visual.h>

auto img = velk::ui::visual::create_image("image:app://images/logo.png");
img.set_tint(velk::color::white());
element.add_attachment(img);
```

`ImageVisual` internally:

1. On `uri` change, calls `instance().resource_store().get_resource<IImage>(uri)`.
2. Lazily creates an internal `ImageMaterial` and wires it as the visual's `paint`.
3. Forwards the `tint` property to the material.
4. Emits a textured-quad draw entry with the image bound as the texture.

### ImageMaterial

The building block. Use directly when you want to apply the same image to a custom visual or animate the tint without going through `ImageVisual`. The `texture` property is `ObjectRef<ITexture>`, so any `ITexture` works: a decoded `Image` (which implements both `IImage` and `ITexture`), a glyph atlas, a future render target.

```cpp
#include <velk-ui/plugins/image/api/image.h>
#include <velk-ui/plugins/image/api/image_material.h>

auto img = velk::ui::Image::load("image:app://images/logo.png");

auto mat = velk::ui::material::create_image(img);
mat.set_tint(velk::color::white());

rect_visual.set_paint(mat);
```

The material samples the bound texture at the quad UV and multiplies by `tint`. Default tint is white (no recoloring). `material::create_image()` also has a no-arg form (texture set later via `set_texture`) and an `ITexture::Ptr` overload for non-image textures.

## JSON declaration

The image plugin works with the importer's resource and visual blocks. Two patterns:

**Inline ImageVisual on an element**, the simplest case:

```json
{
  "objects": [
    { "id": "logo", "class": "velk-ui.Element" }
  ],
  "attachments": [
    { "targets": ["logo"], "class": "velk-ui.FixedSize",
      "properties": { "width": "64px", "height": "64px" } },
    { "targets": ["logo"], "class": "velk_image.ImageVisual",
      "properties": { "uri": "image:app://scenes/images/velk-icon.png" } }
  ]
}
```

`ImageVisual` does its own URI fetch on first frame; nothing needs to be in the `resources` block.

**Pre-loaded resource with persistence**, using the [no-class decoder form](../../../velk/docs/plugins/importer.md) added to the importer:

```json
{
  "resources": [
    { "id": "logo", "uri": "image:app://images/logo.png", "persistent": true }
  ]
}
```

The importer recognizes resource entries without a `class` field and routes them through the resource store. The returned `IResource` is registered under `resource:logo` and can be referenced later via `{ "ref": "resources.logo" }`. The `persistent` flag is applied to the loaded image.

## Lifetime and the renderer

The renderer holds **weak references** to textures it has uploaded. The renderer never extends an image's lifetime: when the last `IImage::Ptr` drops, the image dies on whichever thread released it. The image's destructor synchronously notifies its observers (the renderer is one), and the renderer enqueues the GPU handle for deferred destruction once the last frame which uses the image has been presented.

This means:

- Calling `image.reset()` from the UI thread while a frame is in flight is safe. The CPU object goes away immediately; the GPU handle survives until it is no longer needed.
- `set_persistent(false)` followed by dropping all references actually frees memory promptly — the renderer is not a hidden owner.
- `set_persistent(true)` keeps the image alive even with no other consumers, by anchoring it in the resource store cache.

For the underlying `IGpuResource` / `IGpuResourceObserver` mechanism, see the renderer documentation.

## Reference

### IImage

Header: `velk-render/interface/intf_image.h`

| Method | Description |
|---|---|
| `status()` | Returns `Unloaded`, `Loading`, `Loaded`, or `Failed`. Sync v1 only ever returns the last two. |

Inherits `IResource`, so it also exposes `uri()`, `exists()`, `size()`, `is_persistent()`, `set_persistent()`.

### Image

The concrete class produced by `ImageDecoder`. Implements both `IImage` and `ITexture`. After the renderer uploads it, the CPU pixel buffer is freed (`get_pixels()` returns `nullptr`); the GPU handle is the only remaining surface for binding.

`ClassId::Image` (UID-based; use the constant from `velk-ui/plugins/image/plugin.h`).

### ImageDecoder

`IResourceDecoder` registered as `"image"`. Casts the inner `IResource` to `IFile`, reads bytes, calls `stbi_load_from_memory` (forced to RGBA8), constructs an `Image` with format `RGBA8_SRGB`, returns it. On partial failure (header parsed, decode failed) returns a non-null `Image` with `Failed` status. On total reject (`inner` is not an `IFile`) returns `nullptr`.

Registered with the resource store automatically in `ImagePlugin::initialize()`.
