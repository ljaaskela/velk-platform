#ifndef VELK_RENDER_CONTEXT_IMPL_H
#define VELK_RENDER_CONTEXT_IMPL_H

#include "shader/shader_cache.h"
#include "shader/shader_compiler.h"

#include <velk/ext/object.h>

#include <velk-render/interface/intf_render_context.h>
#include <velk-render/plugin.h>

#include <unordered_map>

namespace velk {

struct PipelineCacheKeyHash
{
    size_t operator()(const PipelineCacheKey& k) const noexcept
    {
        size_t h = std::hash<uint64_t>{}(k.user_key);
        h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.target_format))
             + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<uint64_t>{}(k.target_layout)
             + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

using PipelineCacheMap =
    std::unordered_map<PipelineCacheKey, IGpuPipeline::Ptr, PipelineCacheKeyHash>;

class RenderContextImpl : public ext::ObjectCore<RenderContextImpl, IRenderContext>
{
public:
    VELK_CLASS_UID(ClassId::RenderContext, "RenderContext");

    bool init(const RenderConfig& config) override;
    IWindowSurface::Ptr create_surface(const SurfaceConfig& config) override;
    IMaterial::Ptr create_shader_material(string_view fragment_source, string_view vertex_source) override;

    IShader::Ptr compile_shader(string_view source, ShaderStage stage,
                                uint64_t key = 0) override;
    uint64_t compile_pipeline_dynamic(string_view fragment_source,
                                      string_view vertex_source,
                                      uint64_t key,
                                      array_view<const PixelFormat> color_formats,
                                      DepthFormat depth_format,
                                      const PipelineOptions& options = {}) override;
    uint64_t create_compute_pipeline(const IShader::Ptr& compute, uint64_t key = 0) override;
    uint64_t compile_compute_pipeline(string_view compute_source, uint64_t key = 0) override;

    void set_default_vertex_shader(const IShader::Ptr& shader) override;
    void set_default_fragment_shader(const IShader::Ptr& shader) override;

    void register_shader_include(string_view name, string_view content) override;

    IGpuPipeline::Ptr find_pipeline(const PipelineCacheKey& key) const override
    {
        auto it = pipeline_map_.find(key);
        return it != pipeline_map_.end() ? it->second : nullptr;
    }

    IRenderBackend::Ptr backend() const override { return backend_; }

    IMeshBuilder& get_mesh_builder() override;

    IBuffer::Ptr get_default_buffer(DefaultBufferType type) const override;

private:
    IRenderBackend::Ptr backend_;
    IMeshBuilder::Ptr mesh_builder_;
    IMeshBuffer::Ptr default_uv1_;
    PipelineCacheMap pipeline_map_;
    ShaderIncludeMap shader_includes_;
    mutable ShaderCache shader_cache_;
    IShader::Ptr default_vertex_shader_;
    IShader::Ptr default_fragment_shader_;
    uint64_t next_pipeline_key_ = PipelineKey::CustomBase;
    bool initialized_ = false;
};

} // namespace velk

#endif // VELK_RENDER_CONTEXT_IMPL_H
