#ifndef VELK_RENDER_INTF_RENDER_BACKEND_H
#define VELK_RENDER_INTF_RENDER_BACKEND_H

#include <velk/api/math_types.h>
#include <velk/array_view.h>
#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>

#include <cstddef>
#include <cstdint>
#include <velk-render/interface/intf_gpu_buffer.h>
#include <velk-render/interface/intf_gpu_command_buffer.h>
#include <velk-render/interface/intf_gpu_pipeline.h>
#include <velk-render/interface/intf_gpu_texture.h>
#include <velk-render/interface/intf_render_texture_group.h>
#include <velk-render/interface/intf_shader.h>
#include <velk-render/render_types.h>

namespace velk {

/// @name Handle types
/// Opaque handles returned by the backend. 0 is null/invalid for all.
/// @{
using TextureId = uint32_t; ///< Also the bindless shader index.

/// "Unknown" frame completion marker.
inline constexpr uint64_t kDefaultCompletionMarker = uint64_t(-1);
/// @}

// PixelFormat moved to render_types.h alongside sibling format enums
// (DepthFormat etc.); included transitively via the headers above.

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

/// Describes a multi-attachment render-target group (MRT) to create.
struct TextureGroupDesc
{
    array_view<const PixelFormat> formats;           ///< Color attachment formats, in declaration order.
    int width{};                                     ///< Width in pixels (shared by all attachments).
    int height{};                                    ///< Height in pixels.
    DepthFormat depth{DepthFormat::None};            ///< Optional depth attachment format.
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

/// Maximum push constant size in bytes. The minimum guaranteed by
/// modern GPU APIs is 128; 256 is supported by every modern desktop
/// GPU (NVIDIA Turing+, AMD RDNA2+, Intel Gen12+) and all tested
/// AMD/NVIDIA mobile parts. Bumped so the RT push constants can carry
/// both the shape buffer and the per-frame light buffer addresses in
/// one dispatch.
inline constexpr size_t kMaxRootConstantsSize = 256;

/// A single indirect draw call submitted to the backend.
///
/// Always dispatched indirectly with the actual draw count read from
/// `count_buffer` at GPU execution time, allowing future GPU-side
/// culling to write the count without CPU involvement. Today the CPU
/// writes count = 1 and a single indirect-draw record per batch into
/// `args_buffer`. Record layout (matches the GPU's draw-indirect
/// hardware):
///   - indexed:     `index_count, instance_count, first_index, vertex_offset, first_instance`
///   - non-indexed: `vertex_count, instance_count, first_vertex, first_instance`
struct DrawCall
{
    IGpuPipeline* pipeline{};   ///< Which pipeline to bind. Lifetime owned by the renderer's pipeline cache.
    bool indexed{false};        ///< true => indexed draw (uses `index_buffer`).

    IGpuBuffer* index_buffer{};       ///< Index buffer to bind. Required when `indexed`.
    uint64_t index_buffer_offset{};   ///< Byte offset into `index_buffer`.

    IGpuBuffer* args_buffer{};        ///< Buffer holding indirect-draw records.
    uint64_t args_buffer_offset{};    ///< Byte offset of the first record.
    uint32_t args_stride{};           ///< Bytes per indirect-draw record (5×u32 indexed, 4×u32 non-indexed).

    IGpuBuffer* count_buffer{};       ///< Buffer holding the uint32 actual draw count.
    uint64_t count_buffer_offset{};   ///< Byte offset of the count value.
    uint32_t max_draw_count{1};       ///< Upper bound; backend issues min(count_buffer[0], max_draw_count) draws.

    /// Push constant data, typically an 8-byte GPU pointer to a DrawDataHeader.
    uint8_t root_constants[kMaxRootConstantsSize]{};
    uint32_t root_constants_size{}; ///< Bytes used in root_constants.
};

/// A single compute dispatch submitted to the backend.
struct DispatchCall
{
    IGpuPipeline* pipeline{};        ///< Which compute pipeline to bind. Lifetime owned by the cache.
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

    /** @brief Blocks until all submitted GPU work has completed.
     *
     * Heavy-handed (`vkDeviceWaitIdle`-style); intended for shutdown
     * paths and as a debug fallback when proper per-submit fencing is
     * not yet wired up. Production frame-pacing should not call this.
     */
    virtual void wait_idle() = 0;

    /** @brief Returns a monotonically increasing marker tagging the GPU
     *         work submitted by the most recent `end_frame()` call.
     *
     * Stamp the returned value on whatever per-frame state needs to
     * outlive the frame's GPU work (frame slot, deferred-destroy queue
     * entries) and pass it back to `wait_for_frame_completion` before
     * touching that state again. Returns 0 if no frame has been
     * submitted yet.
     */
    virtual uint64_t frame_completion_marker() const = 0;

    /** @brief Blocks until the GPU work tagged with @p marker has
     *         completed. @p marker == 0 is a no-op. Markers from prior
     *         frames remain valid forever (they only resolve in the past).
     */
    virtual void wait_for_frame_completion(uint64_t marker) = 0;

    /** @brief Marker the next `end_frame()` submit will signal.
     *
     * Use this to tag deferred-destroy entries for resources still
     * referenced by the in-flight frame: once the returned marker
     * resolves, that frame's GPU work has finished and the resource
     * is safe to destroy.
     */
    virtual uint64_t pending_frame_completion_marker() const = 0;

    /** @brief Non-blocking query: has GPU work tagged with @p marker
     *         finished? @p marker == 0 returns true.
     */
    virtual bool is_frame_complete(uint64_t marker) const = 0;

    /// @}
    /// @name Surfaces
    /// @{

    /** @brief Creates a swapchain surface. Returns the surface ID, or 0 on failure. */
    virtual uint64_t create_surface(const SurfaceDesc& desc) = 0;

    /** @brief Destroys a surface and its swapchain. */
    virtual void destroy_surface(uint64_t surface_id) = 0;

    /** @brief Recreates the swapchain for the given surface at the new dimensions. */
    virtual void resize_surface(uint64_t surface_id, int width, int height) = 0;

    /// True if @p id refers to a swapchain surface; false for textures
    /// or unknown ids. Producers consult this to decide whether a
    /// blit destination can be baked into a cached secondary command
    /// buffer (textures: yes; surfaces: no, because per-frame
    /// swapchain image acquisition can't be recorded once).
    virtual bool is_surface(uint64_t id) const = 0;

    /// @}
    /// @name GPU Memory
    /// @{

    /**
     * @brief Low-level GPU buffer allocation. Producers should use
     *        `IGpuResourceManager::create_gpu_buffer` so the manager
     *        observes the buffer's lifetime and orchestrates its
     *        destruction.
     */
    virtual IGpuBuffer::Ptr create_gpu_buffer(const GpuBufferDesc& desc) = 0;

    /**
     * @brief Queues @p gb's underlying GPU memory for destruction
     *        once @p completion_marker has been signalled. Called by
     *        the resource manager from its observer callback when an
     *        IGpuBuffer's last Ptr drops. Must read out @p gb's
     *        backend handles synchronously (the wrapper object goes
     *        away once the observer chain unwinds).
     * @note  If @p completion_marker = kDefaultCompletionMarker, implementation should use
     *        IRenderBackend::frame_completion_marker() as the destruction frame.
     */
    virtual void defer_destroy_gpu_buffer(IGpuBuffer* gb,
                                          uint64_t completion_marker = kDefaultCompletionMarker) = 0;

    /**
     * @brief Records an in-place update of @p size bytes at @p offset
     *        on the backend's per-frame primary command stream. Backs
     *        `IGpuBuffer::update` so concrete buffer impls don't need
     *        to know about backend internals.
     */
    virtual void record_buffer_update(IGpuBuffer& target,
                                      size_t offset,
                                      size_t size,
                                      const void* data) = 0;

    /// @}
    /// @name Textures
    /// @{

    /** @brief Creates a texture and assigns a bindless index. Lifetime
     *         is managed via Ptr; the returned `IGpuTexture` defers its
     *         backend handles for destroy when the last Ptr drops. */
    virtual IGpuTexture::Ptr create_texture(const TextureDesc& desc) = 0;

    /**
     * @brief Queues an `IGpuTexture`'s native handles (image / view /
     *        allocation / framebuffer / render passes / bindless slot)
     *        for destruction once the GPU is past the current pending
     *        frame. Called by `~VkGpuTexture` / `~VkRenderTexture` when
     *        their owning Ptr drops.
     */
    virtual void defer_destroy_gpu_texture(IGpuTexture* texture,
                                           uint64_t completion_marker = kDefaultCompletionMarker) = 0;

    /** @brief Uploads pixel data to a texture via a staging buffer. */
    virtual void upload_texture(IGpuTexture& texture, const uint8_t* pixels, int width, int height) = 0;

    /**
     * @brief Reads back a texture's pixels from the GPU into host memory.
     *
     * Synchronous: allocates a host-readable staging buffer, submits a
     * texture-to-buffer copy (mip 0 only), waits for completion, copies
     * the bytes into @p out_pixels, and tears the staging buffer down.
     * Restores the texture's prior layout so subsequent rendering is
     * unaffected. Intended for debug dumps and golden-image tests; do
     * not call inside a hot frame.
     *
     * @return false if the staging allocation fails. On success,
     *         @p out_pixels contains `width * height *
     *         bytes_per_pixel(format)` bytes.
     */
    virtual bool read_texture(IGpuTexture& texture, vector<uint8_t>& out_pixels,
                              PixelFormat& out_format, uvec2& out_dims) = 0;

    /// @}
    /// @name Multi-attachment render targets (MRT)
    /// @{

    /**
     * @brief Creates a multi-attachment render target group.
     *
     * Allocates one sampleable `IGpuTexture` per entry in @p formats at
     * `width × height`, and a shared backend render pass + framebuffer
     * binding all of them in the declared order. Shaders that draw to
     * the group declare `layout(location = N) out vec4` for each
     * attachment.
     *
     * Lifetime is managed via Ptr; the returned `IRenderTextureGroup`
     * defers its backend handles for destroy when the last Ptr drops
     * (cascading through each attachment's deferred destroy too).
     */
    virtual IRenderTextureGroup::Ptr create_render_target_group(const TextureGroupDesc& desc) = 0;

    /**
     * @brief Queues an `IRenderTextureGroup`'s native handles for
     *        destruction once the GPU is past the current pending
     *        frame. Called by `~VkRenderTargetGroup` when its owning
     *        Ptr drops.
     */
    virtual void defer_destroy_gpu_render_target_group(
        IRenderTextureGroup* group,
        uint64_t completion_marker = kDefaultCompletionMarker) = 0;

    /// @}
    /// @name Pipelines
    /// @{

    /**
     * @brief Creates a graphics pipeline from SPIR-V shaders. Returns a handle.
     *
     * @param target_format Color attachment format the pipeline will
     *        render into. `Surface` follows the swapchain (default,
     *        used by direct-to-swapchain and surface-format RTT).
     *        Explicit formats (e.g. `RGBA16F` for HDR) get a per-format
     *        single-attachment render pass cached internally so the
     *        pipeline is render-pass compatible with the target.
     * @param target_group If non-null, the pipeline is compiled against
     *        the render pass of the given MRT group; in that case
     *        @p target_format is ignored (group attachments are fixed).
     */
    virtual IGpuPipeline::Ptr create_pipeline(const PipelineDesc& desc,
                                              PixelFormat target_format = PixelFormat::Surface,
                                              IRenderTextureGroup* target_group = nullptr) = 0;

    /** @brief Creates a compute pipeline from a compute shader. */
    virtual IGpuPipeline::Ptr create_compute_pipeline(const ComputePipelineDesc& desc) = 0;

    /**
     * @brief Queues an `IGpuPipeline`'s native handle for destruction
     *        once the GPU is past the current pending frame. Called
     *        by `~VkGpuPipeline` when its owning Ptr drops.
     */
    virtual void defer_destroy_gpu_pipeline(IGpuPipeline* pipeline) = 0;

    /// @}
    /// @name Frame submission
    /// @{

    /** @brief Begins a new frame: waits for GPU fence and starts command buffer recording. */
    virtual void begin_frame() = 0;

    /**
     * @brief Begins a render pass targeting the given surface or MRT group.
     *
     * For surface targets, acquires the swapchain image. Begins the
     * backend render pass and binds the bindless descriptor set. Use
     * the `IGpuTexture&` overload for renderable-texture targets.
     */
    virtual void begin_pass(uint64_t target_id) = 0;

    /**
     * @brief Begins a render pass targeting a renderable texture.
     *
     * The texture must have been created with TextureUsage::RenderTarget
     * or ColorAttachment. Begins the backend render pass and binds the
     * bindless descriptor set.
     */
    virtual void begin_pass(IGpuTexture& target) = 0;

    /**
     * @brief Begins a render pass targeting an MRT group.
     */
    virtual void begin_pass(IRenderTextureGroup& target) = 0;

    /**
     * @brief Allocates a `IGpuCommandBuffer` compatible with
     *        @p target_id. Producers record once when their pass
     *        content changes; the graph executor replays via
     *        `execute(cmd)` each frame.
     *
     * @param target_id Target the cmd buffer will play inside. For
     *                  Vulkan secondaries this resolves to the
     *                  render pass used during inheritance setup.
     *                  Pass `0` for compute / transfer cmd buffers
     *                  that play outside any render pass.
     */
    virtual IGpuCommandBuffer::Ptr create_command_buffer(uint64_t target_id) = 0;

    /**
     * @brief Allocates a `IGpuCommandBuffer` compatible with rendering
     *        into @p target. Mirror of the `uint64_t` overload for
     *        renderable-texture targets.
     */
    virtual IGpuCommandBuffer::Ptr create_command_buffer(IGpuTexture& target) = 0;

    /**
     * @brief Allocates an `IGpuCommandBuffer` compatible with rendering
     *        into the given MRT group.
     */
    virtual IGpuCommandBuffer::Ptr create_command_buffer(IRenderTextureGroup& target) = 0;

    /**
     * @brief Replays a recorded command buffer in the active frame.
     *        For raster cmd buffers, must be called between
     *        `begin_pass` / `end_pass` on a compatible target.
     */
    virtual void execute(const IGpuCommandBuffer::Ptr& cmd) = 0;

    /** @brief Ends the current render pass. */
    virtual void end_pass() = 0;

    /**
     * @brief Blits a source texture onto the swapchain image of @p surface_id.
     *
     * Acquires the swapchain image if not already acquired this frame,
     * records a scaling blit of @p source into the destination rect, and
     * transitions the swapchain image to a present-ready layout. The
     * source texture must have been created with TextureUsage::Storage.
     * Mutually exclusive with begin_pass on the same surface within a
     * frame.
     *
     * @param dst_rect Destination rect in surface pixels. Zero width/height
     *                 means "full surface".
     */
    virtual void blit_to_surface(IGpuTexture& source, uint64_t surface_id, rect dst_rect = {}) = 0;

    /**
     * @brief Blits @p source into @p dest. Both textures must have been
     *        created with renderable / storage / color-attachment usage.
     *        Source layout is restored after the blit; dest ends in
     *        SHADER_READ_ONLY so subsequent samples bind cleanly.
     */
    virtual void blit_to_texture(IGpuTexture& source, IGpuTexture& dest, rect dst_rect = {}) = 0;

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
