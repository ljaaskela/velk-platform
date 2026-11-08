#ifndef VELK_UI_REFERENCE_PATH_H
#define VELK_UI_REFERENCE_PATH_H

#include <velk-render/ext/render_path.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/interface/intf_render_path.h>
#include <velk-render/interface/intf_view_entry.h>
#include <velk-render/plugin.h>
#include <velk-render/render_path/frame_context.h>

#include <unordered_map>

namespace velk {

/**
 * @brief Path traces the view and accumulates it over frames, for reference images.
 *
 * Starts over when the camera, lights or environment change.
 */
class ReferencePath : public ext::RenderPath<ReferencePath>
{
public:
    VELK_CLASS_UID(ClassId::Path::Reference, "ReferencePath");

    Needs needs() const override
    {
        Needs n;
        n.batches = true;
        n.lights = true;
        return n;
    }

    void build_passes(IViewEntry& view, const RenderView& render_view, IRenderTarget::Ptr color_target,
                      FrameContext& ctx, IRenderGraph& graph) override;
    void on_view_removed(IViewEntry& view, FrameContext& ctx) override;
    void shutdown(FrameContext& ctx) override;

private:
    struct ViewState
    {
        IRenderTarget::Ptr sum;
        IRenderTarget::Ptr shown;
        uvec2 size{};
        uint64_t setup = 0;
        IRenderPass::Ptr pass;
    };

    IGpuPipeline::Ptr ensure_pipeline(FrameContext& ctx);

    std::unordered_map<IViewEntry*, ViewState> view_states_;
};

} // namespace velk

#endif // VELK_UI_REFERENCE_PATH_H
