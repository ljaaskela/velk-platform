#ifndef VELK_UI_RENDERER_IMPL_H
#define VELK_UI_RENDERER_IMPL_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <unordered_map>
#include "batch_builder.h"
#include "frame_data_manager.h"
#include "gpu_resource_manager.h"
#include <velk-render/detail/intf_renderer_internal.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/plugin.h>
#include <velk-render/render_types.h>
#include <velk-ui/interface/intf_camera.h>
#include <velk-ui/interface/intf_render_to_texture.h>
#include <velk-ui/interface/intf_renderer.h>
#include <velk-ui/interface/intf_scene.h>

#include <condition_variable>
#include <mutex>

namespace velk::ui {

class Renderer : public ::velk::ext::Object<Renderer, IRendererInternal, IRenderer, IGpuResourceObserver>
{
public:
    VELK_CLASS_UID(::velk::ClassId::Renderer, "Renderer");

    // IRendererInternal
    void set_backend(const IRenderBackend::Ptr& backend, IRenderContext* ctx) override;

    // IGpuResourceObserver
    void on_gpu_resource_destroyed(IGpuResource* resource) override;

    // IRenderer
    void add_view(const IElement::Ptr& camera_element, const IWindowSurface::Ptr& surface,
                  const rect& viewport) override;
    void remove_view(const IElement::Ptr& camera_element, const IWindowSurface::Ptr& surface) override;
    Frame prepare(const FrameDesc& desc) override;
    void present(Frame frame) override;
    void render() override;
    void set_max_frames_in_flight(uint32_t count) override;
    void shutdown() override;

private:
    struct ViewEntry
    {
        IElement::Ptr camera_element;
        IWindowSurface::Ptr surface;
        rect viewport;
        bool batches_dirty = true;
        int cached_width = 0;
        int cached_height = 0;
        vector<BatchBuilder::Batch> batches;
    };

    struct RenderTarget
    {
        IRenderTarget::Ptr target;
    };

    struct RenderPass
    {
        RenderTarget target;
        rect viewport;
        vector<DrawCall> draw_calls;
    };

    struct FrameSlot
    {
        uint64_t id = 0;
        vector<RenderPass> passes;
        bool ready = false;
        uint64_t presented_at = 0;
        FrameDataManager::Slot buffer;
    };

    struct RenderTargetEntry
    {
        IRenderTarget::Ptr target;
        TextureId texture_id = 0;
        int width = 0;
        int height = 0;
        bool dirty = true;
    };

    FrameSlot* claim_frame_slot();
    std::unordered_map<IScene*, SceneState> consume_scenes(const FrameDesc& desc);
    void build_frame_passes(const FrameDesc& desc,
                            std::unordered_map<IScene*, SceneState>& consumed_scenes,
                            FrameSlot& slot);
    void prepend_environment_batch(ICamera& camera, ViewEntry& entry);
    bool view_matches(const ViewEntry& entry, const FrameDesc& desc) const;

    IRenderBackend::Ptr backend_;
    IRenderContext* render_ctx_ = nullptr;
    vector<ViewEntry> views_;
    const std::unordered_map<uint64_t, PipelineId>* pipeline_map_ = nullptr;

    BatchBuilder batch_builder_;
    FrameDataManager frame_buffer_;
    GpuResourceManager resources_;

    FrameSlot* active_slot_ = nullptr;
    uint64_t globals_gpu_addr_ = 0;
    vector<DrawCall> draw_calls_;

    std::unordered_map<IElement*, RenderTargetEntry> render_target_entries_;

    static constexpr uint64_t kGpuLatencyFrames = 3;
    static constexpr uint32_t kDefaultMaxFramesInFlight = kGpuLatencyFrames + 1;
    vector<FrameSlot> frame_slots_{kDefaultMaxFramesInFlight};
    uint64_t next_frame_id_ = 1;
    uint64_t present_counter_ = 0;
    std::mutex slot_mutex_;
    std::condition_variable slot_cv_;
};

} // namespace velk::ui

#endif // VELK_UI_RENDERER_IMPL_H
