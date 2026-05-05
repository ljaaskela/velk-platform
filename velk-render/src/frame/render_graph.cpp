#include "frame/render_graph.h"

#include <velk/api/velk.h>
#include <velk/interface/intf_interface.h>

#include <velk-render/detail/intf_gpu_resource_manager_internal.h>
#include <velk-render/plugin.h>

namespace velk::impl {

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
    passes_.clear();
    barriers_.clear();
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
    // Last-op-wins: the post-pass resource state matches the kind of
    // the last work-doing op. Submit -> ColorWrite (raster); Dispatch
    // alone -> Storage (compute); BlitToSurface (with or without a
    // preceding Dispatch) -> ColorWrite (blit destination ends up in
    // a color-attachment-readable layout).
    bool has_submit = false;
    bool has_dispatch = false;
    bool has_blit = false;
    for (auto& op : pass.ops()) {
        if (std::holds_alternative<::velk::ops::Submit>(op)) has_submit = true;
        else if (std::holds_alternative<::velk::ops::Dispatch>(op)) has_dispatch = true;
        else if (std::holds_alternative<::velk::ops::BlitToSurface>(op)
                 || std::holds_alternative<::velk::ops::BlitGroupDepthToSurface>(op)) {
            has_blit = true;
        }
    }
    if (has_blit) return PassClass::Blit;
    if (has_submit) return PassClass::Raster;
    if (has_dispatch) return PassClass::Compute;
    // Empty / barrier-only pass: treat as Raster for default barrier dst.
    return PassClass::Raster;
}

void RenderGraph::compile()
{
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

    /// Raster passes that target an RTT texture write a fresh target
    /// without sampling prior graph resources. Skip the pre-pass barrier
    /// for them (matches old `reads_textures(Raster, raster_to_texture)`).
    auto skip_pre_barrier = [](const ::velk::IRenderPass& pass, PassClass c) {
        if (c != PassClass::Raster) return false;
        for (auto& op : pass.ops()) {
            if (auto* bp = std::get_if<::velk::ops::BeginPass>(&op)) {
                // A BeginPass on a non-zero target could be either a
                // surface or an RTT texture; raster-into-texture is the
                // case we want to skip. We can't tell the difference
                // from the target_id alone (both encode as uint64), so
                // err on the conservative side: only skip when there's
                // a write declared (which is the RTT case — RenderTarget
                // RTT path always pushes its target into writes).
                (void)bp;
                return !pass.writes().empty();
            }
        }
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
}

void RenderGraph::execute(::velk::IRenderBackend& backend)
{
    uint64_t last_vg_addr = 0;

    for (size_t i = 0; i < passes_.size(); ++i) {
        const ::velk::IRenderPass& gp = *passes_[i];
        auto& barrier = barriers_[i];

        if (barrier.emit) {
            backend.barrier(barrier.src, barrier.dst);
        }

        uint64_t vg_addr = gp.view_globals_address();
        if (vg_addr != 0 && vg_addr != last_vg_addr) {
            backend.push_view_globals(vg_addr);
            last_vg_addr = vg_addr;
        }

        for (auto& op : gp.ops()) {
            std::visit([&](auto& o) {
                using T = std::decay_t<decltype(o)>;
                if constexpr (std::is_same_v<T, ::velk::ops::BeginPass>) {
                    backend.begin_pass(o.target_id);
                } else if constexpr (std::is_same_v<T, ::velk::ops::Submit>) {
                    backend.submit({o.draw_calls.data(), o.draw_calls.size()},
                                   o.viewport);
                } else if constexpr (std::is_same_v<T, ::velk::ops::EndPass>) {
                    backend.end_pass();
                } else if constexpr (std::is_same_v<T, ::velk::ops::Dispatch>) {
                    backend.dispatch({&o.call, 1});
                } else if constexpr (std::is_same_v<T, ::velk::ops::BlitToSurface>) {
                    backend.blit_to_surface(o.source, o.surface_id, o.dst_rect);
                } else if constexpr (std::is_same_v<T,
                                                    ::velk::ops::BlitGroupDepthToSurface>) {
                    backend.blit_group_depth_to_surface(
                        o.src_group, o.surface_id, o.dst_rect);
                }
            }, op);
        }
    }
}

} // namespace velk::impl
