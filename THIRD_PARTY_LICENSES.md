# Third-party licenses

velk-ui bundles the following third-party software:

| Component | Version | License | Location |
|-----------|---------|---------|----------|
| [GLFW](https://www.glfw.org/) | 3.4 | Zlib | `third_party/glfw-3.4.tar.gz` |
| [GLAD 2](https://github.com/Dav1dde/glad) | 2.x | MIT / CC0 | `plugins/render/gl/third_party/glad/` |
| [FreeType](https://freetype.org/) | 2.13.3 | FreeType License (BSD-style) | `plugins/text/third_party/freetype-2.13.3.zip` |
| [HarfBuzz](https://harfbuzz.github.io/) | 10.2.0 | MIT | `plugins/text/third_party/harfbuzz-10.2.0.zip` |
| [Inter](https://rsms.me/inter/) | 4.x | SIL Open Font License 1.1 | Embedded in `velk_text.dll` |

## Inter font

The Inter typeface by Rasmus Andersson is embedded as the default font in the text plugin (`velk_text`). The full SIL Open Font License is at [`plugins/text/third_party/inter/OFL.txt`](plugins/text/third_party/inter/OFL.txt).

## FreeType

FreeType is distributed under the FreeType License (FTL), a BSD-style license. The full license is included in the FreeType source archive.

## HarfBuzz

HarfBuzz is distributed under the MIT license. The full license is included in the HarfBuzz source archive.

## GLFW

GLFW is distributed under the Zlib license. The full license is included in the GLFW source archive.

## GLAD

GLAD 2 generated loader code is public domain (CC0) or MIT at the user's choice.
