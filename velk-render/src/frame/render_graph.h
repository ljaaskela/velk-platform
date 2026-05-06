#ifndef VELK_RENDER_RENDER_GRAPH_H
#define VELK_RENDER_RENDER_GRAPH_H

#include <unordered_map>

#include <velk-render/detail/intf_render_graph_internal.h>
#include <velk-render/ext/render_state.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_graph.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Tier 1 RenderGraph implementation.
 *
 * Insertion-ordered. Compile assigns a pre-pass barrier per pass using
 * a coarse per-resource state machine: any prior pass that wrote a
 * tracked resource forces a barrier before the next consumer pass.
 * Execute walks the op list of every pass and dispatches each op 1:1
 * to a backend method via std::visit.
 */
class RenderGraph final
    : public ::velk::ext::RenderState<RenderGraph, ::velk::IRenderGraphInternal>
{
public:
    VELK_CLASS_UID(::velk::ClassId::RenderGraph, "RenderGraph");

    // IRenderGraph
    bool empty() const override { return passes_.empty(); }
    void clear() override;
    void import(const ::velk::IGpuResource::Ptr& resource) override;
    void add_pass(::velk::IRenderPass::Ptr pass) override;
    void compile() override;
    void execute(::velk::IRenderBackend& backend) override;

    ::velk::vector<::velk::IRenderPass::Ptr>& passes() override { return passes_; }
    const ::velk::vector<::velk::IRenderPass::Ptr>& passes() const override { return passes_; }

    ::velk::IGpuResourceManager& resources() override;

    // IRenderGraphInternal
    void init(::velk::IRenderBackend* backend) override;

private:
    /// Per-resource state classifying *what kind* of producer last
    /// touched it. Determines which `src` pipeline stage to wait on
    /// before the next consumer.
    enum class ResourceState : uint8_t
    {
        Undefined,
        ColorWrite,   ///< Written via raster (Submit) or transfer (BlitToSurface dst).
        Storage,      ///< Written via compute (Dispatch).
        ShaderRead,   ///< Already barriered into a sampleable state.
    };

    /// Per-pass logical class derived from its op stream. Drives the
    /// barrier `dst` stage and the write-state new value.
    enum class PassClass : uint8_t
    {
        Raster,    ///< Pass contains a Submit op (BeginPass + Submit + EndPass).
        Compute,   ///< Pass contains a Dispatch but no surface blit (e.g. Tonemap).
        Blit,      ///< Pass writes via BlitToSurface (with or without an upstream Dispatch).
    };

    struct Barrier
    {
        bool emit = false;
        ::velk::PipelineStage src = ::velk::PipelineStage::ColorOutput;
        ::velk::PipelineStage dst = ::velk::PipelineStage::FragmentShader;
    };

    ::velk::vector<::velk::IRenderPass::Ptr> passes_;
    ::velk::vector<Barrier> barriers_;
    std::unordered_map<::velk::IGpuResource*, ResourceState> states_;
    std::unordered_map<::velk::IGpuResource*, bool> imported_;

    /// Per-frame transient resource manager. Distinct from the
    /// renderer's persistent manager: pool / aliasing / load-op
    /// inference (Tier 2 follow-up steps) live here and don't bleed
    /// into the persistent path. Created lazily in `init()`.
    ::velk::IGpuResourceManager::Ptr resources_;

    /// Classifies a pass by inspecting its ops. The order of checks
    /// reflects the post-write resource state we want to record:
    /// passes that end with a blit settle into ColorWrite (the
    /// blit destination's final layout) regardless of any preceding
    /// dispatch.
    static PassClass classify(const ::velk::IRenderPass& pass);
};

} // namespace velk::impl

#endif // VELK_RENDER_RENDER_GRAPH_H
