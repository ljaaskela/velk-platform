#include "frame/render_graph.h"

#include <velk/api/perf.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_interface.h>

#include <velk-render/detail/intf_gpu_resource_manager_internal.h>
#include <velk-render/plugin.h>

namespace velk::impl {

#ifdef VELK_RENDER_DEBUG
#define RENDER_LOG(...) VELK_LOG(I, __VA_ARGS__)
#else
#define RENDER_LOG(...) ((void)0)
#endif

void RenderGraph::init(::velk::IRenderBackend* backend)
{
    if (!resources_) {
        resources_ = ::velk::instance().create<::velk::IGpuResourceManager>(
            ::velk::ClassId::GpuResourceManager);
    }
    if (auto* internal = interface_cast<
            ::velk::IGpuResourceManagerInternal>(resources_.get())) {
        internal->init(backend);
        // Per-graph transient pool: routes destroyed shells onto the
        // pool free-list, with LRU eviction folding back into the
        // deferred-destroy queue. The renderer's persistent manager
        // never enables this mode.
        internal->enable_transient_pool();
    }
}

::velk::IGpuResourceManager& RenderGraph::resources()
{
    // Accessing transient resources before init() is a programming
    // error. Renderer wires init() right after creating the graph.
    return *resources_;
}

void RenderGraph::clear()
{
    // Preserve `prev_passes_` and `barriers_` across clears: they
    // capture the last *successfully compiled* frame and let the next
    // compile short-circuit when the pass list matches. The renderer
    // calls clear() at multiple points (slot claim, build-retry top,
    // post-present cleanup) and a mid-frame retry's failed attempt
    // must not clobber the prior frame's snapshot. `compile()`
    // rebuilds `prev_passes_` itself at the end of its rebuild path.
    passes_.clear();
    states_.clear();
    imported_.clear();
}

void RenderGraph::import(const ::velk::IGpuResource::Ptr& resource)
{
    if (!resource) return;
    auto* raw = resource.get();
    imported_[raw] = true;
    states_.emplace(raw, ResourceState::Undefined);
}

void RenderGraph::add_pass(::velk::IRenderPass::Ptr pass)
{
    if (!pass) return;
    for (auto& r : pass->reads()) {
        if (r) states_.emplace(r.get(), ResourceState::Undefined);
    }
    for (auto& w : pass->writes()) {
        if (w) states_.emplace(w.get(), ResourceState::Undefined);
    }
    passes_.push_back(std::move(pass));
}

RenderGraph::PassClass RenderGraph::classify(const ::velk::IRenderPass& pass)
{
    // Surface-blit seam → Blit (post-pass dest in color-attachment layout).
    // Cmd buffer with a target_id / target_texture / target_group → Raster.
    // Cmd buffer alone (no target) → Compute (compute dispatch, possibly
    //   followed by an in-cmd-buffer record_blit_to_texture; the post-
    //   pass writes list reflects what was written).
    // Empty / barrier-only pass → Raster as a default barrier dst.
    if (pass.surface_blit_source() != nullptr) return PassClass::Blit;
    if (pass.command_buffer()) {
        if (pass.target_group() || pass.target_texture() || pass.target_id() != 0) {
            return PassClass::Raster;
        }
        return PassClass::Compute;
    }
    return PassClass::Raster;
}

void RenderGraph::compile()
{
    VELK_PERF_SCOPE("renderer.graph_compile");
    // Short-circuit: when the pass Ptr list is identical to last
    // successful compile's, the prior barriers_ (and resource state
    // machine output it derives from) are still valid. Reuse them.
    if (passes_.size() == prev_passes_.size()) {
        bool match = true;
        for (size_t i = 0; i < passes_.size(); ++i) {
            if (passes_[i] != prev_passes_[i]) { match = false; break; }
        }
        if (match) return;
    }

    barriers_.assign(passes_.size(), Barrier{});
    states_.clear();

    /// Coarse per-resource state machine. For each pass that consumes
    /// data (i.e., any work-doing pass — bindless reads aren't declared
    /// so we conservatively assume every pass might sample any prior
    /// writer), we scan the state map for resources still in a
    /// writeable state, emit a single transition barrier from their
    /// producer stage to this pass's consumer stage, and flip them to
    /// ShaderRead. Then we transition each declared write into the
    /// post-state implied by this pass's class.
    auto consumer_stage = [](PassClass c) {
        switch (c) {
        case PassClass::Raster:  return ::velk::PipelineStage::FragmentShader;
        case PassClass::Compute: return ::velk::PipelineStage::ComputeShader;
        case PassClass::Blit:    return ::velk::PipelineStage::ComputeShader;
        }
        return ::velk::PipelineStage::FragmentShader;
    };

    auto write_state = [](PassClass c) {
        switch (c) {
        case PassClass::Raster:  return ResourceState::ColorWrite;
        case PassClass::Compute: return ResourceState::Storage;
        case PassClass::Blit:    return ResourceState::ColorWrite;
        }
        return ResourceState::ColorWrite;
    };

    /// Raster passes that target an RTT texture / MRT group write a
    /// fresh target without sampling prior graph resources. Skip the
    /// pre-pass barrier for them (matches old behaviour where the skip
    /// fired only when the raster pass declared writes).
    auto skip_pre_barrier = [](const ::velk::IRenderPass& pass, PassClass c) {
        if (c != PassClass::Raster) return false;
        if (!pass.command_buffer()) return false;
        if (!pass.target_group() && !pass.target_texture() && pass.target_id() == 0) {
            return false;
        }
        return !pass.writes().empty();
    };

    for (size_t i = 0; i < passes_.size(); ++i) {
        const ::velk::IRenderPass& gp = *passes_[i];
        auto& barrier = barriers_[i];

        PassClass cls = classify(gp);

        if (!skip_pre_barrier(gp, cls)) {
            ::velk::PipelineStage src_stage = ::velk::PipelineStage::ColorOutput;
            bool need_barrier = false;
            for (auto& [r, st] : states_) {
                if (st == ResourceState::ColorWrite) {
                    src_stage = ::velk::PipelineStage::ColorOutput;
                    need_barrier = true;
                } else if (st == ResourceState::Storage) {
                    src_stage = ::velk::PipelineStage::ComputeShader;
                    need_barrier = true;
                }
            }
            if (need_barrier) {
                barrier.emit = true;
                barrier.src = src_stage;
                barrier.dst = consumer_stage(cls);
                for (auto& [r, st] : states_) {
                    if (st == ResourceState::ColorWrite || st == ResourceState::Storage) {
                        st = ResourceState::ShaderRead;
                    }
                }
            }
        }

        ResourceState new_state = write_state(cls);
        for (auto& w : gp.writes()) {
            if (w) states_[w.get()] = new_state;
        }
    }

    // Snapshot the just-compiled pass list for next frame's match
    // check. Done only on the rebuild path: short-circuit hits return
    // early with `prev_passes_` already correctly reflecting the
    // current `passes_` from the prior compile.
    prev_passes_ = passes_;
}

void RenderGraph::execute(::velk::IRenderBackend& backend)
{
    VELK_PERF_SCOPE("renderer.graph_execute");

    RENDER_LOG("graph.execute passes=%zu", passes_.size());

    for (size_t i = 0; i < passes_.size(); ++i) {
        const ::velk::IRenderPass& gp = *passes_[i];
        auto& barrier = barriers_[i];

        if (barrier.emit) {
            RENDER_LOG("graph.barrier src=%d dst=%d", (int)barrier.src, (int)barrier.dst);
            backend.barrier(barrier.src, barrier.dst);
        }

        RENDER_LOG("graph.pass[%zu] target=%llu has_cmd=%d surface_blit=%d",
                   i,
                   (unsigned long long)gp.target_id(),
                   gp.command_buffer() ? 1 : 0,
                   gp.surface_blit_source() ? 1 : 0);

        // Surface-blit seam: per-frame swapchain blit; can't be baked
        // into a cached secondary, recorded onto the primary every
        // frame inside the backend.
        if (auto* src = gp.surface_blit_source()) {
            backend.blit_to_surface(*src,
                                    gp.surface_blit_surface_id(),
                                    gp.surface_blit_rect());
            continue;
        }

        if (auto cmd = gp.command_buffer()) {
            // Group/texture targets route through their typed overloads;
            // surface targets stay on the uint64 overload. Compute /
            // blit cmd buffers leave all three unset and skip the
            // begin_pass / end_pass wrap.
            if (auto* group = gp.target_group()) {
                backend.begin_pass(*group);
                backend.execute(cmd);
                backend.end_pass();
            } else if (auto* tex = gp.target_texture()) {
                backend.begin_pass(*tex);
                backend.execute(cmd);
                backend.end_pass();
            } else if (uint64_t target = gp.target_id()) {
                backend.begin_pass(target);
                backend.execute(cmd);
                backend.end_pass();
            } else {
                backend.execute(cmd);
            }
        }
    }
}

} // namespace velk::impl
