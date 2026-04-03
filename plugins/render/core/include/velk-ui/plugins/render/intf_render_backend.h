#ifndef VELK_UI_INTF_RENDER_BACKEND_H
#define VELK_UI_INTF_RENDER_BACKEND_H

#include <velk/array_view.h>
#include <velk/interface/intf_metadata.h>

#include <cstddef>
#include <cstdint>

namespace velk_ui {

// Handle types. 0 = null/invalid for all.
using GpuBuffer  = uint64_t;
using TextureId  = uint32_t;  // also the bindless shader index
using PipelineId = uint64_t;

enum class PixelFormat : uint8_t
{
    RGBA8,  // 4 bpp, standard color
    R8,     // 1 bpp, glyph atlases
};

struct GpuBufferDesc
{
    size_t size = 0;
    bool cpu_writable = true;  // false = device-local only
};

struct TextureDesc
{
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::RGBA8;
};

struct PipelineDesc
{
    const uint32_t* vertex_spirv = nullptr;
    size_t vertex_spirv_size = 0;
    const uint32_t* fragment_spirv = nullptr;
    size_t fragment_spirv_size = 0;
};

inline constexpr size_t kMaxRootConstantsSize = 128;

struct DrawCall
{
    PipelineId pipeline = 0;
    uint32_t vertex_count = 0;
    uint32_t instance_count = 1;

    // Pushed to the shader as push constants / setBytes.
    // Typically an 8-byte GPU address pointing to the draw's data struct,
    // but can hold up to 128 bytes for simple draws that skip the indirection.
    uint8_t root_constants[kMaxRootConstantsSize]{};
    uint32_t root_constants_size = 0;
};

struct SurfaceDesc
{
    void* window_handle = nullptr;
    int width = 0;
    int height = 0;
};

/**
 * @brief Pointer-based GPU rendering backend.
 *
 * Designed around how modern GPUs work: buffer device addresses (pointers),
 * bindless textures, and push constants. No vertex input descriptions,
 * no uniform introspection, no descriptor set management.
 *
 * Implemented by velk_vk (Vulkan 1.2) and future Metal backend.
 */
class IRenderBackend : public velk::Interface<IRenderBackend>
{
public:
    // Lifecycle
    virtual bool init(void* params) = 0;
    virtual void shutdown() = 0;

    // Surfaces (swapchains)
    virtual uint64_t create_surface(const SurfaceDesc& desc) = 0;
    virtual void destroy_surface(uint64_t surface_id) = 0;
    virtual void resize_surface(uint64_t surface_id, int width, int height) = 0;

    // GPU memory
    virtual GpuBuffer create_buffer(const GpuBufferDesc& desc) = 0;
    virtual void destroy_buffer(GpuBuffer buffer) = 0;
    virtual void* map(GpuBuffer buffer) = 0;
    virtual uint64_t gpu_address(GpuBuffer buffer) = 0;

    // Textures (bindless)
    virtual TextureId create_texture(const TextureDesc& desc) = 0;
    virtual void destroy_texture(TextureId texture) = 0;
    virtual void upload_texture(TextureId texture,
                                const uint8_t* pixels, int width, int height) = 0;

    // Pipelines
    virtual PipelineId create_pipeline(const PipelineDesc& desc) = 0;
    virtual void destroy_pipeline(PipelineId pipeline) = 0;

    // Frame
    virtual void begin_frame(uint64_t surface_id) = 0;
    virtual void submit(velk::array_view<const DrawCall> calls) = 0;
    virtual void end_frame() = 0;
};

} // namespace velk_ui

#endif // VELK_UI_INTF_RENDER_BACKEND_H
