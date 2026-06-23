#ifndef VELK_RENDER_CONTEXT_IMPL_H
#define VELK_RENDER_CONTEXT_IMPL_H

#include "shader/shader_cache.h"
#include "shader/shader_compiler.h"

#include <velk/ext/object.h>

#include <velk-render/interface/intf_render_context.h>
#include <velk-render/plugin.h>

#include <velk/vector.h>

namespace velk {

/// One entry in the weak-ref pipeline intern pool.
struct PipelineCacheEntry
{
    PipelineCacheKey key;
    IGpuPipeline::WeakPtr pipeline;
};

/// Weak-ref intern pool: compiled pipelines are looked up here but owned
/// (strong) by the recorders that bind them (each cached IRenderPass holds
/// `IGpuPipeline::Ptr`s for the pipelines its command buffer uses). A
/// pipeline dies when the last pass referencing it is gone; `find_pipeline`
/// returns nullptr for an entry whose pipeline has expired and prunes it.
///
/// Backed by a flat vector, not a hash map: the set is small (bounded by
/// distinct pipeline content) and lookups are cold (gated by pass rebuilds),
/// so a linear scan wins on simplicity + cache locality, and lets
/// `find_pipeline` swap-remove expired entries as it scans — keeping the pool
/// bounded with no separate sweep.

class RenderContextImpl : public ext::ObjectCore<RenderContextImpl, IRenderContext>
{
public:
    VELK_CLASS_UID(ClassId::RenderContext, "RenderContext");

    bool init(const RenderConfig& config) override;
    IWindowSurface::Ptr create_surface(const SurfaceConfig& config) override;
    IMaterial::Ptr create_shader_material(string_view fragment_source, string_view vertex_source) override;

    IShader::Ptr compile_shader(string_view source, ShaderStage stage,
                                uint64_t key = 0) override;
    IGpuPipeline::Ptr compile_pipeline_dynamic(string_view fragment_source,
                                      string_view vertex_source,
                                      uint64_t key,
                                      array_view<const PixelFormat> color_formats,
                                      DepthFormat depth_format,
                                      const PipelineOptions& options = {},
                                      uint64_t* out_key = nullptr) override;
    IGpuPipeline::Ptr create_compute_pipeline(const IShader::Ptr& compute, uint64_t key = 0) override;
    IGpuPipeline::Ptr compile_compute_pipeline(string_view compute_source, uint64_t key = 0) override;

    void set_default_vertex_shader(const IShader::Ptr& shader) override;
    void set_default_fragment_shader(const IShader::Ptr& shader) override;

    void register_shader_include(string_view name, string_view content) override;

    IGpuPipeline::Ptr find_pipeline(const PipelineCacheKey& key) const override
    {
        for (size_t i = 0; i < pipeline_cache_.size();) {
            if (pipeline_cache_[i].pipeline.expired()) {
                // Dead entry: swap-remove it and re-examine this slot.
                if (i + 1 < pipeline_cache_.size()) {
                    pipeline_cache_[i] = std::move(pipeline_cache_.back());
                }
                pipeline_cache_.pop_back();
            } else if (pipeline_cache_[i].key == key) {
                return pipeline_cache_[i].pipeline.lock();
            } else {
                ++i;
            }
        }
        return nullptr;
    }

    IRenderBackend::Ptr backend() const override { return backend_; }

    IMeshBuilder& get_mesh_builder() override;

    IBuffer::Ptr get_default_buffer(DefaultBufferType type) const override;

private:
    /// Interns a compiled pipeline weakly under @p key, replacing any existing
    /// entry for that key. Called by the compile paths after a find miss.
    void store_pipeline(const PipelineCacheKey& key, const IGpuPipeline::Ptr& pipeline)
    {
        for (auto& e : pipeline_cache_) {
            if (e.key == key) {
                e.pipeline = pipeline;
                return;
            }
        }
        pipeline_cache_.push_back(PipelineCacheEntry{key, pipeline});
    }

    IRenderBackend::Ptr backend_;
    IMeshBuilder::Ptr mesh_builder_;
    IMeshBuffer::Ptr default_uv1_;
    /// Mutable: find_pipeline prunes expired entries during its scan.
    mutable vector<PipelineCacheEntry> pipeline_cache_;
    ShaderIncludeMap shader_includes_;
    mutable ShaderCache shader_cache_;
    IShader::Ptr default_vertex_shader_;
    IShader::Ptr default_fragment_shader_;
    uint64_t next_pipeline_key_ = PipelineKey::CustomBase;
    bool initialized_ = false;
};

} // namespace velk

#endif // VELK_RENDER_CONTEXT_IMPL_H
