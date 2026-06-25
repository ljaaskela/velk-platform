# Lighting

Lighting describes the light *sources* in a scene and how the renderer turns them into shaded pixels: direct lighting with soft shadows, plus image-based ambient lighting and a background sky from an environment map. Where [materials](materials.md) define how a surface responds to light, this document covers what produces that light.

Lights are scene traits (`ILight`) attached to elements, exactly like cameras. Shadows are opt-in per light via a pluggable shadow technique. Ambient / image-based lighting comes from an `IEnvironment` attached to the camera.

![The bistro sample: a sunlit street with soft contact shadows on the cobblestones, ~20 warm local lights, and sky-based ambient (image-based lighting). The panels on the right are G-buffer debug views.](bistro.jpg)

## Contents

- [Lights](#lights)
- [Shadows](#shadows)
- [Environment and image-based lighting](#environment-and-image-based-lighting)
- [The deferred lighting path](#the-deferred-lighting-path)
- [Classes](#classes)

## Lights

A light is a `RenderTrait` (`ILight`) attached to an element. The element's world transform supplies the light's position (its translation) and forward axis (for directional / spot); the trait carries only the intrinsic source properties. There are three kinds:

| `LightType` | Position | Direction | Notes |
|---|---|---|---|
| `Directional` | ignored | element forward axis | Infinite source (the sun). |
| `Point` | element translation | ignored | Omnidirectional, range-limited. |
| `Spot` | element translation | element forward axis | Cone, range-limited. |

Properties (`ILight`):

| Property | Default | Meaning |
|---|---|---|
| `type` | `Directional` | Light kind (above). |
| `color` | white | Linear RGB color. |
| `intensity` | `1.0` | Multiplier on `color`. |
| `range` | `1000` | Falloff distance for point / spot. Unused for directional. |
| `cone_inner_deg` | `20` | Spot inner half-angle (full intensity inside). |
| `cone_outer_deg` | `30` | Spot outer half-angle (falloff to zero). |
| `size` | `0` | Apparent source size, driving **soft-shadow penumbra width**. Directional: angular diameter in degrees (the real sun is ~0.53). Point / spot: world-space radius. `0` is a point source (a hard shadow). |

Create a light through the `trait::render` factories and attach it to an element:

```cpp
#include <velk-scene/api/trait/light.h>

// A sun: warm, strong, with a soft 2-degree penumbra.
velk::Light sun = velk::trait::render::create_directional_light(
    velk::color::white(), 4.0f);
sun.set_size(2.0f);                 // angular diameter in degrees

auto sun_elem = velk::create_element();
sun_elem.add_trait(sun);
// The element's orientation sets the sun direction.

// A warm point light (a bulb), 0.3 m radius for soft shadows.
velk::Light bulb = velk::trait::render::create_point_light(
    {1.0f, 0.8f, 0.5f, 1.0f}, 6.0f, /*range*/ 12.0f);
bulb.set_size(0.3f);                // world-space radius
```

The `Light` wrapper exposes a getter/setter for every property (`set_color`, `set_intensity`, `set_range`, `set_cone_inner_deg`, `set_cone_outer_deg`, `set_size`, ...), so a light can be re-tuned at runtime (e.g. dimming the sun and turning on the bulbs to switch a scene from a day to a night look).

## Shadows

A light casts **no shadow** until a shadow technique is attached to it. Attach one with `add_technique`:

```cpp
#include <velk-render/api/shadow_technique.h>

sun.add_technique(velk::technique::create_rt_shadow());
```

`create_rt_shadow()` returns the ray-traced shadow technique: occlusion is tested by tracing rays against the scene's geometry, so shadows are accurate regardless of light type and need no shadow-map setup or resolution tuning. Shadow casting is per light, so you can give the sun and a few key lights shadows while leaving fill lights unshadowed.

### Soft shadows

Shadows are **soft**: the penumbra widens with the light's `size` and with the distance between the occluder and the surface it shadows (a contact shadow is crisp, a shadow cast across a room is broad), matching how real area lights behave. Set `size` to `0` for a hard shadow.

Two behaviors follow from how the soft shadows are produced (see [the deferred lighting path](#the-deferred-lighting-path)):

- Shadows **converge over a few frames**. When the camera holds still the penumbra resolves to a smooth gradient; this is normal and not a glitch.
- A **fast-moving occluder** can leave a brief, faint trail in its shadow before it re-converges. This is the cost of keeping the shadow cost independent of light count; it is mild and only visible during fast motion.

## Environment and image-based lighting

An `IEnvironment` is an equirectangular HDR image that provides the scene's ambient / indirect light and its background sky. Attach one to the camera and the deferred and RT paths will:

- sample it as the **ambient / IBL** term, so surfaces a light does not directly reach are filled by skylight instead of going black;
- draw it as the **background sky** behind the geometry.

```cpp
#include <velk-ui/plugins/image/api/environment.h>
#include <velk-scene/api/camera.h>

velk::ui::Environment sky = velk::ui::load_environment("env:app://hdri/sky.hdr");
sky.set_intensity(1.5f);   // exposure multiplier
sky.set_rotation(45.f);    // Y-axis rotation in degrees

velk::Camera cam(camera_element.find_trait<velk::ICamera>());
cam.set_environment(sky);  // pass a null Ptr to clear
```

The `env:` URI prefix routes through the environment decoder, which loads the HDR via stb_image and stores it as `RGBA16F` with a mip chain (the mips act as a cheap roughness prefilter for the specular reflection term). The resource store caches by URI, so loading the same sky twice returns the same object. Swapping the camera's environment (e.g. a day sky for a dusk sky) changes the whole ambient look without touching geometry.

## The deferred lighting path

Lighting is evaluated in the deferred path. After the G-buffer is filled (albedo, normal, world position, material parameters, emissive), a compute pass shades every pixel. The shading is built to keep cost independent of the number of lights:

1. **Direct lighting.** For each pixel, one light is importance-sampled (weighted by its contribution) and a **single** soft shadow ray is traced for it, then weighted to give an unbiased estimate of all lights' contribution. So the shadow-ray cost is **one ray per pixel regardless of how many lights the scene has**: twenty lights cost the same as one. Specular is evaluated analytically for all lights.
2. **Ambient / IBL.** The environment supplies a diffuse irradiance term (modulated by ray-traced ambient occlusion) and a roughness-scaled specular reflection.
3. **Emissive.** Emissive surfaces add their radiance directly (and feed bloom, if the camera has it).

Because the single-ray direct term is noisy, the diffuse lighting is **denoised**: a temporal pass accumulates it across frames (reprojected by world position, so the history survives camera motion), and an edge-aware spatial filter cleans the residual while preserving shadow edges. This is what makes the soft shadows smooth, and it is the reason shadows converge over a few frames and fast occluders can briefly trail (above).

Shadow rays are traced against the same scene geometry whether the renderer is using the software ray-tracer or hardware ray queries; the soft-shadow ray jitter lives in the shared shadow snippet, so it applies to both the deferred and RT paths. The temporal + spatial denoiser is part of the deferred path.

For the renderer lifecycle these passes run within, see [rendering.md](rendering.md); for the GPU/bindless model they are built on, see [render-backend.md](render-backend.md).

## Classes

| ClassId | Implements | Description |
|---|---|---|
| `velk::ClassId::Render::Light` | `ILight` | A scene light source. Construct via `velk::trait::render::create_directional_light` / `create_point_light` / `create_spot_light`, then `element.add_trait(light)`. |
| `velk::ClassId::RtShadow` | `IShadowTechnique`, `IShaderSource` | Ray-traced shadow technique. Attach to a light with `light.add_technique(velk::technique::create_rt_shadow())`. A light with no technique casts no shadow. |

Environment maps are provided by the `velk_image` plugin:

| ClassId | Plugin | Description |
|---|---|---|
| `velk::ui::ClassId::Environment` | velk_image | Equirectangular HDR environment map (`IEnvironment`). Load via `velk::ui::load_environment("env:<uri>")` and attach with `Camera::set_environment`. |
