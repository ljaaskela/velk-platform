#ifndef VELK_RENDER_INTF_GPU_COMMAND_BUFFER_H
#define VELK_RENDER_INTF_GPU_COMMAND_BUFFER_H

#include <velk/api/math_types.h>
#include <velk/array_view.h>
#include <velk/interface/intf_interface.h>
#include <velk/uid.h>

#include <cstdint>

namespace velk {

// Forward declarations: the handle / draw / dispatch types live in
// `intf_render_backend.h` (they're part of the backend's vocabulary).
// Including that header here would create a cycle since it returns
// `IGpuCommandBuffer::Ptr` — keep the forward decls.
struct DrawCall;
struct DispatchCall;
class IGpuTexture;
class IRenderTextureGroup;
using TextureId = uint32_t;


/**
 * @brief Pre-recorded sequence of GPU commands.
 *
 * Producers (render paths) record once when their pass content
 * changes; the render graph executor replays via
 * `IRenderBackend::execute(cmd)` each frame. This replaces the
 * legacy `GraphOp` flow where the executor walked an op list and
 * the backend translated each op every frame — moving the
 * translation work to "when the pass changes" instead of "every
 * frame".
 *
 * Maps onto Vulkan secondary command buffers, D3D12 command lists /
 * bundles, Metal indirect command buffers. The interface is
 * deliberately narrow: a recording lifecycle (`begin_recording` /
 * `end_recording`) plus the `record_*` family.
 *
 * Lifetime: producers hold `IGpuCommandBuffer::Ptr` in their cached
 * pass state. Dropping the Ptr releases the underlying backend
 * handle through the standard hive-pool destruction; the backend
 * defers the actual free until the in-flight frame completes (so
 * the GPU is never asked to read a freed cmd buffer).
 *
 * Recording is single-shot per call: `begin_recording` resets any
 * previous content. Re-record by calling `begin_recording` again.
 */
class IGpuCommandBuffer
    : public Interface<IGpuCommandBuffer, IInterface,
                       VELK_UID("84f711bf-cb74-4b90-ad55-531121e17c44")>
{
public:
    /// Begin recording a fresh command sequence. Any previously
    /// recorded content is discarded.
    virtual void begin_recording() = 0;

    /// Finish recording. The buffer is now executable via
    /// `IRenderBackend::execute`.
    virtual void end_recording() = 0;

    /// @name Inside-renderpass commands.
    ///
    /// Valid only on cmd buffers created with a non-zero target_id
    /// (the buffer is bound to a render pass for inheritance). The
    /// backend may reject these on a compute-only cmd buffer.
    /// @{

    /// Set the viewport + scissor for subsequent draws. Zero
    /// width/height means "full target" (filled in by the backend
    /// from the current target's dims).
    virtual void set_viewport(rect viewport) = 0;

    /// Record the given draw calls into the buffer. Each `DrawCall`
    /// is bound + drawn in order (BindPipeline + PushConstants +
    /// optional BindIndexBuffer + DrawIndirectCount).
    virtual void record_draws(array_view<const DrawCall> calls) = 0;

    /// @}

    /// @name Outside-renderpass commands.
    /// @{

    /// Record a compute dispatch.
    virtual void record_dispatch(const DispatchCall& call) = 0;

    /// Record a blit from a storage texture into a surface (or a
    /// renderable texture used as a surface stand-in).
    virtual void record_blit_to_surface(IGpuTexture& source,
                                        uint64_t surface_id,
                                        rect dst_rect) = 0;

    /// Record a blit from a storage / renderable texture into another
    /// renderable / sampleable texture. Source layout restored; dest
    /// ends in SHADER_READ_ONLY.
    virtual void record_blit_to_texture(IGpuTexture& source,
                                        IGpuTexture& dest,
                                        rect dst_rect) = 0;

    /// Record a blit of a render-target group's depth attachment
    /// into a surface.
    virtual void record_blit_group_depth_to_surface(
        IRenderTextureGroup& src_group,
        uint64_t surface_id,
        rect dst_rect) = 0;

    /// @}
};

} // namespace velk

#endif // VELK_RENDER_INTF_GPU_COMMAND_BUFFER_H
