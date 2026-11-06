#ifndef VELK_RENDER_INTF_RENDER_GRAPH_INTERNAL_H
#define VELK_RENDER_INTF_RENDER_GRAPH_INTERNAL_H

#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_graph.h>

namespace velk {

/**
 * @brief Framework-internal lifecycle API for the render graph.
 *
 * Pipelines / paths see only `IRenderGraph` (add_pass, compile,
 * execute, resources). The Renderer drives backend wiring via this
 * sibling interface so the public surface stays minimal.
 */
class IRenderGraphInternal
    : public Interface<IRenderGraphInternal, IRenderGraph>
{
public:
    /// Gives the graph the transient `IGpuResourceManager` that `resources()`
    /// returns. Called by Renderer right after the graph is created. Every
    /// frame slot's graph shares one manager, because textures created through
    /// it can outlive the frame and are looked up again from later slots.
    virtual void init(const IGpuResourceManager::Ptr& resources) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_GRAPH_INTERNAL_H
