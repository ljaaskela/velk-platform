#ifndef VELK_RENDER_INTF_RENDER_PASS_H
#define VELK_RENDER_INTF_RENDER_PASS_H

#include <velk/array_view.h>
#include <velk/interface/intf_interface.h>
#include <velk/uid.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_gpu_command_buffer.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_state.h>

#include <cstdint>

namespace velk {

/**
 * @brief One pass added to the render graph.
 *
 * Carries a pre-recorded GPU command buffer (or a surface_blit seam)
 * + explicit read/write resource declarations + the per-pass
 * FrameGlobals address. The graph's compile step inspects the seams
 * + reads/writes to classify the pass and insert pipeline barriers
 * before consumers of prior writes.
 *
 * Ptr-based so the velk hive pools allocations and producer pipelines
 * can cache per-frame Ptr identity for persistent-pass work.
 *
 * Bindless texture reads from materials are NOT declared — they're
 * invisible to the graph and stay fence-synced at the descriptor-set
 * level. Tier 1 tracks resources at coarse granularity: gbuffer
 * attachments are tracked through the group resource (not per-
 * attachment).
 */
class IRenderPass
    : public Interface<IRenderPass, IRenderState,
                       VELK_UID("ffc6e6c3-639a-461e-ad5c-7bc4ed902edf")>
{
public:
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

    /// Clear reads, writes, command buffer, target seams, surface-blit
    /// seam, and the view-globals address. Producers that cache an
    /// `IRenderPass::Ptr` across frames call this at the top of each
    /// rebuild so the same Ptr identity carries fresh contents — the
    /// graph's compile-time short-circuit only fires when pass Ptrs
    /// match, so reusing the same Ptr is what makes that path active.
    virtual void reset() = 0;

    /// Pre-recorded secondary command buffer for this pass. Producers
    /// record once and the executor replays it every frame; per-view
    /// FrameGlobals BDAs are stable across frames (see
    /// `prepare_frame_globals`) so the same secondary is correct on
    /// every replay. `reset()` drops the cmd buffer Ptr.
    ///
    /// All raster passes are self-contained dynamic-rendering
    /// secondaries — they call `record_begin_rendering` /
    /// `record_end_rendering` internally; the executor doesn't wrap
    /// the replay in any begin_pass / end_pass.
    virtual IGpuCommandBuffer::Ptr command_buffer() const = 0;
    virtual void set_command_buffer(IGpuCommandBuffer::Ptr cmd) = 0;

    /// Retain strong refs to the GPU pipelines this pass's command buffer
    /// binds. The pipeline cache holds only weak refs, so the recorder is
    /// the strong owner — a pipeline lives as long as some pass that binds
    /// it. Replaces the previously-held set; producers call this once per
    /// re-record with the pipelines their command buffer references. The
    /// set is GPU-object lifetime state, independent of `reset()` (so a
    /// rebuild that bails after `reset()` keeps the old pipelines alive
    /// rather than dropping them mid-flight).
    virtual void set_held_pipelines(vector<IGpuPipeline::Ptr> pipelines) = 0;
    /// @}
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_PASS_H
