#ifndef VELK_RENDER_FRAME_RENDER_PASS_H
#define VELK_RENDER_FRAME_RENDER_PASS_H

#include <variant>

#include <velk/vector.h>

#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/render_types.h>

namespace velk {

/**
 * @brief One GPU command to execute. Each variant alternative maps 1:1
 *        to a backend method.
 *
 * Paths emit a sequence of these into a `GraphPass`. The graph's
 * executor walks them in order with `std::visit` and calls the
 * matching backend method. Adding new backend primitives (cross-frame
 * resource transitions, indirect dispatches, etc.) means adding a new
 * struct + visit case — no central enum to grow.
 */
namespace ops {

/// `IRenderBackend::begin_pass(target_id)`. `target_id` is a TextureId,
/// surface id, or RenderTargetGroup handle (the backend dispatches by
/// the high-bit tag bits).
struct BeginPass
{
    uint64_t target_id = 0;
};

/// `IRenderBackend::submit(calls, viewport)`. Owns its draw-call list
/// because it's typically large per-pass. Use std::move to transfer in.
struct Submit
{
    rect viewport{};
    vector<DrawCall> draw_calls;
};

/// `IRenderBackend::end_pass()`.
struct EndPass {};

/// `IRenderBackend::dispatch(calls)`. Holds a single dispatch — emit
/// multiple Dispatch ops in sequence for batched dispatches.
struct Dispatch
{
    DispatchCall call{};
};

/// `IRenderBackend::blit_to_surface(source, surface_id, dst_rect)`.
/// The source IGpuTexture must outlive op execution; producers hold an
/// owning Ptr in their cached pass state alongside the op list.
struct BlitToSurface
{
    IGpuTexture* source = nullptr;
    uint64_t surface_id = 0;
    rect dst_rect{};
};

/// `IRenderBackend::blit_to_texture(source, dest, dst_rect)`. Used when
/// a non-cached pass needs to copy a storage texture into another
/// renderable / sampleable texture (post-process intermediate, RTT
/// composite, etc.). Both Ptrs must outlive op execution.
struct BlitToTexture
{
    IGpuTexture* source = nullptr;
    IGpuTexture* dest = nullptr;
    rect dst_rect{};
};

/// `IRenderBackend::blit_group_depth_to_surface(group, surface_id, dst_rect)`.
/// Currently only used by the deferred path to plumb G-buffer depth
/// into the swapchain's depth buffer for forward overlays.
struct BlitGroupDepthToSurface
{
    IRenderTextureGroup* src_group = nullptr;
    uint64_t surface_id = 0;
    rect dst_rect{};
};

} // namespace ops

using GraphOp = std::variant<
    ops::BeginPass,
    ops::Submit,
    ops::EndPass,
    ops::Dispatch,
    ops::BlitToSurface,
    ops::BlitToTexture,
    ops::BlitGroupDepthToSurface>;

} // namespace velk

#endif // VELK_RENDER_FRAME_RENDER_PASS_H
