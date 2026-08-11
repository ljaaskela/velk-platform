#ifndef VELK_RENDER_CONTEXT_IMPL_H
#define VELK_RENDER_CONTEXT_IMPL_H

#include <velk/ext/object.h>

#include <velk-render/interface/intf_render_context.h>
#include <velk-render/plugin.h>

namespace velk {

class RenderContextImpl : public ext::ObjectCore<RenderContextImpl, IRenderContext>
{
public:
    VELK_CLASS_UID(ClassId::RenderContext, "RenderContext");

    bool init(const RenderConfig& config) override;
    IWindowSurface::Ptr create_surface(const SurfaceConfig& config) override;
    IMaterial::Ptr create_shader_material(string_view fragment_source, string_view vertex_source) override;

    IShaderManager& shaders() override { return *shader_manager_; }
    IPipelineManager& pipelines() override { return *pipeline_manager_; }

    IRenderBackend::Ptr backend() const override { return backend_; }
    IGpuResourceManager* resources() const override { return resources_; }
    void set_resource_manager(IGpuResourceManager* resources) override { resources_ = resources; }

    IMeshBuilder& get_mesh_builder() override;

    IBuffer::Ptr get_default_buffer(DefaultBufferType type) const override;

private:
    IRenderBackend::Ptr backend_;
    IMeshBuilder::Ptr mesh_builder_;
    IMeshBuffer::Ptr default_uv1_;
    IShaderManager::Ptr shader_manager_;
    IPipelineManager::Ptr pipeline_manager_;
    /// Installed by the renderer, which owns it. Raw: the renderer outlives
    /// the context's use of it.
    IGpuResourceManager* resources_ = nullptr;
    bool initialized_ = false;
};

} // namespace velk

#endif // VELK_RENDER_CONTEXT_IMPL_H
