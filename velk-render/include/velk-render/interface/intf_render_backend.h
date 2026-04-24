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

/// Handle for a multi-attachment render target group (MRT).
/// Wraps a set of sampleable `TextureId`s sharing one Vulkan render
/// pass + framebuffer. Produced by `create_render_target_group`.
/// Encoding: high bit set to disambiguate from surface / texture IDs
/// in `begin_pass` dispatch.
using RenderTargetGroup = uint64_t;

inline constexpr uint64_t kRenderTargetGroupTag = 0x4000000000000000ULL;
inline constexpr bool is_render_target_group(uint64_t id)
{
    return (id & kRenderTargetGroupTag) != 0;
}
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
    size_t size{};            ///< Buffer size in bytes.
    bool cpu_writable{true};  ///< If false, the buffer is device-local only.
    bool index_buffer{false}; ///< If true, allocated with INDEX_BUFFER usage so it can be bound for indexed draws.
};

/// Texture usage hint.
enum class TextureUsage : uint8_t
{
    Sampled,         ///< Uploadable and samplable in shaders (default).
    RenderTarget,    ///< Renderable via begin_pass() and samplable. Format forced to match the surface so RTT passes can composite to the swapchain.
    Storage,         ///< Writable from compute (imageStore) and samplable in shaders.
    ColorAttachment  ///< Renderable with the explicit format. Used as an MRT group attachment; not swapchain-compatible.
};

/// Describes a texture to create.
struct TextureDesc
{
    int width{};                                    ///< Texture width in pixels.
    int height{};                                   ///< Texture height in pixels.
    int mip_levels{1};                              ///< Number of mip levels. upload_texture fills mip 0 and generates the rest via blit-downsampling.
    PixelFormat format{PixelFormat::RGBA8};          ///< Pixel format.
    TextureUsage usage{TextureUsage::Sampled};       ///< Usage hint.
    SamplerDesc sampler{};                           ///< Per-texture sampler state (wrap / filter / mipmap). Defaults to Repeat + Linear.
};

/// Primitive topology for pipeline creation.
enum class Topology : uint8_t
{
    TriangleList,  ///< 3 vertices per triangle (default; matches every IMesh today, including the unit quad).
    TriangleStrip, ///< Legacy strip mode; no current path uses it.
};

/// Non-shader pipeline state. Every knob the rasterizer and fragment
/// stage need is collected here so create_pipeline / compile_pipeline
/// signatures stay short as options grow.
struct PipelineOptions
{
    Topology  topology{Topology::TriangleList};             ///< Primitive assembly mode.
    CullMode  cull_mode{CullMode::None};                    ///< Face culling.
    FrontFace front_face{FrontFace::CounterClockwise};      ///< Winding that counts as front.
    BlendMode blend_mode{BlendMode::Alpha};                 ///< Color-attachment blend. Forced to Opaque on MRT groups.
    CompareOp depth_test{CompareOp::Disabled};              ///< Depth test op. Ignored if the target has no depth attachment.
    bool      depth_write{false};                           ///< Write depth. Ignored if the target has no depth attachment.
};

/// Describes a graphics pipeline to create.
struct PipelineDesc
{
    IShader::Ptr vertex;        ///< Vertex shader
    IShader::Ptr fragment;      ///< Fragment shader
    PipelineOptions options{};  ///< Rasterizer / depth / blend state.

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

/// Maximum push constant size in bytes. Vulkan's guaranteed minimum is
/// 128; 256 is supported by every modern desktop GPU (NVIDIA Turing+,
/// AMD RDNA2+, Intel Gen12+) and all tested AMD/NVIDIA mobile parts.
/// Bumped so the RT push constants can carry both the shape buffer and
/// the per-frame light buffer addresses in one dispatch.
inline constexpr size_t kMaxRootConstantsSize = 256;

/// A single draw call submitted to the backend.
///
/// When `index_buffer` is non-zero the call is dispatched as
/// `vkCmdDrawIndexed(index_count, ...)` after binding the IBO. Otherwise
/// it falls through to `vkCmdDraw(vertex_count, ...)`.
struct DrawCall
{
    PipelineId pipeline{};      ///< Which pipeline to bind.
    uint32_t vertex_count{};    ///< Vertices per instance for non-indexed draws.
    uint32_t instance_count{1}; ///< Number of instances to draw.

    GpuBuffer index_buffer{};         ///< Index buffer to bind (0 = non-indexed draw).
    uint64_t index_buffer_offset{};   ///< Byte offset into `index_buffer` where indices start.
    uint32_t index_count{};           ///< Indices per instance for indexed draws.

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
    DepthFormat depth{DepthFormat::None};               ///< Depth attachment for the swapchain.
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
    /// @name Multi-attachment render targets (MRT)
    /// @{

    /**
     * @brief Creates a multi-attachment render target group.
     *
     * Allocates one sampleable `TextureId` per entry in @p formats at
     * `width × height`, and a shared Vulkan render pass + framebuffer
     * binding all of them in the declared order. Shaders that draw to
     * the group declare `layout(location = N) out vec4` for each
     * attachment.
     *
     * The returned handle is the target passed to `begin_pass`. Each
     * attachment's `TextureId` is available via
     * `get_render_target_group_attachment(group, i)` — sample them
     * like any other bindless texture once the pass has ended.
     *
     * @return a `RenderTargetGroup` handle (high bit set), or 0 on failure.
     */
    virtual RenderTargetGroup create_render_target_group(
        array_view<const PixelFormat> formats, int width, int height,
        DepthFormat depth = DepthFormat::None) = 0;

    /** @brief Destroys a render target group, its attachments, render pass, and framebuffer. */
    virtual void destroy_render_target_group(RenderTargetGroup group) = 0;

    /**
     * @brief Returns the `TextureId` of attachment `index` in the group.
     *
     * The texture is sampleable in shaders (bindless) once the group's
     * render pass has ended and the attachment has been transitioned
     * to `SHADER_READ_ONLY_OPTIMAL` (done automatically by `end_pass`).
     *
     * @return 0 if the group or index is invalid.
     */
    virtual TextureId get_render_target_group_attachment(
        RenderTargetGroup group, uint32_t index) const = 0;

    /// @}
    /// @name Pipelines
    /// @{

    /**
     * @brief Creates a graphics pipeline from SPIR-V shaders. Returns a handle.
     *
     * @param target_group If non-zero, the pipeline is compiled against
     *        the render pass of the given group (so its fragment shader
     *        can write multiple color outputs). Defaults to the single-
     *        attachment swapchain render pass.
     */
    virtual PipelineId create_pipeline(const PipelineDesc& desc,
                                       RenderTargetGroup target_group = 0) = 0;

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
     * @brief Blits a source texture onto the swapchain image of @p surface_id.
     *
     * Acquires the swapchain image if not already acquired this frame, records
     * a vkCmdBlitImage that scales @p source into the destination rect, and
     * transitions the swapchain image to PRESENT_SRC_KHR. The source texture
     * must have been created with TextureUsage::Storage. Mutually exclusive
     * with begin_pass on the same surface within a frame.
     *
     * @param dst_rect Destination rect in surface pixels. Zero width/height
     *                 means "full surface".
     */
    virtual void blit_to_surface(TextureId source, uint64_t surface_id, rect dst_rect = {}) = 0;

    /**
     * @brief Copies the depth attachment of an MRT render target group into
     *        the surface's depth buffer.
     *
     * Used by the deferred compositor to plumb the G-buffer's depth into the
     * swapchain so subsequent forward draws onto the surface can depth-test
     * against the deferred scene. Source group must have been created with a
     * depth attachment; destination surface must have been created with
     * SurfaceDesc::depth != None.
     *
     * No-op if either side lacks a depth attachment. Handles layout
     * transitions; leaves both images back in DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
     *
     * @param dst_rect Destination rect in surface pixels. Zero width/height
     *                 means "full surface".
     */
    virtual void blit_group_depth_to_surface(RenderTargetGroup src_group,
                                             uint64_t surface_id,
                                             rect dst_rect = {}) = 0;

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
