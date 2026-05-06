#ifndef VELK_RENDER_FRAME_INTF_RENDER_GRAPH_H
#define VELK_RENDER_FRAME_INTF_RENDER_GRAPH_H

#include <velk/interface/intf_interface.h>
#include <velk/uid.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/interface/intf_render_state.h>

namespace velk {

/**
 * @brief Per-frame collection of passes with declared resource flow.
 *
 * Tier 1 contract:
 *
 * - Identifies resources by `IGpuResource::Ptr` identity. The Ptr
 *   keeps the resource alive for the graph's lifetime.
 * - Pipelines / paths call `add_pass` to enqueue work. Insertion
 *   order is preserved (no topo reorder in Tier 1; the existing emit
 *   order is correct).
 * - `compile()` walks the pass list and inserts barriers based on a
 *   per-resource state machine.
 * - `execute(IRenderBackend&)` walks the compiled list and dispatches
 *   each pass to the backend, replacing the per-PassKind switch +
 *   `had_texture_pass` heuristic that lived in `Renderer::present()`.
 *
 * Owned per `FrameSlot` on the Renderer. `clear()`-ed at frame start.
 *
 * NOT in Tier 1: transient resource pool (Tier 2), pass culling /
 * merging (Tier 3), pass-Ptr caching (see design-notes/persistent_passes.md).
 */
class IRenderGraph
    : public Interface<IRenderGraph, IRenderState,
                       VELK_UID("cd1a3708-c96f-4b81-8ed3-b68ce11d133e")>
{
public:
    /** @brief True if no passes have been added. */
    virtual bool empty() const = 0;

    /** @brief Releases all passes, imports, and per-frame state. */
    virtual void clear() = 0;

    /**
     * @brief Registers an externally-allocated resource for state tracking.
     *
     * Tier 1 doesn't strictly require this — `add_pass` adds resources
     * to the state map on first sight. Provided for forward
     * compatibility with Tier 2 (transient pool).
     */
    virtual void import(const IGpuResource::Ptr& resource) = 0;

    /** @brief Appends a pass. The graph takes shared ownership of the Ptr. */
    virtual void add_pass(IRenderPass::Ptr pass) = 0;

    /**
     * @brief Tier 1 compile: assigns barriers based on per-resource
     *        state transitions. Insertion order is execution order.
     */
    virtual void compile() = 0;

    /**
     * @brief Walks compiled passes and dispatches each to @p backend.
     *
     * Replaces the per-PassKind switch in the old `Renderer::present()`
     * loop. Inserts the per-pass barriers computed by `compile()`.
     */
    virtual void execute(IRenderBackend& backend) = 0;

    /**
     * @brief Mutable access to the pass list, for outside concerns
     *        like the Renderer's frame-overlap discard logic that
     *        needs to walk Raster targets and prune overlapping passes
     *        from older frames. Tier 2 graph deps will subsume this.
     */
    virtual vector<IRenderPass::Ptr>& passes() = 0;
    virtual const vector<IRenderPass::Ptr>& passes() const = 0;

    /**
     * @brief Per-frame transient resource manager owned by the graph.
     *
     * Pipelines that allocate intermediate render targets / groups /
     * buffers go through this manager, distinct from the renderer's
     * persistent `IGpuResourceManager`. With Tier 2 + aliasing, the
     * graph re-binds the physical handles backing these allocations
     * to compact non-overlapping lifetimes onto a shared pool.
     *
     * Use the persistent manager (`FrameContext::resources`) for
     * resources that outlive a frame (mesh buffers, image textures,
     * pipelines). Use the graph's manager for resources whose
     * lifetime is bounded by the frame.
     */
    virtual IGpuResourceManager& resources() = 0;
};

} // namespace velk

#endif // VELK_RENDER_FRAME_INTF_RENDER_GRAPH_H
