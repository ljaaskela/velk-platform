#ifndef VELK_RENDER_INTF_RENDER_CONTEXT_H
#define VELK_RENDER_INTF_RENDER_CONTEXT_H

#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>

#include <unordered_map>
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
 * `(user_key, target_format, target_group)`. The user-facing API still
 * exposes the user-key as a `uint64_t`; lookups reconstruct the full
 * key from the active path's render target description.
 *
 * - `user_key`: stable id chosen by the caller (visual / material /
 *   built-in `PipelineKey::*`) or auto-assigned when 0.
 * - `target_format`: the color attachment format the pipeline was
 *   compiled against. `PixelFormat::Surface` for swapchain / RTT
 *   targets that follow the surface; explicit format for HDR or
 *   other non-default render targets.
 * - `target_group`: non-zero for MRT (G-buffer) variants. Compute
 *   pipelines and forward raster pipelines use 0.
 */
struct PipelineCacheKey
{
    uint64_t user_key = 0;
    PixelFormat target_format = PixelFormat::Surface;
    RenderTargetGroup target_group = 0;

    bool operator==(const PipelineCacheKey& o) const noexcept
    {
        return user_key == o.user_key
            && target_format == o.target_format
            && target_group == o.target_group;
    }
};

struct PipelineCacheKeyHash
{
    size_t operator()(const PipelineCacheKey& k) const noexcept
    {
        size_t h = std::hash<uint64_t>{}(k.user_key);
        h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.target_format))
             + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<uint64_t>{}(static_cast<uint64_t>(k.target_group))
             + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

using PipelineCacheMap =
    std::unordered_map<PipelineCacheKey, PipelineId, PipelineCacheKeyHash>;

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
     * @brief Creates a pipeline from compiled shaders.
     *
     * If vertex or fragment is nullptr, the registered default is used.
     * If @p key is 0, a new key is auto-assigned. Otherwise the given key is used.
     * Returns the pipeline key, or 0 on failure.
     */
    virtual uint64_t create_pipeline(const IShader::Ptr& vertex, const IShader::Ptr& fragment,
                                     uint64_t key = 0,
                                     PixelFormat target_format = PixelFormat::Surface,
                                     RenderTargetGroup target_group = 0,
                                     const PipelineOptions& options = {}) = 0;

    /**
     * @brief Convenience: compiles GLSL shaders and creates the pipeline in one call.
     *
     * Empty sources are substituted with the registered defaults.
     * @p target_format is the color attachment format the pipeline will
     * render into (`Surface` to follow the swapchain). When
     * @p target_group is non-zero the pipeline is compiled against that
     * MRT group's render pass; otherwise against the single-attachment
     * render pass for @p target_format.
     */
    virtual uint64_t compile_pipeline(string_view fragment_source, string_view vertex_source,
                                      uint64_t key = 0,
                                      PixelFormat target_format = PixelFormat::Surface,
                                      RenderTargetGroup target_group = 0,
                                      const PipelineOptions& options = {}) = 0;

    /**
     * @brief Creates a compute pipeline from a compiled compute shader.
     *
     * If @p key is 0, a new key is auto-assigned. Otherwise the given key is
     * used. Returns the pipeline key, or 0 on failure. Compute pipelines
     * share the pipeline_map() with graphics pipelines and the same backend
     * destroy_pipeline() path.
     */
    virtual uint64_t create_compute_pipeline(const IShader::Ptr& compute, uint64_t key = 0) = 0;

    /**
     * @brief Convenience: compiles a compute GLSL shader and creates the pipeline.
     */
    virtual uint64_t compile_compute_pipeline(string_view compute_source, uint64_t key = 0) = 0;

    /** @brief Registers a default vertex shader used when create_pipeline receives nullptr. */
    virtual void set_default_vertex_shader(const IShader::Ptr& shader) = 0;

    /** @brief Registers a default fragment shader used when create_pipeline receives nullptr. */
    virtual void set_default_fragment_shader(const IShader::Ptr& shader) = 0;

    /** @brief Returns the unified pipeline cache keyed by `PipelineCacheKey`. */
    virtual const PipelineCacheMap& pipeline_map() const = 0;

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
