#ifndef VELK_RENDER_CONTEXT_IMPL_H
#define VELK_RENDER_CONTEXT_IMPL_H

#include "shader_compiler.h"

#include <velk/ext/object.h>

#include <unordered_map>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/plugin.h>

namespace velk {

class RenderContextImpl : public ext::ObjectCore<RenderContextImpl, IRenderContext>
{
public:
    VELK_CLASS_UID(ClassId::RenderContext, "RenderContext");

    bool init(const RenderConfig& config) override;
    ISurface::Ptr create_surface(int width, int height) override;
    IMaterial::Ptr create_shader_material(string_view fragment_source, string_view vertex_source) override;
    uint64_t compile_pipeline(string_view fragment_source, string_view vertex_source,
                              uint64_t key = 0) override;
    void register_shader_include(string_view name, string_view content) override;

    const std::unordered_map<uint64_t, PipelineId>& pipeline_map() const override { return pipeline_map_; }

    IRenderBackend::Ptr backend() const override { return backend_; }

private:
    IRenderBackend::Ptr backend_;
    std::unordered_map<uint64_t, PipelineId> pipeline_map_;
    ShaderIncludeMap shader_includes_;
    uint64_t next_pipeline_key_ = PipelineKey::CustomBase;
    bool initialized_ = false;
};

} // namespace velk

#endif // VELK_RENDER_CONTEXT_IMPL_H
