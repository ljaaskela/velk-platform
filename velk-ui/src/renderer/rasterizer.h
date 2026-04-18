#ifndef VELK_UI_RASTERIZER_H
#define VELK_UI_RASTERIZER_H

#include "view_renderer.h"

#include <velk/vector.h>

#include <unordered_map>
#include <velk-render/interface/intf_camera.h>

namespace velk::ui {

/**
 * @brief Per-view renderer that emits classic graphics draw passes.
 *
 * Owns the RenderToTexture (RTT) texture cache used by elements with
 * RenderCache traits. Activated when a view's camera has
 * render_path == RenderPath::Raster (or no camera).
 *
 * Uses (but does not own) the Renderer-hosted BatchBuilder via the
 * FrameContext; that cache is shared with the GPU-resource-upload pass
 * in Renderer::consume_scenes, so it cannot live solely here.
 */
class Rasterizer : public IViewRenderer
{
public:
    void build_passes(ViewEntry& view,
                      const SceneState& scene_state,
                      FrameContext& ctx,
                      vector<RenderPass>& out_passes) override;

    void build_shared_passes(FrameContext& ctx,
                             vector<RenderPass>& out_passes) override;

    void on_view_removed(ViewEntry& view, FrameContext& ctx) override;
    void on_element_removed(IElement* elem, FrameContext& ctx) override;
    void shutdown(FrameContext& ctx) override;

private:
    struct RenderTargetEntry
    {
        IRenderTarget::Ptr target;
        TextureId texture_id = 0;
        int width = 0;
        int height = 0;
        bool dirty = true;
    };

    std::unordered_map<IElement*, RenderTargetEntry> render_target_entries_;

    // Inserts an env fullscreen batch at the front of view.batches when the
    // camera has an environment. Relies on ensure_env_ready to do the
    // texture upload.
    void prepend_environment_batch(ICamera& camera, ViewEntry& view, FrameContext& ctx);

    // Ensures the view's G-buffer render target group exists at exactly
    // `width x height`. Creates on first call; reallocates on resize.
    // Returns the group handle (0 on failure or zero-size viewport).
    RenderTargetGroup ensure_gbuffer(ViewEntry& view, int width, int height,
                                     FrameContext& ctx);
};

} // namespace velk::ui

#endif // VELK_UI_RASTERIZER_H
