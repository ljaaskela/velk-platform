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
 * RenderCache traits. Activated for views whose camera has
 * render_path == RenderPath::Forward or Deferred (i.e. any non-RT
 * view). For Deferred views the rasterizer additionally emits a
 * G-buffer fill pass; Forward views skip it.
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

    // Ensures every render-target element in batch_builder's
    // render_target_passes has an allocated backend texture and its
    // render_target_id set on the RenderTarget object. Called from
    // build_passes BEFORE emit_forward_pass so that build_draw_calls
    // for the main pass sees correct render-target ids when resolving
    // texture_keys that point at RTT surfaces.
    void ensure_render_targets(FrameContext& ctx);

    // Inserts an env fullscreen batch at the front of view.batches when the
    // camera has an environment. Relies on ensure_env_ready to do the
    // texture upload.
    void prepend_environment_batch(ICamera& camera, ViewEntry& view, FrameContext& ctx);

    // Ensures the view's G-buffer render target group exists at exactly
    // `width x height`. Creates on first call; reallocates on resize.
    // Returns the group handle (0 on failure or zero-size viewport).
    RenderTargetGroup ensure_gbuffer(ViewEntry& view, int width, int height,
                                     FrameContext& ctx);

    // Forward path: one surface draw pass. Env batch (prepended in
    // rebuild) renders first, then scene geometry.
    void emit_forward_pass(ViewEntry& view, FrameContext& ctx,
                           uint64_t globals_gpu_addr,
                           const rect& viewport,
                           vector<RenderPass>& out_passes);

    // Deferred path: G-buffer fill only. DeferredLighter emits the
    // compute lighting pass; composite-to-surface is a later milestone.
    void emit_deferred_gbuffer_pass(ViewEntry& view, FrameContext& ctx,
                                    int width, int height,
                                    uint64_t globals_gpu_addr,
                                    vector<RenderPass>& out_passes);
};

} // namespace velk::ui

#endif // VELK_UI_RASTERIZER_H
