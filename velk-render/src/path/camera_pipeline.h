#ifndef VELK_RENDER_CAMERA_PIPELINE_H
#define VELK_RENDER_CAMERA_PIPELINE_H

#include <unordered_map>

#include <velk-render/plugin.h>
#include <velk-render/ext/view_pipeline.h>
#include <velk-render/interface/intf_post_process.h>
#include <velk-render/interface/intf_render_path.h>
#include <velk-render/interface/intf_view_pipeline.h>

namespace velk::impl {

/**
 * @brief Default per-camera view pipeline.
 *
 * Auto-attached to every Camera trait at construction time. Composes
 * the IRenderPath also attached to the camera trait; falls back to a
 * built-in ForwardPath when no path is attached.
 *
 * Holds the built-in fallback path so the Renderer doesn't have to.
 * `needs()` forwards from the resolved path so ViewPreparer extracts
 * only the collections the path actually consumes.
 *
 * Lifetime hooks (`on_view_removed`, `shutdown`) forward to the path
 * the pipeline last dispatched against; if a different path gets
 * attached later, the previous path's per-view state is reclaimed when
 * the Renderer drives `seen_pipelines_` teardown.
 */
class CameraPipeline final
    : public ::velk::ext::ViewPipeline<CameraPipeline>
{
public:
    VELK_CLASS_UID(::velk::ClassId::Pipeline::Camera, "CameraPipeline");

    CameraPipeline();

    ::velk::IRenderPath::Needs needs(const ::velk::FrameContext& ctx) const override;

    void emit(::velk::IViewEntry& view,
              const ::velk::RenderView& render_view,
              ::velk::IRenderTarget::Ptr color_target,
              ::velk::FrameContext& ctx,
              ::velk::IRenderGraph& graph) override;

    void on_view_removed(::velk::IViewEntry& view, ::velk::FrameContext& ctx) override;
    void shutdown(::velk::FrameContext& ctx) override;

private:
    /// Pipeline can't reach the camera trait it's attached to; the
    /// Renderer passes the trait pointer in `FrameContext` (see
    /// `FrameContext::view_camera_trait`). Falls back to the built-in
    /// forward path when no IRenderPath is attached.
    ::velk::IRenderPath* resolve_path(const ::velk::FrameContext& ctx) const;

    /// Returns the IPostProcess attached to this pipeline, or null if
    /// none. Discovery via `find_attachment`; if the user attaches
    /// multiple, the first wins (typical setup is a single root, often
    /// a `PostProcessChain`).
    ::velk::IPostProcess::Ptr resolve_post_process();

    /// Per-view storage for the post-process pipeline:
    ///   - `path_output` is the intermediate the path writes into so
    ///     the post-process can read it (path normally targets the
    ///     surface).
    ///   - `post_output` is the intermediate the post-process writes
    ///     into so the pipeline can blit it to the surface (compute
    ///     shaders can't `imageStore` to a window surface).
    /// Both are storage textures; both lazy-allocate / resize.
    struct ViewState
    {
        ::velk::IRenderTarget::Ptr path_output;
        ::velk::IRenderTarget::Ptr post_output;
    };
    std::unordered_map<::velk::IViewEntry*, ViewState> view_states_;

    ::velk::IRenderTarget::Ptr ensure_storage_target(
        ::velk::IRenderTarget::Ptr& slot,
        int width, int height,
        ::velk::TextureUsage usage,
        ::velk::PixelFormat format,
        ::velk::FrameContext& ctx,
        ::velk::IRenderGraph& graph);

    void release_view_state(ViewState& vs, ::velk::FrameContext& ctx);

    ::velk::IRenderPath::Ptr fallback_path_;

    /// Set of post-process roots the pipeline has dispatched against.
    /// On_view_removed / shutdown fan out to each so view-keyed effects
    /// can release their state.
    std::unordered_map<::velk::IPostProcess*, ::velk::IPostProcess::Ptr> seen_post_;
};

} // namespace velk::impl

#endif // VELK_RENDER_CAMERA_PIPELINE_H
