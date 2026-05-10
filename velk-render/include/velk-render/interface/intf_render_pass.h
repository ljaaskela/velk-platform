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
    virtual IGpuCommandBuffer::Ptr command_buffer() const = 0;
    virtual void set_command_buffer(IGpuCommandBuffer::Ptr cmd) = 0;

    /// Target id for cmd-buffer-bearing raster passes whose target is
    /// a window surface or MRT group. The executor wraps `execute(cmd)`
    /// in `begin_pass(target_id) / end_pass()` so the secondary command
    /// buffer's `vkCmdExecuteCommands` runs inside an active render
    /// pass on the primary. Zero for compute / blit passes (cmd buffer
    /// recorded outside any render pass) and for texture-target raster
    /// passes (use `set_target_texture` instead).
    virtual uint64_t target_id() const = 0;
    virtual void set_target_id(uint64_t target_id) = 0;

    /// Target IGpuTexture for cmd-buffer-bearing raster passes whose
    /// target is a renderable texture. Mutually exclusive with
    /// `set_target_id` and `set_target_group`; whichever is set non-
    /// default routes the `begin_pass` dispatch. Caller must keep the
    /// texture alive (the pass's writes/reads list typically owns the
    /// wrapper).
    virtual IGpuTexture* target_texture() const = 0;
    virtual void set_target_texture(IGpuTexture* texture) = 0;

    /// Target MRT group for cmd-buffer-bearing raster passes whose
    /// target is an `IRenderTextureGroup`. Mutually exclusive with
    /// `set_target_id` / `set_target_texture`.
    virtual IRenderTextureGroup* target_group() const = 0;
    virtual void set_target_group(IRenderTextureGroup* group) = 0;

    /// Surface-blit seam. When `surface_blit_source()` is non-null the
    /// executor calls `IRenderBackend::blit_to_surface` directly,
    /// bypassing the cmd-buffer / target dispatch entirely. Mutually
    /// exclusive with command_buffer + the three target seams. Used
    /// for the per-frame swapchain blit (CameraPipeline final stage,
    /// DeferredPath / RtPath surface dest) — the swapchain image
    /// changes per frame so this can't be baked into a secondary cmd
    /// buffer; it's recorded onto the primary every frame inside the
    /// backend.
    virtual IGpuTexture* surface_blit_source() const = 0;
    virtual uint64_t surface_blit_surface_id() const = 0;
    virtual rect surface_blit_rect() const = 0;
    virtual void set_surface_blit(IGpuTexture* source,
                                  uint64_t surface_id,
                                  rect dst_rect) = 0;
    /// @}
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_PASS_H
