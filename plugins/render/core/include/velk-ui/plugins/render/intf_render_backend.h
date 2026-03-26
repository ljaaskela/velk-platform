#ifndef VELK_UI_INTF_RENDER_BACKEND_H
#define VELK_UI_INTF_RENDER_BACKEND_H

#include <velk/api/math_types.h>
#include <velk/array_view.h>
#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>

#include <cstdint>

namespace velk_ui {

/// Well-known pipeline keys used by the Renderer and consumed by backends.
namespace PipelineKey {
inline constexpr uint64_t Rect = 1;
inline constexpr uint64_t Text = 2;
inline constexpr uint64_t CustomBase = 1000;
} // namespace PipelineKey

/// Well-known vertex format keys. Backends use these to select VAO / vertex input state.
namespace VertexFormat {
inline constexpr uint64_t Untextured = 1;  ///< {x, y, w, h, r, g, b, a} = 32 bytes
inline constexpr uint64_t Textured = 2;    ///< {x, y, w, h, r, g, b, a, u0, v0, u1, v1} = 48 bytes
inline constexpr uint32_t UntexturedStride = 32;
inline constexpr uint32_t TexturedStride = 48;
} // namespace VertexFormat

/// Well-known texture keys.
namespace TextureKey {
inline constexpr uint64_t Atlas = 1;
} // namespace TextureKey

struct SurfaceDesc
{
    int width{};
    int height{};
};

struct PipelineDesc
{
    const char* vertex_source = nullptr;
    const char* fragment_source = nullptr;
    uint64_t vertex_format_key{};
};

/**
 * @brief Internal batch representation.
 *
 * Groups draw calls sharing the same GPU state. The renderer collects
 * batches dynamically; backends resolve opaque keys to GPU objects.
 */
struct RenderBatch
{
    uint64_t pipeline_key{};
    uint64_t vertex_format_key{};
    uint64_t texture_key{};
    velk::vector<uint8_t> instance_data;
    uint32_t instance_stride{};
    uint32_t instance_count{};

    /// Per-batch element rect (screen space). Set for custom material batches.
    velk::rect rect{};
    bool has_rect = false;
};

/**
 * @brief Backend contract for GPU rendering.
 *
 * Implemented by velk_gl (and future velk_vk). The renderer resolves
 * materials and packs batches; the backend compiles shaders, manages
 * GPU resources, and issues draw calls.
 */
class IRenderBackend : public velk::Interface<IRenderBackend>
{
public:
    virtual bool init(void* params) = 0;
    virtual void shutdown() = 0;

    virtual bool create_surface(uint64_t surface_id, const SurfaceDesc& desc) = 0;
    virtual void destroy_surface(uint64_t surface_id) = 0;
    virtual void update_surface(uint64_t surface_id, const SurfaceDesc& desc) = 0;

    virtual bool register_pipeline(uint64_t pipeline_key, const PipelineDesc& desc) = 0;
    virtual void upload_texture(uint64_t texture_key,
                                const uint8_t* pixels,
                                int width, int height) = 0;

    virtual void begin_frame(uint64_t surface_id) = 0;
    virtual void submit(velk::array_view<const RenderBatch> batches) = 0;
    virtual void end_frame() = 0;
};

} // namespace velk_ui

#endif // VELK_UI_INTF_RENDER_BACKEND_H
