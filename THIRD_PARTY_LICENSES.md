# Third-party licenses

velk-ui bundles or links the following third-party software:

| Component | Version | License | Location |
|-----------|---------|---------|----------|
| [GLFW](https://www.glfw.org/) | 3.4 | Zlib | `third_party/glfw-3.4.tar.gz` |
| [volk](https://github.com/zeux/volk) | via Vulkan SDK | MIT | Vulkan SDK headers |
| [VMA](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | via Vulkan SDK | MIT | Vulkan SDK headers |
| [shaderc](https://github.com/google/shaderc) | via Vulkan SDK | Apache 2.0 | Vulkan SDK shared library |
| [FreeType](https://freetype.org/) | 2.13.3 | FreeType License (BSD-style) | `plugins/text/third_party/freetype-2.13.3.zip` |
| [HarfBuzz](https://harfbuzz.github.io/) | 10.2.0 | MIT | `plugins/text/third_party/harfbuzz-10.2.0.zip` |
| [Inter](https://rsms.me/inter/) | 4.x | SIL Open Font License 1.1 | Embedded in `velk_text.dll` |
| [stb_image](https://github.com/nothings/stb) | latest (single header) | MIT or Public Domain (dual) | `plugins/image/third_party/stb/stb_image.h` |

## Inter font

The Inter typeface by Rasmus Andersson is embedded as the default font in the text plugin (`velk_text`). The full SIL Open Font License is at [`plugins/text/third_party/inter/OFL.txt`](plugins/text/third_party/inter/OFL.txt).

## FreeType

FreeType is distributed under the FreeType License (FTL), a BSD-style license. The full license is included in the FreeType source archive.

## HarfBuzz

HarfBuzz is distributed under the MIT license. The full license is included in the HarfBuzz source archive.

## GLFW

GLFW is distributed under the Zlib license. The full license is included in the GLFW source archive.

## volk

volk is a meta-loader for Vulkan, distributed under the MIT license.

## VMA (Vulkan Memory Allocator)

VMA is distributed under the MIT license by AMD GPUOpen.

## shaderc

shaderc is Google's GLSL to SPIR-V compiler, distributed under the Apache 2.0 license. Used as a shared library from the Vulkan SDK for runtime shader compilation.

## stb_image

`stb_image.h` is a single-header public domain (or MIT) image loader by Sean Barrett, vendored as `plugins/image/third_party/stb/stb_image.h`. Used by the image plugin (`velk_image`) to decode png/jpg/bmp/tga/gif/hdr/psd images into RGBA8 pixel data. The full dual license text is at the bottom of the header file itself.
