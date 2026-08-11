#include "pipeline/pipeline_manager.h"

#include <velk/api/velk.h>

namespace velk::impl {

bool PipelineManager::init(const IRenderBackend::Ptr& backend, IShaderManager& shaders)
{
    if (!backend) {
        VELK_LOG(E, "PipelineManager::init: no backend");
        return false;
    }
    backend_ = backend;
    shaders_ = &shaders;
    return true;
}

IGpuPipeline::Ptr PipelineManager::compile_dynamic(
    string_view fragment_source, string_view vertex_source,
    uint64_t key, array_view<const PixelFormat> color_formats,
    DepthFormat depth_format, const PipelineOptions& options,
    uint64_t* out_key)
{
    if (!backend_) {
        return {};
    }
    auto vert_src = vertex_source.empty() ? nullptr
                  : shaders_->compile(vertex_source, ShaderStage::Vertex);
    auto frag_src = fragment_source.empty() ? nullptr
                  : shaders_->compile(fragment_source, ShaderStage::Fragment);
    IShader::Ptr vert_shader = vert_src ? vert_src : shaders_->default_vertex_shader();
    IShader::Ptr frag_shader = frag_src ? frag_src : shaders_->default_fragment_shader();
    if (!vert_shader || !frag_shader) {
        VELK_LOG(E, "PipelineManager::compile_dynamic: missing vertex or fragment shader");
        return {};
    }

    PipelineDesc desc;
    desc.vertex = vert_shader;
    desc.fragment = frag_shader;
    desc.options = options;

    auto pid = backend_->create_pipeline_dynamic(desc, color_formats, depth_format);
    if (!pid) return {};

    if (key == 0) {
        key = next_key_++;
    }
    // Cache slot is `(user_key, color_formats[0], layout)`. The layout
    // signature (derived from the full color-format set) differentiates MRT
    // pipelines (e.g. gbuffer) from single-color forward ones sharing the
    // same user_key, without keying on a render-target instance. The pool
    // holds only a weak ref; the returned strong Ptr is the caller's to keep.
    PixelFormat cache_format = color_formats.empty()
        ? PixelFormat::RGBA8
        : color_formats[0];
    store(PipelineCacheKey{key, cache_format, depth_format,
                           pipeline_target_layout(color_formats)}, pid);
    if (out_key) *out_key = key;
    return pid;
}

IGpuPipeline::Ptr PipelineManager::create_compute(const IShader::Ptr& compute, uint64_t key)
{
    if (!backend_ || !compute) {
        return {};
    }

    ComputePipelineDesc desc;
    desc.compute = compute;

    auto pid = backend_->create_compute_pipeline(desc);
    if (!pid) {
        return {};
    }

    if (key == 0) {
        key = next_key_++;
    }
    // Compute pipelines are render-pass independent; key under a
    // canonical (RGBA8, layout 0) placeholder tuple so call sites look
    // them up with just the user_key. Pool holds a weak ref; the returned
    // strong Ptr is the caller's to keep.
    store(PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0}, pid);
    return pid;
}

IGpuPipeline::Ptr PipelineManager::compile_compute(string_view compute_source, uint64_t key)
{
    if (compute_source.empty()) {
        return {};
    }
    auto compute = shaders_->compile(compute_source, ShaderStage::Compute);
    if (!compute) {
        return {};
    }
    return create_compute(compute, key);
}

IGpuPipeline::Ptr PipelineManager::find(const PipelineCacheKey& key) const
{
    for (size_t i = 0; i < pool_.size();) {
        if (pool_[i].pipeline.expired()) {
            // Dead entry: swap-remove it and re-examine this slot.
            if (i + 1 < pool_.size()) {
                pool_[i] = std::move(pool_.back());
            }
            pool_.pop_back();
        } else if (pool_[i].key == key) {
            return pool_[i].pipeline.lock();
        } else {
            ++i;
        }
    }
    return nullptr;
}

void PipelineManager::store(const PipelineCacheKey& key, const IGpuPipeline::Ptr& pipeline)
{
    for (auto& e : pool_) {
        if (e.key == key) {
            e.pipeline = pipeline;
            return;
        }
    }
    pool_.push_back(PipelineCacheEntry{key, pipeline});
}

} // namespace velk::impl
