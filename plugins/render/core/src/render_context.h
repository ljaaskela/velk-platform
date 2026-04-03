#ifndef VELK_UI_RENDER_CONTEXT_IMPL_H
#define VELK_UI_RENDER_CONTEXT_IMPL_H

#include <velk/ext/object.h>

#include <velk-ui/interface/intf_render_context.h>
#include <velk-ui/plugins/render/intf_render_backend.h>
#include <velk-ui/plugins/render/plugin.h>

#include <unordered_map>

namespace velk_ui {

class RenderContextImpl : public velk::ext::ObjectCore<RenderContextImpl, IRenderContext>
{
public:
    VELK_CLASS_UID(ClassId::RenderContext, "RenderContext");

    bool init(const RenderConfig& config) override;
    ISurface::Ptr create_surface(int width, int height) override;
    IRenderer::Ptr create_renderer() override;
    velk::IObject::Ptr create_shader_material(const char* fragment_source,
                                              const char* vertex_source = nullptr) override;

    // Pipeline key -> PipelineId mapping (used by renderer)
    const std::unordered_map<uint64_t, PipelineId>& pipeline_map() const { return pipeline_map_; }

private:
    IRenderBackend::Ptr backend_;
    std::unordered_map<uint64_t, PipelineId> pipeline_map_;
    uint64_t next_pipeline_key_ = PipelineKey::CustomBase;
    bool initialized_ = false;
};

} // namespace velk_ui

#endif // VELK_UI_RENDER_CONTEXT_IMPL_H
