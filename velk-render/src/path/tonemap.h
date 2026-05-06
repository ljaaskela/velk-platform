#ifndef VELK_RENDER_TONEMAP_H
#define VELK_RENDER_TONEMAP_H

#include <velk-render/ext/effect.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief ACES filmic tonemap effect.
 *
 * Reads `input` as a bindless texture, writes the tonemapped result to
 * `output` as a storage image. One compute dispatch per view, sized to
 * the input dimensions.
 *
 * Stateless across views (no per-view caches), but compiles its
 * compute pipeline lazily on first emit. The pipeline is identical
 * across views and shared via the renderer's pipeline_map.
 */
class Tonemap final
    : public ::velk::ext::Effect<Tonemap>
{
public:
    VELK_CLASS_UID(::velk::ClassId::Effect::Tonemap, "Tonemap");

    void emit(::velk::IViewEntry& view,
              ::velk::IRenderTarget::Ptr input,
              ::velk::IRenderTarget::Ptr output,
              ::velk::FrameContext& ctx,
              ::velk::IRenderGraph& graph) override;

private:
    /// Compiles the tonemap compute pipeline if not yet compiled.
    /// Returns the pipeline cache key (zero on failure).
    uint64_t ensure_pipeline(::velk::FrameContext& ctx);

    bool compiled_ = false;
};

} // namespace velk::impl

#endif // VELK_RENDER_TONEMAP_H
