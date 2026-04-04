#ifndef VELK_RENDER_INTF_RENDER_CONTEXT_H
#define VELK_RENDER_INTF_RENDER_CONTEXT_H

#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/render_types.h>

#include <unordered_map>

namespace velk {

/**
 * @brief Owns the render backend and provides rendering infrastructure.
 *
 * The context is created via create_render_context(). It loads the backend
 * plugin, initializes the GPU, and provides factory methods for surfaces
 * and shader materials.
 */
class IRenderContext : public Interface<IRenderContext>
{
public:
    virtual bool init(const RenderConfig& config) = 0;
    virtual ISurface::Ptr create_surface(int width, int height) = 0;

    /**
     * @brief Creates a shader material from GLSL source.
     *
     * Compiles the shader, registers the pipeline with the backend,
     * and returns a material with the pipeline handle set.
     * If vertex_source is nullptr, a default instanced quad vertex shader is used.
     * Returns nullptr on compilation failure.
     */
    virtual IObject::Ptr create_shader_material(const char* fragment_source,
                                                const char* vertex_source = nullptr) = 0;

    /// Returns the mapping from pipeline keys to backend PipelineId handles.
    virtual const std::unordered_map<uint64_t, PipelineId>& pipeline_map() const = 0;

    /// Returns the render backend.
    virtual IRenderBackend::Ptr backend() const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_CONTEXT_H
