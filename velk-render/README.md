# velk-render

Pointer-based GPU rendering foundation for [velk-platform](../README.md). Namespace: `velk::`.

## Architecture

The render backend maps directly to how modern GPUs work: buffer device addresses (pointers), bindless textures, and push constants. The entire GPU interface is minimal (currently 15 methods). Shaders access all data through GPU pointers via `buffer_reference`, textures are bindless indices, and geometry is procedurally generated or vertex-pulled from buffers.

See [Render Backend Architecture](docs/render-backend-architecture.md) for the full technical writeup.

## Source structure

| Directory | Description |
|-----------|-------------|
| `include/velk-render/` | Public headers: IRenderBackend, IRenderContext, IMaterial, ISurface, GPU data types |
| `include/velk-render/api/` | Convenience wrappers: RenderContext, create_render_context() |
| `src/` | Implementation: RenderContext, shader compiler (shaderc), default shaders |
| `plugins/vk/` | Vulkan 1.2 backend: BDA, bindless descriptors, VMA, volk |

## Key interfaces

**IRenderBackend**: The actual API: Buffers (create/map/gpu_address), bindless textures, pipelines (SPIR-V in, handle out), frame submission (begin/submit/end).

**IRenderContext**: Backend lifecycle, surface creation, shader material compilation. Exposes the pipeline map for renderer use.

**IMaterial**: Interface which each material implements. Provides pipeline handle and GPU data written inline after the DrawDataHeader.

## Documentation

| Document | Description |
|----------|-------------|
| [Render Backend Architecture](docs/render-backend-architecture.md) | Design, data flow, shader model, technical details, Vulkan implementation |
