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

/// Maximum push constant size in bytes. Vulkan's spec-guaranteed
/// minimum is 128; we sit at that floor so any conformant driver works.
/// Larger payloads (e.g. RT per-dispatch state) live in IGpuBuffer-
/// backed root structs reached through an 8-byte BDA in push constants.
inline constexpr size_t kMaxRootConstantsSize = 128;

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
    SurfaceColorFormat color_format{SurfaceColorFormat::RGBA8_SRGB}; ///< Per-surface composite format.
};

/// One timed GPU region resolved from timestamp queries.
///
/// `label` points at a static string (the producing pass's `name()`),
/// so it stays valid without copying. `ms` is the GPU-side duration of
/// all regions sharing that label this frame, summed (so the many
/// bloom sub-dispatches collapse to a single "bloom" line).
struct GpuPassTiming
{
    const char* label = ""; ///< Pass name (static literal, never freed).
    float ms = 0.f;         ///< GPU duration in milliseconds.
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
     *         work submitted by the most recent `submit_frame()` call.
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

    /** @brief Marker the next `submit_frame()` submit will signal.
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

    /**
     * @brief Rebuilds a surface's platform handle and swapchain against a
     *        new native window, keeping @p surface_id stable.
     *
     * For platforms (Android) where the OS destroys and recreates the
     * native window across suspend/resume: the old platform surface handle
     * is invalid, so resize_surface (which keeps the handle) is not enough.
     * @p native_handle is the new opaque platform window (ANativeWindow* on
     * Android) passed through to the backend's surface-create callback.
     *
     * Must be called with no frame in flight (GPU work submitted against the
     * old swapchain must be drained first) — it waits idle and tears the old
     * swapchain + surface handle down before rebuilding.
     */
    virtual void recreate_surface(uint64_t surface_id, void* native_handle,
                                  int width, int height) = 0;

    /**
     * @brief Returns the per-surface composite intermediate as a
     *        renderable texture.
     *
     * Producers render to the returned target as if it were any
     * regular `IGpuTexture`. The backend tracks which surfaces were
     * rendered to and emits a final composite-to-swap blit at
     * `close_frame`. Multi-view-to-same-surface is handled internally
     * (first view loadOp respected; subsequent records overridden to
     * LOAD so draws stack).
     *
     * Stable Ptr: same wrapper across frames, recreated only on
     * surface resize. Cached cmd buffers that capture the wrapper's
     * IGpuTexture* / VkImage* / VkImageView remain valid.
     */
    virtual IRenderTarget::Ptr acquire_swapchain_texture(uint64_t surface_id) = 0;

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

    /// Fixed binding slots in the per-frame "global buffer" descriptor
    /// set (set = 1) that compute shaders read by index instead of by
    /// chasing a buffer_device_address. First step of the
    /// index-not-address resource model; GpuHive pages claim further
    /// slots later. Keep in sync with the set = 1 declarations in the
    /// compute shader preludes.
    enum GlobalBufferSlot : uint32_t {
        kGlobalBvhNodes  = 0,
        kGlobalBvhShapes = 1,
        kGlobalBufferSlotCount = 2,
    };

    /// Binds @p buffer at slot @p binding of the current frame's set = 1
    /// descriptor set, so compute shaders read it as a bound storage
    /// buffer. Call once per frame during prepare (after begin_frame),
    /// each frame the buffer is needed; pass null to leave the slot
    /// unbound. @p buffer must outlive the frame. The slot's set is only
    /// updated after its fence has fired, so no in-flight frame is
    /// disturbed.
    virtual void set_global_buffer(uint32_t binding, IGpuBuffer* buffer) = 0;

    /// The in-flight frame slot currently being recorded
    /// (0 .. frame_slot_count() - 1). begin_frame has already waited this
    /// slot's fence, so a per-slot resource region indexed by it has no
    /// in-flight reader and is safe to overwrite. IGpuArena uses this to
    /// pick its ring-buffer region.
    virtual uint32_t current_frame_slot() const = 0;
    virtual uint32_t frame_slot_count() const = 0;

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

    /**
     * @brief Allocates a standalone depth attachment as an `IGpuTexture`.
     *
     * Returned texture has `VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`
     * (no `SAMPLED_BIT` — depth is bound for record_begin_rendering, not
     * sampled through the bindless heap). Lifetime via Ptr; backend
     * defers destroy when the last Ptr drops. Used by paths that own
     * their color attachment separately and need a paired depth.
     */
    virtual IGpuTexture::Ptr create_depth_attachment_texture(
        int width, int height, DepthFormat format) = 0;

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
     * @brief Lifecycle callback from `~VkRenderTargetGroup` when its
     *        owning Ptr drops. The group owns no GPU resources of its own
     *        (color/depth attachments are `IGpuTexture::Ptr`s that defer
     *        their own destruction), so this only lets the backend drop
     *        its internal tracking of the group. The `completion_marker`
     *        is accepted for symmetry with the other defer-destroy
     *        callbacks but is unused.
     */
    virtual void defer_destroy_gpu_render_target_group(
        IRenderTextureGroup* group,
        uint64_t completion_marker = kDefaultCompletionMarker) = 0;

    /// @}
    /// @name Pipelines
    /// @{

    /**
     * @brief Creates a graphics pipeline against dynamic-rendering attachment
     *        formats.
     *
     * Pipelines compiled this way are render-pass-agnostic and run inside
     * a `vkCmdBeginRendering` scope opened by `IGpuCommandBuffer::record_begin_rendering`
     * with attachments matching the declared formats (count + ordering).
     *
     * @param color_formats Color attachment formats in declaration order (must
     *                      match the cmd-buffer's `record_begin_rendering` color list).
     * @param depth_format  Depth attachment format, or `DepthFormat::None` for
     *                      no depth.
     */
    virtual IGpuPipeline::Ptr create_pipeline_dynamic(
        const PipelineDesc& desc,
        array_view<const PixelFormat> color_formats,
        DepthFormat depth_format) = 0;

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
     * @brief Allocates a self-contained `IGpuCommandBuffer` for
     *        dynamic-rendering recording.
     *
     * Producers record once when their pass content changes; the graph
     * executor replays via `execute(cmd)` each frame. Raster passes
     * call `record_begin_rendering` / `record_end_rendering` inside
     * the cmd buffer to bind attachments at record time. Compute /
     * blit passes record outside any rendering scope.
     */
    virtual IGpuCommandBuffer::Ptr create_command_buffer() = 0;

    /**
     * @brief Replays a recorded command buffer in the active frame.
     *        Always called outside any rendering scope on the primary;
     *        the secondary internally manages its own scope.
     */
    virtual void execute(const IGpuCommandBuffer::Ptr& cmd) = 0;

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

    /** @brief Finalizes command recording for the current frame: records
     *         the composite-to-swap present blit for any surfaces used and
     *         ends the primary command buffer.
     *
     * Runs on the same thread as `prepare` / `begin_frame` (the recording
     * thread). After this returns the frame's command buffers are complete
     * and immutable — nothing records into them again — so the frame can be
     * handed to another thread for submission. Pair with `submit_frame`.
     */
    virtual void close_frame() = 0;

    /** @brief Submits the closed frame to the GPU queue and presents any
     *         surfaces used.
     *
     * Records nothing — it only submits already-recorded command buffers,
     * so it is safe to call on a render thread separate from the one that
     * ran `prepare` / `close_frame`. Pair with `close_frame`.
     */
    virtual void submit_frame() = 0;

    /// @}
    /// @name GPU timing
    ///
    /// Per-pass GPU timestamps for ground-truth profiling. Disabled by
    /// default: when off, `begin_gpu_timer` / `end_gpu_timer` are no-ops
    /// and no query work is recorded, so there is zero overhead. The perf
    /// overlay flips it on while it is shown. Results lag by the
    /// frames-in-flight depth (the backend reads a slot's timestamps only
    /// after that slot's fence has fired), so `last_gpu_timings()` returns
    /// the most recently resolved frame, not the one being recorded.
    /// @{

    /** @brief Enables or disables per-pass GPU timestamp capture. */
    virtual void set_gpu_timing_enabled(bool enabled) = 0;

    /** @brief Whether GPU timestamp capture is currently enabled. */
    virtual bool gpu_timing_enabled() const = 0;

    /** @brief Opens a timed GPU region labeled @p label on the current
     *         frame's command stream. No-op when timing is disabled or the
     *         per-frame region budget is exhausted. @p label must point at
     *         storage that outlives the frame (a static literal). */
    virtual void begin_gpu_timer(const char* label) = 0;

    /** @brief Closes the most recently opened timed GPU region. */
    virtual void end_gpu_timer() = 0;

    /** @brief Returns the last fully-resolved frame's per-pass GPU
     *         durations, aggregated by label. Empty until a timed frame
     *         has completed. The backing storage is stable until the next
     *         resolve (at `begin_frame`). */
    virtual array_view<const GpuPassTiming> last_gpu_timings() const = 0;

    /// @}
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_BACKEND_H
