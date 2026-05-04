#include "path/camera_pipeline.h"

#include "path/forward_path.h"

#include <velk/api/velk.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/plugin.h>

namespace velk::impl {

CameraPipeline::CameraPipeline()
    : fallback_path_(::velk::instance().create<::velk::IRenderPath>(
          ::velk::ClassId::Path::Forward))
{
}

::velk::IRenderPath* CameraPipeline::resolve_path(const ::velk::FrameContext& ctx) const
{
    if (ctx.view_camera_trait) {
        if (auto p = ::velk::find_attachment<::velk::IRenderPath>(ctx.view_camera_trait)) {
            return p.get();
        }
    }
    return fallback_path_.get();
}

::velk::IPostProcess::Ptr CameraPipeline::resolve_post_process()
{
    /// PostProcess discovery on the pipeline itself: the user attaches
    /// one root IPostProcess (often a `PostProcessChain`). Multi-root
    /// support is a future concern; for now first-wins. The
    /// `IViewPipeline*` cast disambiguates the diamond on the way to
    /// IInterface; find_attachment then resolves IObjectStorage.
    ::velk::IInterface* self = static_cast<::velk::IViewPipeline*>(this);
    return ::velk::find_attachment<::velk::IPostProcess>(self);
}

::velk::IRenderPath::Needs CameraPipeline::needs(const ::velk::FrameContext& ctx) const
{
    if (auto* path = resolve_path(ctx)) {
        return path->needs();
    }
    return {};
}

::velk::IRenderTarget::Ptr
CameraPipeline::ensure_storage_target(::velk::IRenderTarget::Ptr& slot,
                                      int width, int height,
                                      ::velk::TextureUsage usage,
                                      ::velk::PixelFormat format,
                                      ::velk::FrameContext& /*ctx*/,
                                      ::velk::IRenderGraph& graph)
{
    ::velk::TextureDesc td{};
    td.width = width;
    td.height = height;
    td.format = format;
    td.usage = usage;
    // Per-frame allocation: drop the prior Ptr (manager parks the
    // handle on its pool) and request a fresh one. Pool reuse kicks
    // in once the parked handle's GPU work has retired.
    slot = graph.resources().create_render_texture(td);
    return slot;
}

void CameraPipeline::release_view_state(ViewState& /*vs*/, ::velk::FrameContext& /*ctx*/)
{
    // path_output and post_output are managed: dropping the Ptrs (when
    // the ViewState is erased) auto-defers the backend handles via the
    // resource manager observer chain.
}

void CameraPipeline::emit(::velk::ViewEntry& view,
                          const ::velk::RenderView& render_view,
                          ::velk::IRenderTarget::Ptr color_target,
                          ::velk::FrameContext& ctx,
                          ::velk::IRenderGraph& graph)
{
    auto* path = resolve_path(ctx);
    if (!path) return;

    auto post = resolve_post_process();
    if (!post) {
        // No post-process: path renders directly to color_target.
        path->build_passes(view, render_view, color_target, ctx, graph);
        return;
    }

    /// Pipeline owns two storage intermediates per view:
    ///   path → path_target  (compute / raster outputs)
    ///   post → post_output  (post-process compute outputs)
    /// Then a final Blit copies post_output to the surface, since
    /// surfaces can't be bound as storage images for `imageStore`.
    /// This keeps the post-process contract uniform: callers always
    /// pass a storage target as `output`, never a window surface.
    int w = render_view.width;
    int h = render_view.height;

    auto& vs = view_states_[&view];

    /// path_target: RenderTarget so ForwardPath can `begin_pass` it,
    ///   DeferredPath can blit to it, and post-process effects can
    ///   sample it via bindless. NOT writable from compute shaders
    ///   (so a path that writes via Compute must blit, not imageStore).
    /// post_target: Storage so post-process compute shaders can
    ///   `imageStore` here. Sampleable too, so a final blit reads it.
    /// HDR intermediates: cameras with a post-process attached
    /// (typically a tonemap chain) render through RGBA16F so the
    /// effects compose in linear high-range space. The final blit
    /// converts RGBA16F to the swapchain's display format in one GPU
    /// op. ctx.target_format flows to forward pipelines so they
    /// compile against an RGBA16F render pass.
    /// Save and restore so subsequent views (e.g. an overlay camera
    /// added to the same surface) don't inherit RGBA16F and compile
    /// pipelines that mismatch the swapchain's render pass.
    ::velk::PixelFormat saved_format = ctx.target_format;
    ctx.target_format = ::velk::PixelFormat::RGBA16F;

    auto path_target = ensure_storage_target(vs.path_output, w, h,
                                             ::velk::TextureUsage::RenderTarget,
                                             ::velk::PixelFormat::RGBA16F, ctx, graph);
    auto post_target = ensure_storage_target(vs.post_output, w, h,
                                             ::velk::TextureUsage::Storage,
                                             ::velk::PixelFormat::RGBA16F, ctx, graph);
    if (!path_target || !post_target) {
        // Allocation failure: fall back to direct rendering. Restore
        // the format first so the path compiles pipelines matching the
        // swapchain rather than the HDR target we couldn't allocate.
        ctx.target_format = saved_format;
        path->build_passes(view, render_view, color_target, ctx, graph);
        return;
    }

    path->build_passes(view, render_view, path_target, ctx, graph);
    seen_post_[post.get()] = post;
    post->emit(view, path_target, post_target, ctx, graph);

    // Final blit from post-process output to the actual surface.
    auto blit = ::velk::instance().create<::velk::IRenderPass>(::velk::ClassId::DefaultRenderPass);
    if (blit) {
        blit->add_op(::velk::ops::BlitToSurface{
            static_cast<::velk::TextureId>(
                post_target->get_gpu_handle(::velk::GpuResourceKey::Default)),
            color_target
                ? color_target->get_gpu_handle(::velk::GpuResourceKey::Default)
                : 0,
            render_view.viewport});
        blit->add_read(post_target);
        if (color_target) {
            blit->add_write(color_target);
        }
        graph.add_pass(std::move(blit));
    }

    ctx.target_format = saved_format;
}

void CameraPipeline::on_view_removed(::velk::ViewEntry& view, ::velk::FrameContext& ctx)
{
    if (auto* path = resolve_path(ctx)) {
        path->on_view_removed(view, ctx);
    }
    for (auto& [raw, ptr] : seen_post_) {
        ptr->on_view_removed(view, ctx);
    }
    auto it = view_states_.find(&view);
    if (it != view_states_.end()) {
        release_view_state(it->second, ctx);
        view_states_.erase(it);
    }
}

void CameraPipeline::shutdown(::velk::FrameContext& ctx)
{
    if (fallback_path_) {
        fallback_path_->shutdown(ctx);
    }
    for (auto& [raw, ptr] : seen_post_) {
        ptr->shutdown(ctx);
    }
    seen_post_.clear();
    for (auto& [v, vs] : view_states_) {
        release_view_state(vs, ctx);
    }
    view_states_.clear();
    // Attached paths are owned by the camera trait and shut down when
    // the trait drops them; the pipeline only owns its fallback.
}

} // namespace velk::impl
