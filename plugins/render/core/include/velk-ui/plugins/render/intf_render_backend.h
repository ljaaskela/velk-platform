#ifndef VELK_UI_INTF_RENDER_BACKEND_H
#define VELK_UI_INTF_RENDER_BACKEND_H

#include <velk/api/math_types.h>
#include <velk/array_view.h>
#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>

#include <velk-ui/types.h>

#include <cstdint>

namespace velk_ui {

// PipelineKey and TextureKey constants are defined in velk-ui/types.h.

enum class VertexAttribType : uint8_t
{
    Float,      ///< 1 float  (4 bytes)
    Float2,     ///< 2 floats (8 bytes)
    Float3,     ///< 3 floats (12 bytes)
    Float4,     ///< 4 floats (16 bytes)
};

struct VertexAttribute
{
    uint32_t location{};
    uint32_t offset{};          ///< Byte offset within stride.
    VertexAttribType type{};
};

struct VertexInputDesc
{
    uint32_t stride{};
    velk::vector<VertexAttribute> attributes;
};

struct SurfaceDesc
{
    int width{};
    int height{};
};

/** @brief Descriptor for a shader uniform extracted from SPIR-V reflection. */
struct UniformInfo
{
    velk::string name;   ///< Uniform name in the shader (e.g. "u_start_color").
    velk::Uid typeUid;   ///< Mapped velk type UID (float, color/vec4, mat4).
    int location{-1};    ///< Layout location (GL) or push constant offset (Vulkan).
};

struct PipelineDesc
{
    const uint32_t* vertex_spirv = nullptr;
    size_t vertex_spirv_size = 0;
    const uint32_t* fragment_spirv = nullptr;
    size_t fragment_spirv_size = 0;
    VertexInputDesc vertex_input;
    velk::vector<UniformInfo> uniforms; ///< Uniform metadata extracted from SPIR-V reflection.
};

/** @brief A uniform value to be set on the GPU before a draw call. */
struct UniformValue
{
    int location{-1};
    velk::Uid typeUid;
    float data[16]{};    ///< Enough for mat4; smaller types use a prefix.
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
    uint64_t texture_key{};
    velk::vector<uint8_t> instance_data;
    uint32_t instance_stride{};
    uint32_t instance_count{};

    /// Per-batch uniform values for material properties.
    velk::vector<UniformValue> uniforms;

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

    /** @brief Returns the active uniforms for a registered pipeline. */
    virtual velk::vector<UniformInfo> get_pipeline_uniforms(uint64_t pipeline_key) const = 0;
    virtual void upload_texture(uint64_t texture_key,
                                const uint8_t* pixels,
                                int width, int height) = 0;

    virtual void begin_frame(uint64_t surface_id) = 0;
    virtual void submit(velk::array_view<const RenderBatch> batches) = 0;
    virtual void end_frame() = 0;
};

} // namespace velk_ui

#endif // VELK_UI_INTF_RENDER_BACKEND_H
