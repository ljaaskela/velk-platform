#ifndef VELK_RENDER_CONTEXT_IMPL_H
#define VELK_RENDER_CONTEXT_IMPL_H

#include "shader_cache.h"
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
    IWindowSurface::Ptr create_surface(const SurfaceConfig& config) override;
    IMaterial::Ptr create_shader_material(string_view fragment_source, string_view vertex_source) override;

    IShader::Ptr compile_shader(string_view source, ShaderStage stage,
                                uint64_t key = 0) override;
    uint64_t create_pipeline(const IShader::Ptr& vertex, const IShader::Ptr& fragment,
                             uint64_t key = 0) override;
    uint64_t compile_pipeline(string_view fragment_source, string_view vertex_source,
                              uint64_t key = 0) override;
    uint64_t create_compute_pipeline(const IShader::Ptr& compute, uint64_t key = 0) override;
    uint64_t compile_compute_pipeline(string_view compute_source, uint64_t key = 0) override;

    void set_default_vertex_shader(const IShader::Ptr& shader) override;
    void set_default_fragment_shader(const IShader::Ptr& shader) override;

    void register_shader_include(string_view name, string_view content) override;

    const std::unordered_map<uint64_t, PipelineId>& pipeline_map() const override { return pipeline_map_; }

    IRenderBackend::Ptr backend() const override { return backend_; }

private:
    IRenderBackend::Ptr backend_;
    std::unordered_map<uint64_t, PipelineId> pipeline_map_;
    ShaderIncludeMap shader_includes_;
    mutable ShaderCache shader_cache_;
    IShader::Ptr default_vertex_shader_;
    IShader::Ptr default_fragment_shader_;
    uint64_t next_pipeline_key_ = PipelineKey::CustomBase;
    bool initialized_ = false;
};

} // namespace velk

#endif // VELK_RENDER_CONTEXT_IMPL_H
