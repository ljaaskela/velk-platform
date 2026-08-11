#ifndef VELK_RENDER_PIPELINE_MANAGER_H
#define VELK_RENDER_PIPELINE_MANAGER_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_pipeline_manager.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/// One entry in the weak-ref pipeline intern pool.
struct PipelineCacheEntry
{
    PipelineCacheKey key;
    IGpuPipeline::WeakPtr pipeline;
};

/**
 * Weak-ref intern pool: compiled pipelines are looked up here but owned
 * (strong) by the recorders that bind them (each cached IRenderPass holds
 * `IGpuPipeline::Ptr`s for the pipelines its command buffer uses). A
 * pipeline dies when the last pass referencing it is gone; `find` returns
 * nullptr for an entry whose pipeline has expired and prunes it.
 *
 * Backed by a flat vector, not a hash map: the set is small (bounded by
 * distinct pipeline content) and lookups are cold (gated by pass rebuilds),
 * so a linear scan wins on simplicity + cache locality, and lets `find`
 * swap-remove expired entries as it scans, keeping the pool bounded with no
 * separate sweep.
 */
class PipelineManager : public ext::ObjectCore<PipelineManager, IPipelineManager>
{
public:
    VELK_CLASS_UID(ClassId::PipelineManager, "PipelineManager");

    bool init(const IRenderBackend::Ptr& backend, IShaderManager& shaders) override;

    IGpuPipeline::Ptr compile_dynamic(string_view fragment_source,
                                      string_view vertex_source,
                                      uint64_t key,
                                      array_view<const PixelFormat> color_formats,
                                      DepthFormat depth_format,
                                      const PipelineOptions& options = {},
                                      uint64_t* out_key = nullptr) override;

    IGpuPipeline::Ptr create_compute(const IShader::Ptr& compute, uint64_t key = 0) override;
    IGpuPipeline::Ptr compile_compute(string_view compute_source, uint64_t key = 0) override;

    IGpuPipeline::Ptr find(const PipelineCacheKey& key) const override;

private:
    /// Interns @p pipeline weakly under @p key, replacing any existing entry
    /// for that key. Called by the compile paths after a find miss.
    void store(const PipelineCacheKey& key, const IGpuPipeline::Ptr& pipeline);

    IRenderBackend::Ptr backend_;
    /// Owned by the render context, which outlives this.
    IShaderManager* shaders_ = nullptr;
    /// Mutable: find prunes expired entries during its scan.
    mutable vector<PipelineCacheEntry> pool_;
    uint64_t next_key_ = PipelineKey::CustomBase;
};

} // namespace velk::impl

#endif // VELK_RENDER_PIPELINE_MANAGER_H
