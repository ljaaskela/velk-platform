#ifndef VELK_RENDER_TONEMAP_H
#define VELK_RENDER_TONEMAP_H

#include <unordered_map>

#include <velk-render/ext/effect.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_tonemap.h>
#include <velk-render/interface/intf_view_entry.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief ACES filmic tonemap effect.
 *
 * Reads `input` as a bindless texture, writes the tonemapped result to
 * `output` as a storage image. One compute dispatch per view, sized to
 * the input dimensions.
 *
 * Compiles its compute pipeline lazily on first emit (identical across
 * views, shared via the render context's pipeline cache). Each view caches
 * its recorded pass and re-records only when the exposure, input/output
 * texture, or size changes — otherwise the same pass is re-added every frame.
 */
class Tonemap final
    : public ::velk::ext::Effect<Tonemap, ::velk::ITonemap>
{
public:
    VELK_CLASS_UID(::velk::ClassId::Effect::Tonemap, "Tonemap");

    void emit(::velk::IViewEntry& view,
              ::velk::IRenderTarget::Ptr input,
              ::velk::IRenderTarget::Ptr output,
              ::velk::FrameContext& ctx,
              ::velk::IRenderGraph& graph) override;

    void on_view_removed(::velk::IViewEntry& view, ::velk::FrameContext& ctx) override;
    void shutdown(::velk::FrameContext& ctx) override;

private:
    /// Resolves the tonemap compute pipeline, compiling on a (weak) cache
    /// miss. Returns a strong Ptr the caller keeps alive (the tonemap pass
    /// holds it); nullptr on failure.
    ::velk::IGpuPipeline::Ptr ensure_pipeline(::velk::FrameContext& ctx);

    /// Cached pass + the snapshot of what its command buffer baked, per view.
    struct ViewState
    {
        ::velk::IRenderPass::Ptr pass;
        bool snapshot_valid = false;
        float exposure = 0.f;
        uint32_t input_id = 0, output_id = 0;
        ::velk::uvec2 dims{};
    };

    std::unordered_map<::velk::IViewEntry*, ViewState> view_states_;
};

} // namespace velk::impl

#endif // VELK_RENDER_TONEMAP_H
