#ifndef VELK_RENDER_INTF_RENDER_PASS_H
#define VELK_RENDER_INTF_RENDER_PASS_H

#include <velk/array_view.h>
#include <velk/interface/intf_interface.h>
#include <velk/uid.h>
#include <velk/vector.h>

#include <velk-render/frame/render_pass.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_backend.h>

#include <cstdint>

namespace velk {

/**
 * @brief One pass added to the render graph.
 *
 * Carries an ordered sequence of GPU ops + explicit read/write resource
 * declarations + the per-pass FrameGlobals address. The graph's
 * executor walks the ops 1:1 to backend methods; the graph's compile
 * step inspects ops + reads/writes to classify the pass and insert
 * pipeline barriers before consumers of prior writes.
 *
 * Ptr-based so the velk hive pools allocations and producer pipelines
 * can cache per-frame Ptr identity for future persistent-pass work.
 *
 * Bindless texture reads from materials are NOT declared — they're
 * invisible to the graph and stay fence-synced at the descriptor-set
 * level. Tier 1 tracks resources at coarse granularity: gbuffer
 * attachments are tracked through the group resource (not per-
 * attachment), and non-Ptr handles (raw `TextureId`,
 * `RenderTargetGroup`) are out of scope.
 */
class IRenderPass
    : public Interface<IRenderPass, IInterface,
                       VELK_UID("ffc6e6c3-639a-461e-ad5c-7bc4ed902edf")>
{
public:
    /// Ordered ops executed in sequence. Typical shapes:
    ///   - Raster pass:        BeginPass, Submit, EndPass
    ///   - GBuffer fill:       BeginPass, Submit, EndPass (target_id is the group handle)
    ///   - Compute dispatch:   Dispatch
    ///   - Compute + blit:     Dispatch, BlitToSurface[, BlitGroupDepthToSurface]
    ///   - Pure blit:          BlitToSurface
    virtual array_view<const GraphOp> ops() const = 0;

    /// Resources read by this pass.
    virtual array_view<const IGpuResource::Ptr> reads() const = 0;

    /// Resources written by this pass.
    virtual array_view<const IGpuResource::Ptr> writes() const = 0;

    /// Per-view FrameGlobals GPU address pushed into push-constant
    /// slot [0..8) at pass start. Shaders dereference it as a
    /// `GlobalData` buffer-reference / device-address read (declared
    /// in the velk.glsl prelude). Returning 0 means the pass doesn't
    /// bind a globals address (the executor leaves whatever was
    /// previously pushed).
    virtual uint64_t view_globals_address() const = 0;

    /// @name Producer mutators
    /// @{
    /// Append one GPU op to the pass's op stream. Order matters: the
    /// graph executor walks the stream verbatim.
    virtual void add_op(GraphOp op) = 0;

    /// Declare a resource the pass reads. Inserted into the dependency
    /// state machine so prior writers get a barrier before this pass.
    virtual void add_read(IGpuResource::Ptr resource) = 0;

    /// Declare a resource the pass writes. Drives the post-pass
    /// resource state and surfaces in the renderer's overlap-discard
    /// scan.
    virtual void add_write(IGpuResource::Ptr resource) = 0;

    /// Set the FrameGlobals GPU address pushed at pass start. Pass
    /// 0 (default) for passes that don't touch view-level state — the
    /// executor leaves whatever was previously pushed.
    virtual void set_view_globals_address(uint64_t addr) = 0;
    /// @}
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_PASS_H
