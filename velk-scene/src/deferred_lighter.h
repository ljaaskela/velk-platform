#ifndef VELK_UI_DEFERRED_LIGHTER_H
#define VELK_UI_DEFERRED_LIGHTER_H

#include <unordered_map>

#include "view_renderer.h"

namespace velk {

/**
 * @brief Per-view compute pass that consumes the G-buffer and produces
 *        a fully shaded storage image.
 *
 * The Rasterizer emits a `PassKind::GBufferFill` that populates the
 * per-view G-buffer (albedo / normal / world-pos / material-params).
 * DeferredLighter runs a compute shader immediately after that pass,
 * sampling the G-buffer and writing per-pixel shaded color into a
 * storage image also owned on `ViewEntry`. Blit-to-surface is handled
 * by a later milestone (B.3.b); today the storage image is written
 * but never read.
 *
 * Runs only for views whose camera uses `RenderPath::Deferred`.
 * Forward views stay on the plain rasterizer; RT views produce their
 * final image via the ray-tracer's compute+blit path.
 */
class DeferredLighter : public IViewRenderer
{
public:
    void build_passes(ViewEntry& view,
                      const SceneState& scene_state,
                      FrameContext& ctx,
                      vector<RenderPass>& out_passes) override;

    void on_view_removed(ViewEntry& view, FrameContext& ctx) override;
    void shutdown(FrameContext& ctx) override;

private:
    // Pipeline cache: different intersect-snippet sets compose to
    // different shader variants, each compiled once and kept across
    // frames. Key is an FNV-1a hash of the active intersect id list.
    std::unordered_map<uint64_t, bool> compiled_pipelines_;

    uint64_t ensure_pipeline(FrameContext& ctx);

    // Fullscreen composite pipeline that samples the deferred output
    // texture and alpha-blends it onto the surface. Compiled lazily on
    // first use; shared across views (surface render passes are compatible).
    uint64_t composite_pipeline_key_ = 0;
};

} // namespace velk

#endif // VELK_UI_DEFERRED_LIGHTER_H
