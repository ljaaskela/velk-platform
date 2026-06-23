#ifndef VELK_RENDER_API_CACHED_VIEW_PASS_H
#define VELK_RENDER_API_CACHED_VIEW_PASS_H

#include <velk/api/velk.h>
#include <velk/vector.h>

#include <velk-render/ext/default_render_pass.h>
#include <velk-render/interface/intf_gpu_command_buffer.h>
#include <velk-render/interface/intf_gpu_pipeline.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_graph.h>
#include <velk-render/interface/intf_render_pass.h>

namespace velk {

/**
 * @brief What a render path's recorder produces for one cached pass.
 *
 * Filled by the `record` callback of `emit_cached_view_pass` on a rebuild:
 * the pre-recorded command buffer, the pipelines to keep alive (the weak
 * pipeline cache holds only weak refs, so the pass is the strong owner),
 * and the pass's resource read/write declarations for the graph.
 */
struct CachedPassRecording
{
    IGpuCommandBuffer::Ptr cmd;
    vector<IGpuPipeline::Ptr> held;
    vector<IGpuResource::Ptr> reads;
    vector<IGpuResource::Ptr> writes;
};

/**
 * @brief Drives the persistent per-view `IRenderPass` lifecycle shared by
 *        every render path (forward, gbuffer, lighting, RT).
 *
 * The pass identity is stable across frames so the render graph's
 * compile-time short-circuit can match. This helper owns the boilerplate:
 *
 *  - get-or-create the cached pass (first sight forces a rebuild);
 *  - when not dirty, refresh only the per-frame view-globals address and
 *    re-add the same Ptr (steady state);
 *  - when dirty, `reset()` the pass, invoke @p record for the recorder-
 *    specific work, install the command buffer / resource deps / held
 *    pipelines, clear the dirty flag, and add the pass to the graph.
 *
 * @p record runs ONLY on a rebuild. Callers signal a rebuild by setting
 * @p dirty before the call (e.g. on a target-texture / resize change);
 * content invalidation via the render-state observer sets it too.
 *
 * @param pass   In/out cached pass slot; created on first sight.
 * @param dirty  In/out dirty flag; cleared after a successful rebuild.
 * @param view_globals_address Per-frame FrameGlobals GPU address.
 */
template <class RecordFn>
inline void emit_cached_view_pass(IRenderPass::Ptr& pass, bool& dirty,
                                  uint64_t view_globals_address,
                                  IRenderGraph& graph, RecordFn&& record)
{
    if (!pass) {
        pass = ::velk::instance().create<IRenderPass>(ClassId::DefaultRenderPass);
        if (!pass) {
            return;
        }
        dirty = true;
    }

    if (!dirty) {
        pass->set_view_globals_address(view_globals_address);
        graph.add_pass(pass);
        return;
    }

    pass->reset();

    CachedPassRecording rec;
    record(rec);

    if (rec.cmd) {
        pass->set_command_buffer(std::move(rec.cmd));
    }
    for (auto& r : rec.reads) {
        pass->add_read(r);
    }
    for (auto& w : rec.writes) {
        pass->add_write(w);
    }
    pass->set_view_globals_address(view_globals_address);
    pass->set_held_pipelines(std::move(rec.held));
    dirty = false;
    graph.add_pass(pass);
}

} // namespace velk

#endif // VELK_RENDER_API_CACHED_VIEW_PASS_H
