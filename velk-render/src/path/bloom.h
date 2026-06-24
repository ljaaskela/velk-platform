#ifndef VELK_RENDER_BLOOM_H
#define VELK_RENDER_BLOOM_H

#include <velk/vector.h>

#include <unordered_map>

#include <velk-render/ext/effect.h>
#include <velk-render/interface/intf_bloom.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Progressive mip-chain bloom effect.
 *
 * Reads the HDR `input`, prefilters it through a soft-knee threshold while
 * downsampling into a chain of half-resolution mips, blurs back up the chain
 * with an additive tent-filter upsample, then writes `input + intensity *
 * glow` to `output` (still HDR; a downstream tonemap maps it to display
 * range). Bloom must run before tonemap, so it always writes to a
 * container-supplied intermediate, never the surface.
 *
 * Per-view state: the mip-chain scratch textures, keyed off `IViewEntry*` so
 * one effect Ptr can serve several cameras. Recreated only on size change;
 * released in `on_view_removed` / `shutdown`.
 */
class Bloom final
    : public ::velk::ext::Effect<Bloom, ::velk::IBloom>
{
public:
    VELK_CLASS_UID(::velk::ClassId::Effect::Bloom, "Bloom");

    void emit(::velk::IViewEntry& view,
              ::velk::IRenderTarget::Ptr input,
              ::velk::IRenderTarget::Ptr output,
              ::velk::FrameContext& ctx,
              ::velk::IRenderGraph& graph) override;

    void on_view_removed(::velk::IViewEntry& view, ::velk::FrameContext& ctx) override;
    void shutdown(::velk::FrameContext& ctx) override;

private:
    /// Scratch mip chain for one view. `down[i]` is the downsampled +
    /// prefiltered HDR at 1/2^(i+1) resolution; `up[i]` accumulates the
    /// additive upsample at the same resolution as `down[i]` (top of the
    /// chain reuses `down.back()`, so `up` has one fewer entry).
    ///
    /// The recorded passes are cached across frames and re-added to the graph
    /// each frame; they are re-recorded only when the `snapshot` below changes
    /// (resize, day/night param toggle, or input/output retarget), all rare.
    struct ViewState
    {
        ::velk::vector<::velk::IRenderTarget::Ptr> down;
        ::velk::vector<::velk::IRenderTarget::Ptr> up;
        ::velk::uvec2 size{};

        ::velk::vector<::velk::IRenderPass::Ptr> passes;

        /// Everything baked into the recorded command buffers; a mismatch
        /// against the current frame forces a re-record.
        bool snapshot_valid = false;
        ::velk::uvec2 dims{};
        float threshold = 0.f, knee = 0.f, intensity = 0.f, radius = 0.f;
        uint32_t input_id = 0, output_id = 0;
        bool passthrough = false;
    };

    std::unordered_map<::velk::IViewEntry*, ViewState> view_states_;

    /// (Re)allocates the mip chain for @p view at @p width x @p height.
    /// Returns the view's state, or nullptr if allocation failed.
    ViewState* ensure_chain(::velk::IViewEntry& view, int width, int height,
                            ::velk::IRenderGraph& graph);

    ::velk::IGpuPipeline::Ptr ensure_downsample_pipeline(::velk::FrameContext& ctx);
    ::velk::IGpuPipeline::Ptr ensure_upsample_pipeline(::velk::FrameContext& ctx);
    ::velk::IGpuPipeline::Ptr ensure_combine_pipeline(::velk::FrameContext& ctx);
};

} // namespace velk::impl

#endif // VELK_RENDER_BLOOM_H
