#ifndef VELK_RENDER_INTF_RENDER_BACKEND_H
#define VELK_RENDER_INTF_RENDER_BACKEND_H

#include <velk/array_view.h>
#include <velk/interface/intf_metadata.h>

#include <cstddef>
#include <cstdint>

namespace velk {

// Handle types. 0 = null/invalid for all.
using GpuBuffer  = uint64_t;
using TextureId  = uint32_t; // also the bindless shader index
using PipelineId = uint64_t;

enum class PixelFormat : uint8_t
{
    RGBA8, // 4 bpp, standard color
    R8,    // 1 bpp, glyph atlases
};

struct GpuBufferDesc
{
    size_t size{};
    bool cpu_writable{true};
};

struct TextureDesc
{
    int width{};
    int height{};
    PixelFormat format{PixelFormat::RGBA8};
};

enum class Topology : uint8_t
{
    TriangleStrip,
    TriangleList,
};

struct PipelineDesc
{
    const uint32_t* vertex_spirv{};
    size_t vertex_spirv_size{};
    const uint32_t* fragment_spirv{};
    size_t fragment_spirv_size{};
    Topology topology{Topology::TriangleStrip};
};

inline constexpr size_t kMaxRootConstantsSize = 128;

struct DrawCall
{
    PipelineId pipeline{};
    uint32_t vertex_count{};
    uint32_t instance_count{1};
    uint8_t root_constants[kMaxRootConstantsSize]{};
    uint32_t root_constants_size{};
};

struct SurfaceDesc
{
    void* window_handle{};
    int width{};
    int height{};
};

/**
 * @brief Pointer-based GPU rendering backend.
 *
 * Designed around how modern GPUs work: buffer device addresses (pointers),
 * bindless textures, and push constants. No vertex input descriptions,
 * no uniform introspection, no descriptor set management.
 */
class IRenderBackend : public Interface<IRenderBackend>
{
public:
    virtual bool init(void* params) = 0;
    virtual void shutdown() = 0;

    virtual uint64_t create_surface(const SurfaceDesc& desc) = 0;
    virtual void destroy_surface(uint64_t surface_id) = 0;
    virtual void resize_surface(uint64_t surface_id, int width, int height) = 0;

    virtual GpuBuffer create_buffer(const GpuBufferDesc& desc) = 0;
    virtual void destroy_buffer(GpuBuffer buffer) = 0;
    virtual void* map(GpuBuffer buffer) = 0;
    virtual uint64_t gpu_address(GpuBuffer buffer) = 0;

    virtual TextureId create_texture(const TextureDesc& desc) = 0;
    virtual void destroy_texture(TextureId texture) = 0;
    virtual void upload_texture(TextureId texture,
                                const uint8_t* pixels, int width, int height) = 0;

    virtual PipelineId create_pipeline(const PipelineDesc& desc) = 0;
    virtual void destroy_pipeline(PipelineId pipeline) = 0;

    virtual void begin_frame(uint64_t surface_id) = 0;
    virtual void submit(array_view<const DrawCall> calls) = 0;
    virtual void end_frame() = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_BACKEND_H
