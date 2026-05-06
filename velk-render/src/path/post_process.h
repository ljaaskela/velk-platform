#ifndef VELK_RENDER_POST_PROCESS_H
#define VELK_RENDER_POST_PROCESS_H

#include <velk/vector.h>

#include <unordered_map>

#include <velk-render/ext/post_process.h>
#include <velk-render/interface/intf_effect.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Linear post-process container.
 *
 * The default `IPostProcess` implementation. Composes attached
 * `IEffect` children in attachment order via per-view intermediate
 * textures. Discovery: `find_attachments<IEffect>(this)` returns the
 * effects; the container runs them sequentially, last child writing
 * directly to the container's `output`.
 *
 * Per-view state (intermediates) keyed off `IViewEntry*`, so one
 * container Ptr can be attached to multiple pipelines / cameras and
 * serve all of them safely. `on_view_removed` releases just that
 * view's slot; `shutdown` clears everything.
 *
 * The container's caller (CameraPipeline or another container)
 * guarantees `output` is a storage-writable texture, so effects
 * never need to special-case "am I writing to a surface?".
 *
 * Tier 1: one intermediate texture per non-last child. Tier 2
 * transient pool will alias these.
 */
class PostProcess final
    : public ::velk::ext::PostProcess<PostProcess>
{
public:
    VELK_CLASS_UID(::velk::ClassId::Post::PostProcess, "PostProcess");

    void emit(::velk::IViewEntry& view,
              ::velk::IRenderTarget::Ptr input,
              ::velk::IRenderTarget::Ptr output,
              ::velk::FrameContext& ctx,
              ::velk::IRenderGraph& graph) override;

    void on_view_removed(::velk::IViewEntry& view, ::velk::FrameContext& ctx) override;
    void shutdown(::velk::FrameContext& ctx) override;

private:
    struct ViewState
    {
        /// One intermediate per non-last effect. Refreshed each frame:
        /// the prior Ptr is dropped and a new one acquired from the
        /// graph's transient pool. Sized to the effect chain length.
        ::velk::vector<::velk::IRenderTarget::Ptr> intermediates;
    };

    std::unordered_map<::velk::IViewEntry*, ViewState> view_states_;

    ::velk::IRenderTarget::Ptr ensure_intermediate(::velk::IViewEntry& view,
                                                   size_t index,
                                                   int width, int height,
                                                   ::velk::FrameContext& ctx,
                                                   ::velk::IRenderGraph& graph);

    void release_view_state(ViewState& vs, ::velk::FrameContext& ctx);
};

} // namespace velk::impl

#endif // VELK_RENDER_POST_PROCESS_H
