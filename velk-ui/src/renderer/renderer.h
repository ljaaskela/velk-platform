#ifndef VELK_UI_RENDERER_IMPL_H
#define VELK_UI_RENDERER_IMPL_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <unordered_map>
#include "batch_builder.h"
#include "frame_data_manager.h"
#include "gpu_resource_manager.h"
#include "deferred_lighter.h"
#include "ray_tracer.h"
#include "rasterizer.h"
#include "view_renderer.h"
#include <velk-render/detail/intf_renderer_internal.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/plugin.h>
#include <velk-render/render_types.h>
#include <velk-render/interface/intf_camera.h>
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
    void add_debug_overlay(const IWindowSurface::Ptr& surface,
                           TextureId texture_id,
                           const rect& dst_rect) override;
    void clear_debug_overlays() override;
    TextureId get_gbuffer_attachment(const IElement::Ptr& camera_element,
                                     const IWindowSurface::Ptr& surface,
                                     uint32_t attachment_index) const override;
    void shutdown() override;

private:
    struct FrameSlot
    {
        uint64_t id = 0;
        vector<RenderPass> passes;
        bool ready = false;
        uint64_t presented_at = 0;
        FrameDataManager::Slot buffer;
    };

    FrameSlot* claim_frame_slot();
    std::unordered_map<IScene*, SceneState> consume_scenes(const FrameDesc& desc);
    void build_frame_passes(const FrameDesc& desc,
                            std::unordered_map<IScene*, SceneState>& consumed_scenes,
                            FrameSlot& slot);
    bool view_matches(const ViewEntry& entry, const FrameDesc& desc) const;
    FrameContext make_frame_context();

    IRenderBackend::Ptr backend_;
    IRenderContext* render_ctx_ = nullptr;

    // resources_ must outlive any member that holds IProgram::Ptr refs
    // (views_, batch_builder_): material dtors invoke on_gpu_resource_destroyed
    // which calls into resources_.
    GpuResourceManager resources_;

    vector<ViewEntry> views_;
    const std::unordered_map<uint64_t, PipelineId>* pipeline_map_ = nullptr;

    struct DebugOverlay {
        IWindowSurface::Ptr surface;
        TextureId texture_id = 0;
        rect dst_rect{};
    };
    vector<DebugOverlay> debug_overlays_;

    // Shared visual-command / gpu-resource cache. Both the dirty-resource
    // upload in consume_scenes and the Rasterizer path read from it, so it
    // lives here rather than inside a single sub-renderer.
    BatchBuilder batch_builder_;
    FrameDataManager frame_buffer_;

    FrameSlot* active_slot_ = nullptr;

    // Sub-renderers; one per render path.
    Rasterizer rasterizer_;
    RayTracer ray_tracer_;
    DeferredLighter deferred_lighter_;

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
