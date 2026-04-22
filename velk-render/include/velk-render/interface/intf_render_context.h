#ifndef VELK_RENDER_INTF_RENDER_CONTEXT_H
#define VELK_RENDER_INTF_RENDER_CONTEXT_H

#include <velk/interface/intf_metadata.h>

#include <unordered_map>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_shader.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/render_types.h>

namespace velk {

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
                                     RenderTargetGroup target_group = 0,
                                     const PipelineOptions& options = {}) = 0;

    /**
     * @brief Convenience: compiles GLSL shaders and creates the pipeline in one call.
     *
     * Empty sources are substituted with the registered defaults. If
     * @p target_group is non-zero, the pipeline is compiled against that
     * MRT group's render pass (for G-buffer fills); otherwise against
     * the single-attachment swapchain render pass.
     */
    virtual uint64_t compile_pipeline(string_view fragment_source, string_view vertex_source,
                                      uint64_t key = 0,
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

    /**
     * @brief Registers the default deferred G-buffer vertex shader.
     *
     * Used by the deferred pipeline when a material's
     * `IProgram::get_gbuffer_vertex_src()` is empty.
     */
    virtual void set_default_gbuffer_vertex_shader(const IShader::Ptr& shader) = 0;

    /**
     * @brief Registers the default deferred G-buffer fragment shader.
     *
     * Used by the deferred pipeline when a material's
     * `IProgram::get_gbuffer_fragment_src()` is empty.
     */
    virtual void set_default_gbuffer_fragment_shader(const IShader::Ptr& shader) = 0;

    /** @brief Returns the default G-buffer vertex shader (may be null). */
    virtual IShader::Ptr get_default_gbuffer_vertex_shader() const = 0;

    /** @brief Returns the default G-buffer fragment shader (may be null). */
    virtual IShader::Ptr get_default_gbuffer_fragment_shader() const = 0;

    /** @brief Returns the mapping from pipeline keys to backend PipelineId handles. */
    virtual const std::unordered_map<uint64_t, PipelineId>& pipeline_map() const = 0;

    /**
     * @brief Returns the parallel mapping from pipeline keys to G-buffer
     *        PipelineId handles. Populated by compile_gbuffer_pipeline().
     *
     * G-buffer variants are compiled on-demand once per (forward key,
     * target group) combination — render passes of distinct groups are
     * compatible (same formats) so a single pipeline works across views.
     */
    virtual const std::unordered_map<uint64_t, PipelineId>& gbuffer_pipeline_map() const = 0;

    /**
     * @brief Compiles a G-buffer pipeline variant for the given pipeline
     *        key, targeting the supplied MRT group's render pass.
     *
     * Empty sources fall back to the registered default G-buffer shaders.
     * Registers the result under @p key in gbuffer_pipeline_map().
     * Idempotent: returns the existing PipelineId on a repeat call.
     */
    virtual PipelineId compile_gbuffer_pipeline(string_view fragment_source,
                                                string_view vertex_source,
                                                uint64_t key,
                                                RenderTargetGroup target_group,
                                                const PipelineOptions& options = {}) = 0;

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
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_CONTEXT_H
