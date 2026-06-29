#include "path/camera_pipeline.h"

#include "path/forward_path.h"

#include <velk/api/velk.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-render/api/cached_view_pass.h>
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
                                      ::velk::uvec2& size_slot,
                                      int width, int height,
                                      ::velk::TextureUsage usage,
                                      ::velk::PixelFormat format,
                                      ::velk::FrameContext& /*ctx*/,
                                      ::velk::IRenderGraph& graph)
{
    ::velk::uvec2 want{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    if (slot && size_slot == want) return slot;

    ::velk::TextureDesc td{};
    td.width = width;
    td.height = height;
    td.format = format;
    td.usage = usage;
    // Persistent allocation: keep the same Ptr across frames so
    // downstream cached passes embed a stable target. Recreate only on
    // size change. (Caller is responsible for refreshing any cached raw
    // IGpuTexture* whenever this slot is recreated.)
    slot = graph.resources().create_render_texture(td);
    if (slot) size_slot = want;
    return slot;
}

void CameraPipeline::release_view_state(ViewState& /*vs*/, ::velk::FrameContext& /*ctx*/)
{
    // path_output and post_output are managed: dropping the Ptrs (when
    // the ViewState is erased) auto-defers the backend handles via the
    // resource manager observer chain.
}

void CameraPipeline::emit(::velk::IViewEntry& view,
                          const ::velk::RenderView& render_view,
                          ::velk::IRenderTarget::Ptr color_target,
                          ::velk::FrameContext& ctx,
                          ::velk::IRenderGraph& graph)
{
    auto* path = resolve_path(ctx);
    if (!path) return;
    if (!color_target || !ctx.backend) return;

    /// The surface is just an `IGpuTexture` from the producer's
    /// perspective. Backend exposes a per-surface composite via
    /// `acquire_swapchain_texture`; backend handles the
    /// composite-to-swap final blit at end_frame and applies LOAD
    /// loadOp on subsequent record_begin_renderings so multi-view
    /// rendering stacks naturally. Multi-camera-to-same-surface, HDR,
    /// and post-process all converge on the same surface texture.
    int w = render_view.width;
    int h = render_view.height;

    auto& vs = view_states_[&view];

    auto post = resolve_post_process();
    const bool has_post = (post != nullptr);

    const uint64_t surface_id =
        color_target->get_gpu_handle(::velk::GpuResourceKey::Default);
    auto swap_target = ctx.backend->acquire_swapchain_texture(surface_id);
    if (!swap_target) return;
    // The composite is paired with depth — ForwardPath allocates its own
    // depth attachment when color_target carries a depth format.
    swap_target->set_depth_format(::velk::DepthFormat::Default);

    ::velk::PixelFormat saved_format = ctx.target_format;
    // Pipelines that write directly into the composite (no-post case)
    // must compile against the composite's actual format. Pipelines
    // that write into the per-camera path_target (post case) compile
    // against RGBA16F so HDR values survive into tonemap.
    ctx.target_format = has_post
        ? ::velk::PixelFormat::RGBA16F
        : swap_target->format();

    if (has_post) {
        // HDR path: path → path_target (RenderTarget RGBA16F) → post →
        // post_target (Storage RGBA16F) → blit → swap_target. The
        // intermediate post_target keeps tonemap's imageStore happy
        // (composite is bound for raster + final blit, not as
        // storage-by-bindless).
        auto path_target = ensure_storage_target(vs.path_output, vs.path_size, w, h,
                                                 ::velk::TextureUsage::RenderTarget,
                                                 ::velk::PixelFormat::RGBA16F, ctx, graph);
        auto post_target = ensure_storage_target(vs.post_output, vs.post_size, w, h,
                                                 ::velk::TextureUsage::Storage,
                                                 ::velk::PixelFormat::RGBA16F, ctx, graph);
        if (!path_target || !post_target) {
            ctx.target_format = saved_format;
            return;
        }
        path_target->set_depth_format(::velk::DepthFormat::Default);
        path->build_passes(view, render_view, path_target, ctx, graph);
        seen_post_[post.get()] = post;
        post->emit(view, path_target, post_target, ctx, graph);

        // Blit post-process output into the surface composite. Cached per
        // view; the post-process output and the per-surface composite are both
        // stable Ptrs (recreated only on resize), so the recorded blit is
        // reused every frame and re-recorded only when either texture changes.
        // (per-slot cross-slot find_texture caveat is a non-issue here because
        // post_output's Ptr stays stable across this view's slot rotation.)
        ::velk::IGpuTexture* post_tex = graph.resources().find_texture(post_target.get());
        ::velk::IGpuTexture* swap_tex = interface_cast<::velk::IGpuTexture>(swap_target.get());
        if (post_tex && swap_tex) {
            if (post_tex != vs.last_post_tex || swap_tex != vs.last_swap_tex) {
                vs.composite_blit_dirty = true;
                vs.last_post_tex = post_tex;
                vs.last_swap_tex = swap_tex;
            }
            ::velk::emit_cached_view_pass(
                vs.cached_composite_blit, vs.composite_blit_dirty, "composite.blit",
                render_view.view_globals_address, graph,
                [&](::velk::CachedPassRecording& rec) {
                    if (auto cmd = ctx.backend->create_command_buffer()) {
                        cmd->begin_recording();
                        cmd->push_label("CameraPipeline: post -> composite");
                        cmd->record_blit_to_texture(*post_tex, *swap_tex, render_view.viewport);
                        cmd->pop_label();
                        cmd->end_recording();
                        rec.cmd = std::move(cmd);
                    }
                    rec.reads.push_back(interface_pointer_cast<::velk::IGpuResource>(post_target));
                    rec.writes.push_back(interface_pointer_cast<::velk::IGpuResource>(swap_target));
                });
        }
    } else {
        // No post-process: path renders directly to the surface
        // composite. Multi-view (e.g., main camera + perf overlay
        // camera on the same surface) is handled inside the backend
        // — first record on the composite each frame is fine; the
        // second record's loadOp is silently overridden to LOAD so
        // draws stack on top of begin_frame's clear + earlier views.
        path->build_passes(view, render_view, swap_target, ctx, graph);
    }

    ctx.target_format = saved_format;
}

void CameraPipeline::on_view_removed(::velk::IViewEntry& view, ::velk::FrameContext& ctx)
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
