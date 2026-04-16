#ifndef VELK_RENDER_INTF_RENDER_BACKEND_H
#define VELK_RENDER_INTF_RENDER_BACKEND_H

#include <velk/api/math_types.h>
#include <velk/array_view.h>
#include <velk/interface/intf_metadata.h>

#include <cstddef>
#include <cstdint>
#include <velk-render/interface/intf_shader.h>
#include <velk-render/render_types.h>

namespace velk {

/// @name Handle types
/// Opaque handles returned by the backend. 0 is null/invalid for all.
/// @{
using GpuBuffer = uint64_t;
using TextureId = uint32_t; ///< Also the bindless shader index.
using PipelineId = uint64_t;
/// @}

/// Pixel format for textures.
enum class PixelFormat : uint8_t
{
    RGBA8,      ///< 4 bytes per pixel, linear color.
    RGBA8_SRGB, ///< 4 bytes per pixel, sRGB-tagged (auto-linearised on sample).
    R8,         ///< 1 byte per pixel, glyph atlases.
    RGBA16F,    ///< 8 bytes per pixel, 16-bit float per channel (HDR).
};

/// Describes a GPU buffer to create.
struct GpuBufferDesc
{
    size_t size{};           ///< Buffer size in bytes.
    bool cpu_writable{true}; ///< If false, the buffer is device-local only.
};

/// Texture usage hint.
enum class TextureUsage : uint8_t
{
    Sampled,      ///< Uploadable and samplable in shaders (default).
    RenderTarget, ///< Renderable via begin_pass() and samplable in shaders.
    Storage       ///< Writable from compute (imageStore) and samplable in shaders.
};

/// Describes a texture to create.
struct TextureDesc
{
    int width{};                                    ///< Texture width in pixels.
    int height{};                                   ///< Texture height in pixels.
    PixelFormat format{PixelFormat::RGBA8};          ///< Pixel format.
    TextureUsage usage{TextureUsage::Sampled};       ///< Usage hint.
};

/// Primitive topology for pipeline creation.
enum class Topology : uint8_t
{
    TriangleStrip, ///< 4 vertices per quad (default for UI).
    TriangleList,  ///< 3 vertices per triangle (meshes).
};

/// Describes a graphics pipeline to create.
struct PipelineDesc
{
    IShader::Ptr vertex;                        ///< Vertex shader
    IShader::Ptr fragment;                      ///< Fragment shader
    Topology topology{Topology::TriangleStrip}; ///< Primitive assembly mode.

    /// @brief Returns the size of vertex shader bytecode
    inline size_t get_vertex_size() const { return vertex ? vertex->get_data_size() : 0; }
    /// @brief Returns the size of fragment shader bytecode
    inline size_t get_fragment_size() const { return fragment ? fragment->get_data_size() : 0; }
    /// @brief Returns the vertex shader bytecode
    inline array_view<const uint32_t> get_vertex_data() const
    {
        return vertex ? vertex->get_data() : array_view<const uint32_t>{};
    }
    /// @brief Returns the fragment shader bytecode
    inline array_view<const uint32_t> get_fragment_data() const
    {
        return fragment ? fragment->get_data() : array_view<const uint32_t>{};
    }
};

/// Describes a compute pipeline to create.
struct ComputePipelineDesc
{
    IShader::Ptr compute; ///< Compute shader (SPIR-V).
};

/// Maximum push constant size in bytes (Vulkan guaranteed minimum).
inline constexpr size_t kMaxRootConstantsSize = 128;

/// A single draw call submitted to the backend.
struct DrawCall
{
    PipelineId pipeline{};      ///< Which pipeline to bind.
    uint32_t vertex_count{};    ///< Vertices per instance (e.g. 4 for a quad strip).
    uint32_t instance_count{1}; ///< Number of instances to draw.

    /// Push constant data, typically an 8-byte GPU pointer to a DrawDataHeader.
    uint8_t root_constants[kMaxRootConstantsSize]{};
    uint32_t root_constants_size{}; ///< Bytes used in root_constants.
};

/// A single compute dispatch submitted to the backend.
struct DispatchCall
{
    PipelineId pipeline{};           ///< Which compute pipeline to bind.
    uint32_t groups_x{1};            ///< Work group count in X.
    uint32_t groups_y{1};            ///< Work group count in Y.
    uint32_t groups_z{1};            ///< Work group count in Z.
    uint32_t root_constants_size{};  ///< Push constant bytes used.
    uint8_t root_constants[kMaxRootConstantsSize]{}; ///< Push constant bytes (compute stage).
};

/// Describes a render surface (swapchain target). Backend-facing.
struct SurfaceDesc
{
    void* window_handle{};                              ///< Native (HWND, ANativeWindow*)
    int width{};                                        ///< Initial surface width in pixels.
    int height{};                                       ///< Initial surface height in pixels.
    UpdateRate update_rate{UpdateRate::VSync};          ///< Swapchain pacing mode.
    int target_fps{60};                                 ///< Target framerate for UpdateRate::Targeted.
};

/// Pipeline stage for barrier synchronization.
enum class PipelineStage : uint32_t
{
    ColorOutput,    ///< Color attachment writes.
    FragmentShader, ///< Fragment shader reads.
    ComputeShader,  ///< Compute shader reads/writes.
    Transfer        ///< Transfer (copy) operations.
};

/**
 * @brief Bindless GPU rendering backend.
 *
 * Designed around how modern GPUs work: buffer device addresses (pointers),
 * bindless textures, and push constants. No vertex input descriptions,
 * no uniform introspection, no descriptor set management.
 *
 * See render-backend-architecture.md for the full design.
 */
class IRenderBackend : public Interface<IRenderBackend>
{
public:
    /// @name Lifecycle
    /// @{

    /** @brief Initializes the backend with platform-specific parameters. */
    virtual bool init(void* params) = 0;

    /** @brief Shuts down the backend and releases all GPU resources. */
    virtual void shutdown() = 0;

    /// @}
    /// @name Surfaces
    /// @{

    /** @brief Creates a swapchain surface. Returns the surface ID, or 0 on failure. */
    virtual uint64_t create_surface(const SurfaceDesc& desc) = 0;

    /** @brief Destroys a surface and its swapchain. */
    virtual void destroy_surface(uint64_t surface_id) = 0;

    /** @brief Recreates the swapchain for the given surface at the new dimensions. */
    virtual void resize_surface(uint64_t surface_id, int width, int height) = 0;

    /// @}
    /// @name GPU Memory
    /// @{

    /** @brief Allocates a GPU buffer. Returns a handle, or 0 on failure. */
    virtual GpuBuffer create_buffer(const GpuBufferDesc& desc) = 0;

    /** @brief Frees a GPU buffer. */
    virtual void destroy_buffer(GpuBuffer buffer) = 0;

    /** @brief Returns a persistently mapped CPU pointer to the buffer, or nullptr. */
    virtual void* map(GpuBuffer buffer) = 0;

    /** @brief Returns the GPU virtual address for use in shaders via buffer_reference. */
    virtual uint64_t gpu_address(GpuBuffer buffer) = 0;

    /// @}
    /// @name Textures
    /// @{

    /** @brief Creates a texture and assigns a bindless index. Returns the TextureId. */
    virtual TextureId create_texture(const TextureDesc& desc) = 0;

    /** @brief Destroys a texture and frees its bindless slot. */
    virtual void destroy_texture(TextureId texture) = 0;

    /** @brief Uploads pixel data to a texture via a staging buffer. */
    virtual void upload_texture(TextureId texture, const uint8_t* pixels, int width, int height) = 0;

    /// @}
    /// @name Pipelines
    /// @{

    /** @brief Creates a graphics pipeline from SPIR-V shaders. Returns a handle. */
    virtual PipelineId create_pipeline(const PipelineDesc& desc) = 0;

    /** @brief Creates a compute pipeline from a compute shader. Returns a handle. */
    virtual PipelineId create_compute_pipeline(const ComputePipelineDesc& desc) = 0;

    /** @brief Destroys a pipeline (graphics or compute). */
    virtual void destroy_pipeline(PipelineId pipeline) = 0;

    /// @}
    /// @name Frame submission
    /// @{

    /** @brief Begins a new frame: waits for GPU fence and starts command buffer recording. */
    virtual void begin_frame() = 0;

    /**
     * @brief Begins a render pass targeting the given surface or texture.
     *
     * For surface targets, acquires the swapchain image. Begins the Vulkan
     * render pass and binds the bindless descriptor set.
     */
    virtual void begin_pass(uint64_t target_id) = 0;

    /**
     * @brief Records draw calls into the current render pass.
     * @param calls    Draw calls to record.
     * @param viewport Viewport and scissor rect. Zero width/height means full target.
     */
    virtual void submit(array_view<const DrawCall> calls, rect viewport = {}) = 0;

    /** @brief Ends the current render pass. */
    virtual void end_pass() = 0;

    /**
     * @brief Records compute dispatches outside of any render pass.
     *
     * Call between frames (after begin_frame, outside any begin_pass/end_pass
     * window). Emits a memory barrier before sampled reads of storage-image
     * outputs in subsequent graphics passes.
     */
    virtual void dispatch(array_view<const DispatchCall> calls) = 0;

    /**
     * @brief Inserts a pipeline barrier between passes.
     *
     * Call between end_pass() and the next begin_pass() to synchronize
     * GPU work (e.g. after rendering to a texture and before sampling it).
     */
    virtual void barrier(PipelineStage src, PipelineStage dst) = 0;

    /** @brief Ends command recording, submits to GPU queue, and presents any surfaces used. */
    virtual void end_frame() = 0;

    /// @}
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_BACKEND_H
