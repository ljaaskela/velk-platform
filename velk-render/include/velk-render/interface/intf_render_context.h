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
#include <velk-render/interface/intf_pipeline_manager.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_shader.h>
#include <velk-render/interface/intf_shader_manager.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/render_types.h>

namespace velk {

class IGpuResourceManager;

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
     * @brief Shader compilation and caching.
     *
     * Owns the active shader compiler, the registered dependencies, and the
     * on-disk SPIR-V cache.
     */
    virtual IShaderManager& shaders() = 0;

    /**
     * @brief Pipeline compilation and interning.
     *
     * Compiles graphics and compute pipelines through the shader manager and
     * interns them weakly.
     */
    virtual IPipelineManager& pipelines() = 0;

    /** @brief Returns the render backend. */
    virtual IRenderBackend::Ptr backend() const = 0;

    /// The renderer's GPU resource manager, or null before the renderer has
    /// installed it. Plugins reach it to claim a shared set = 1 arena
    /// (`shared_arena`) for data of their own; velk-render itself does not
    /// own it, which is why this is an accessor rather than a member.
    virtual IGpuResourceManager* resources() const = 0;

    /// Installs the resource manager. Called once by the renderer during
    /// init; not for general use.
    virtual void set_resource_manager(IGpuResourceManager* resources) = 0;

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
