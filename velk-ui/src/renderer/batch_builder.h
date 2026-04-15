#ifndef VELK_UI_BATCH_BUILDER_H
#define VELK_UI_BATCH_BUILDER_H

#include <velk/vector.h>

#include <unordered_map>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_material.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/render_types.h>
#include <velk-ui/interface/intf_element.h>
#include <velk-ui/interface/intf_scene.h>

#include "frame_data_manager.h"
#include "gpu_resource_manager.h"

namespace velk::ui {

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
        uint64_t pipeline_override = 0;
        IMaterial::Ptr material;
    };

    struct ElementCache
    {
        vector<VisualCommands> before_visuals;
        vector<VisualCommands> after_visuals;
        vector<IBuffer::WeakPtr> gpu_resources;
    };

    struct Batch
    {
        uint64_t pipeline_key = 0;
        uint64_t texture_key = 0;
        vector<uint8_t> instance_data;
        uint32_t instance_stride = 0;
        uint32_t instance_count = 0;
        IMaterial::Ptr material;
    };

    struct RenderTargetPassData
    {
        IElement* element = nullptr;
        vector<VisualListEntry> before_entries;
        vector<VisualListEntry> after_entries;
        vector<Batch> batches;
    };

    /** @brief Rebuilds visual commands for a single element from its traits. */
    void rebuild_commands(IElement* element, IGpuResourceObserver* observer, IRenderContext* render_ctx);

    /** @brief Rebuilds batches from the visual list, pre-filtering render target subtrees. */
    void rebuild_batches(const SceneState& state, vector<Batch>& out_batches);

    /** @brief Converts batches to GPU draw calls, writing data to the frame buffer. */
    void build_draw_calls(const vector<Batch>& batches, vector<DrawCall>& out_calls,
                          FrameDataManager& frame_data, GpuResourceManager& resources,
                          uint64_t globals_gpu_addr,
                          const std::unordered_map<uint64_t, PipelineId>* pipeline_map,
                          IRenderContext* render_ctx);

    /** @brief Removes an element from the cache. */
    void evict(IElement* element) { element_cache_.erase(element); }

    /** @brief Clears all cached data. */
    void clear() { element_cache_.clear(); render_target_passes_.clear(); }

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

} // namespace velk::ui

#endif // VELK_UI_BATCH_BUILDER_H
