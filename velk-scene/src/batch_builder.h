#ifndef VELK_UI_BATCH_BUILDER_H
#define VELK_UI_BATCH_BUILDER_H

#include <velk/vector.h>

#include <unordered_map>
#include <unordered_set>
#include <velk-render/interface/intf_batch.h>
#include <velk-render/frame/draw_call_emit.h>
#include <velk-render/interface/intf_frame_data_manager.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/frustum.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_shader_source.h>
#include <velk-render/interface/intf_texture_resolver.h>
#include <velk-render/render_types.h>
#include <velk-scene/interface/intf_element.h>
#include <velk-scene/interface/intf_scene.h>

namespace velk {

/**
 * @brief Builds draw batches from the visual list and converts them to GPU draw calls.
 *
 * Manages the element visual cache, pre-filters render target subtrees,
 * and assembles draw calls into the frame data buffer.
 */
class BatchBuilder
{
public:
    struct VisualCommands
    {
        vector<DrawEntry> entries;
        // The visual's GLSL source contributor (vertex / fragment /
        // discard role), copied onto each emitted Batch so render
        // paths can splice the bits they want without re-querying the
        // visual. Null for visuals that don't supply their own raster
        // pipeline (rendering goes through the entry's material).
        IShaderSource::Ptr shader_source;
        // Materials are per-entry (see DrawEntry::material). Pipeline
        // compilation runs lazily in build_draw_calls; the result is
        // stashed on each entry's pipeline_override.
    };

    struct ElementCache
    {
        vector<VisualCommands> before_visuals;
        vector<VisualCommands> after_visuals;
        vector<IBuffer::WeakPtr> gpu_resources;
    };


    struct RenderTargetPassData
    {
        IElement* element = nullptr;
        vector<IElement*> before_entries;  // pre-order
        vector<IElement*> after_entries;   // post-order
        vector<IBatch::Ptr> batches;
    };

    /// Per-element record of every batch slot that element contributes
    /// to. Populated during `rebuild_batches`; consumed by the
    /// transform-only fast path which writes new world matrices directly
    /// into existing slots without rewalking the scene.
    struct ElementSlot
    {
        IBatch* batch = nullptr;       ///< Non-owning; batch lifetime tied to caller-owned vector<IBatch::Ptr>.
        uint32_t instance_index = 0;   ///< Slot index within the batch.
        float offset_x = 0.f;          ///< RTT subtree offset baked in at emit time.
        float offset_y = 0.f;
    };
    using ElementSlotMap = std::unordered_map<IElement*, vector<ElementSlot>>;

    /**
     * @brief Rebuilds visual commands for a single element from its traits.
     *
     * Captures shader sources, pipeline options, and material textures
     * onto the cached visual / draw entries. Pipeline compilation is
     * deferred to `IRenderContext::build_draw_calls`, which compiles
     * lazily against the active path's `FrameContext::target_format`.
     */
    void rebuild_commands(IElement* element, IRenderContext* render_ctx);

    /** @brief Rebuilds batches from the visual list, pre-filtering render
     *         target subtrees. Each batch is a fresh `IBatch::Ptr` with
     *         its own `[args(32)][count(16)][instance_data]` blob ready
     *         for the renderer's standard buffer-upload pipeline. Also
     *         populates @p out_slots with per-element batch-slot records
     *         (used by the transform-only fast path) and @p out_rtt_roots
     *         with elements that introduce an RTT subtree. */
    void rebuild_batches(const SceneState& state,
                         vector<IBatch::Ptr>& out_batches,
                         ElementSlotMap& out_slots,
                         std::unordered_set<IElement*>& out_rtt_roots);

    /** @brief Removes an element from the cache. */
    void evict(IElement* element) { element_cache_.erase(element); }

    /** @brief Clears all cached data. */
    void clear() { element_cache_.clear(); render_target_passes_.clear(); }

    /**
     * @brief Clears the render-target-pass union so each view's
     *        `rebuild_batches` this frame accumulates into a fresh
     *        list. Material upload dedup cache lives on Renderer now.
     */
    void reset_frame_state()
    {
        render_target_passes_.clear();
    }

    /** @brief Returns the element cache (for resource upload iteration). */
    const std::unordered_map<IElement*, ElementCache>& element_cache() const { return element_cache_; }

    /** @brief Returns the render target passes collected during rebuild_batches. */
    const vector<RenderTargetPassData>& render_target_passes() const { return render_target_passes_; }

    /** @brief Mutable access to render target passes (for moving batches in prepare). */
    vector<RenderTargetPassData>& render_target_passes() { return render_target_passes_; }

private:
    std::unordered_map<IElement*, ElementCache> element_cache_;
    vector<RenderTargetPassData> render_target_passes_;
};

} // namespace velk

#endif // VELK_UI_BATCH_BUILDER_H
