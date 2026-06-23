#ifndef VELK_RENDER_INTF_RENDER_CONTEXT_H
#define VELK_RENDER_INTF_RENDER_CONTEXT_H

#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_batch.h>
#include <velk-render/frame/draw_call_emit.h>
#include <velk-render/interface/intf_frame_data_manager.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/frustum.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_shader.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/render_types.h>

namespace velk {

/**
 * @brief Cache key for compiled pipelines.
 *
 * Pipelines are uniquely identified by the tuple
 * `(user_key, target_format, depth_format, target_layout)`. The user-facing
 * API still exposes the user-key as a `uint64_t`; lookups reconstruct the
 * full key from the active path's render target description.
 *
 * - `user_key`: stable id chosen by the caller (visual / material /
 *   built-in `PipelineKey::*`) or auto-assigned when 0.
 * - `target_format`: the color attachment format the pipeline was
 *   compiled against (RGBA8, RGBA8_SRGB, RGBA16F, ...). For pipelines
 *   that don't render into a color attachment (compute, blit), use
 *   the canonical `RGBA8` placeholder so the cache key is deterministic.
 * - `depth_format`: the depth attachment format the pipeline was compiled
 *   against (`None` for no-depth targets and for compute/blit pipelines).
 *   Keyed because dynamic-rendering bakes the depth format into the
 *   pipeline, so a depth and a no-depth variant of the same shader must
 *   not share a cache slot.
 * - `target_layout`: signature of the full color-attachment layout,
 *   non-zero for MRT (G-buffer) variants and 0 for single-attachment
 *   (forward / compute). Derived from the attachment formats via
 *   `pipeline_target_layout`, so it is stable across resize / recreation
 *   of the render target. Disambiguates an MRT pipeline whose first
 *   format matches a single-attachment one (both RGBA8, say).
 */
struct PipelineCacheKey
{
    uint64_t user_key = 0;
    PixelFormat target_format = PixelFormat::RGBA8;
    DepthFormat depth_format = DepthFormat::None;
    uint64_t target_layout = 0;

    bool operator==(const PipelineCacheKey& o) const noexcept
    {
        return user_key == o.user_key
            && target_format == o.target_format
            && depth_format == o.depth_format
            && target_layout == o.target_layout;
    }
};

/**
 * @brief Stable signature of a render target's color-attachment layout.
 *
 * Single-attachment targets (forward, compute, blit) yield 0; MRT targets
 * yield a hash of their color formats. Derived from the formats only —
 * never from a render-target instance — so it is identical across resize
 * or recreation of the target, keeping pipeline-cache lookups stable.
 */
inline uint64_t pipeline_target_layout(array_view<const PixelFormat> color_formats) noexcept
{
    if (color_formats.size() <= 1) return 0;
    uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
    for (auto f : color_formats) {
        h ^= static_cast<uint64_t>(f);
        h *= 1099511628211ull; // FNV-1a prime
    }
    // Force the top bit so an MRT layout can never equal the
    // single-attachment sentinel (0) and collide with a forward key.
    return h | 0x8000000000000000ull;
}

/**
 * @brief Owns the render backend and provides rendering infrastructure.
 *
 * The context is created via create_render_context(). It loads the backend
 * plugin, initializes the GPU, and provides factory methods for surfaces,
 * pipelines, and shader materials.
 */
class IRenderContext : public Interface<IRenderContext>
{
public:
    /** @brief Initializes the backend. Must be called before any other method. */
    virtual bool init(const RenderConfig& config) = 0;

    /** @brief Creates a render target surface with the given configuration. */
    virtual IWindowSurface::Ptr create_surface(const SurfaceConfig& config) = 0;

    /**
     * @brief Creates a shader material from GLSL source.
     *
     * Compiles the shaders, registers the pipeline, reflects material parameters,
     * and returns a ShaderMaterial with the pipeline handle and inputs set.
     * Returns nullptr on compilation failure.
     */
    virtual IMaterial::Ptr create_shader_material(string_view fragment_source,
                                                  string_view vertex_source = {}) = 0;

    /**
     * @brief Compiles GLSL source to a reusable shader handle.
     *
     * @param source GLSL source code.
     * @param stage Shader stage (Vertex or Fragment).
     * @param key Optional cache key. When non-zero, the SPIR-V is read from /
     *            written to the shader cache under this key. When zero, a hash
     *            of @p source is computed at runtime. Built-in shaders should
     *            pass a constexpr `make_hash64(source)` to avoid the runtime
     *            hash. User shaders pass 0.
     */
    virtual IShader::Ptr compile_shader(string_view source, ShaderStage stage,
                                        uint64_t key = 0) = 0;

    /**
     * @brief Compiles a graphics pipeline against dynamic-rendering attachment
     *        formats (no VkRenderPass), runnable inside a
     *        `record_begin_rendering` scope.
     *
     * Cache slot is
     * `(user_key, color_formats[0], depth_format, pipeline_target_layout(color_formats))`
     * — the layout signature is derived from @p color_formats, so MRT
     * pipelines are distinguished from single-attachment ones without a
     * render-target instance leaking into the key, and @p depth_format keeps
     * depth and no-depth variants of the same shader in distinct slots.
     *
     * Returns the compiled pipeline as a strong `Ptr` (also stored in the
     * cache as a weak ref). The caller MUST keep the returned Ptr alive
     * for as long as it's used — the cache won't, since it holds only a
     * weak ref. Returns nullptr on failure. If @p out_key is non-null it
     * receives the final cache key (the passed key, or an auto-assigned
     * one when 0 was passed).
     */
    virtual IGpuPipeline::Ptr compile_pipeline_dynamic(string_view fragment_source,
                                              string_view vertex_source,
                                              uint64_t key,
                                              array_view<const PixelFormat> color_formats,
                                              DepthFormat depth_format,
                                              const PipelineOptions& options = {},
                                              uint64_t* out_key = nullptr) = 0;

    /**
     * @brief Creates a compute pipeline from a compiled compute shader.
     *
     * If @p key is 0, a new key is auto-assigned. Returns the compiled
     * pipeline as a strong `Ptr` (stored weak in the cache); the caller
     * must keep it alive. Returns nullptr on failure. Compute pipelines
     * share the unified pipeline cache with graphics pipelines and the same
     * backend destroy_pipeline() path.
     */
    virtual IGpuPipeline::Ptr create_compute_pipeline(const IShader::Ptr& compute, uint64_t key = 0) = 0;

    /**
     * @brief Convenience: compiles a compute GLSL shader and creates the
     *        pipeline. Returns the strong `Ptr` (stored weak in the cache);
     *        the caller must keep it alive. nullptr on failure.
     */
    virtual IGpuPipeline::Ptr compile_compute_pipeline(string_view compute_source, uint64_t key = 0) = 0;

    /** @brief Registers a default vertex shader used when create_pipeline receives nullptr. */
    virtual void set_default_vertex_shader(const IShader::Ptr& shader) = 0;

    /** @brief Registers a default fragment shader used when create_pipeline receives nullptr. */
    virtual void set_default_fragment_shader(const IShader::Ptr& shader) = 0;

    /**
     * @brief Looks up a compiled pipeline in the unified cache by key.
     *
     * Returns nullptr if no pipeline has been compiled for @p key yet.
     */
    virtual IGpuPipeline::Ptr find_pipeline(const PipelineCacheKey& key) const = 0;

    /**
     * @brief Registers a virtual shader include.
     *
     * Shaders can then use `#include "name"` to pull in the content.
     * Modules register their own includes (e.g. velk-ui registers "velk-ui.glsl").
     */
    virtual void register_shader_include(string_view name, string_view content) = 0;

    /** @brief Returns the render backend. */
    virtual IRenderBackend::Ptr backend() const = 0;

    /**
     * @brief Returns the context-owned mesh builder.
     *
     * Lifetime is tied to the render context (constructed in init).
     * Use it to create IMesh instances (`build(...)`) or fetch shared
     * engine meshes (`get_unit_quad()`).
     */
    virtual IMeshBuilder& get_mesh_builder() = 0;

    /**
     * @brief Returns a context-owned default buffer for an optional
     *        vertex-stream slot.
     *
     * The context owns a single shared fallback per `DefaultBufferType`,
     * uploaded once at init. Draws whose primitive does not supply
     * that stream point their DrawData slot at the fallback and the
     * shader reads it as a safe zero (see `DefaultBufferType` docs for
     * per-slot semantics). Returns nullptr for an unknown type.
     */
    virtual IBuffer::Ptr get_default_buffer(DefaultBufferType type) const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_CONTEXT_H
