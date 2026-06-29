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
    // Every cmd-buffer-bearing pass is self-contained — the secondary
    // internally calls record_begin_rendering for raster or
    // record_dispatch / record_blit_to_texture for compute-style. The
    // graph can't distinguish raster from compute by looking at the
    // pass's outer state. Treat all cmd-buffer passes as Compute for
    // pre-pass barrier purposes (consumer stage = ComputeShader, which
    // covers both raster's vertex pulling and compute reads).
    // Empty / barrier-only pass → Raster as a default barrier dst.
    if (pass.command_buffer()) return PassClass::Compute;
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

    /// Raster passes are not distinguishable from compute at the graph
    /// level (no target_* seams). The skip_pre_barrier optimization
    /// (used to suppress the pre-pass barrier when a raster pass
    /// declared writes against a fresh RTT) doesn't have a clean signal
    /// anymore. Always emit pre-pass barriers; the over-sync is one
    /// extra memory barrier per pass per frame. Revisit if profiling
    /// shows it matters.
    auto skip_pre_barrier = [](const ::velk::IRenderPass&, PassClass) {
        return false;
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

        RENDER_LOG("graph.pass[%zu] has_cmd=%d", i, gp.command_buffer() ? 1 : 0);

        // Time the pass's own GPU work (after the inter-pass barrier so
        // sync cost isn't attributed to it). No-op when timing is off.
        const bool timed = backend.gpu_timing_enabled();
        if (timed) backend.begin_gpu_timer(gp.name());

        if (auto cmd = gp.command_buffer()) {
            // Every cmd-buffer pass is self-contained — raster passes
            // call record_begin_rendering / record_end_rendering
            // internally; compute / blit passes leave the rendering
            // scope alone. Executor never wraps in begin_pass/end_pass.
            backend.execute(cmd);
        }

        if (timed) backend.end_gpu_timer();
    }
}

} // namespace velk::impl
