#ifndef VELK_RENDER_CONTEXT_IMPL_H
#define VELK_RENDER_CONTEXT_IMPL_H

#include <velk/ext/object.h>

#include <velk-render/interface/intf_render_context.h>
#include <velk-render/plugin.h>

#include <unordered_map>

namespace velk {

class RenderContextImpl : public ext::ObjectCore<RenderContextImpl, IRenderContext>
{
public:
    VELK_CLASS_UID(ClassId::RenderContext, "RenderContext");

    bool init(const RenderConfig& config) override;
    ISurface::Ptr create_surface(int width, int height) override;
    IObject::Ptr create_shader_material(const char* fragment_source,
                                        const char* vertex_source = nullptr) override;

    const std::unordered_map<uint64_t, PipelineId>& pipeline_map() const override
    {
        return pipeline_map_;
    }

    IRenderBackend::Ptr backend() const override { return backend_; }

private:
    IRenderBackend::Ptr backend_;
    std::unordered_map<uint64_t, PipelineId> pipeline_map_;
    uint64_t next_pipeline_key_ = PipelineKey::CustomBase;
    bool initialized_ = false;
};

} // namespace velk

#endif // VELK_RENDER_CONTEXT_IMPL_H
