#ifndef VELK_SCENE_RENDER_PATH_FRAME_CONTEXT_H
#define VELK_SCENE_RENDER_PATH_FRAME_CONTEXT_H

#include <unordered_map>

#include <velk/interface/intf_interface.h>

#include <velk-render/frame/draw_call_emit.h>
#include <velk-render/interface/intf_frame_data_manager.h>
#include <velk-render/interface/intf_frame_snippet_registry.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/render_types.h>

namespace velk {

/**
 * @brief Shared non-owning context passed to per-view render paths.
 *
 * Bundles every dependency a path needs to upload data and emit GPU
 * passes. All fields are velk-render-side so paths can be moved to
 * velk-render without losing access. velk-scene-internal helpers
 * (BatchBuilder, RenderTargetCache) are scene-only concerns and the
 * Renderer drives them directly outside the path's view.
 */
struct FrameContext
{
    IRenderBackend* backend = nullptr;
    IRenderContext* render_ctx = nullptr;
    IFrameDataManager* frame_buffer = nullptr;
    IGpuResourceManager* resources = nullptr;
    IFrameSnippetRegistry* snippets = nullptr;
    const PipelineCacheMap* pipeline_map = nullptr;

    /// Color attachment format the active path is writing into.
    /// Pipeline lookups in `pipeline_map` reconstruct their cache key
    /// using this format; raster pipelines must be compiled against a
    /// render pass with this format. `Surface` means "follow the
    /// swapchain", which is the default for direct-to-swapchain paths
    /// and surface-format RTT (element cache).
    /// TODO: today there is one target_format per scene; supporting
    /// concurrent HDR + LDR cameras in the same Renderer pass needs
    /// per-view material variants.
    PixelFormat target_format = PixelFormat::Surface;

    /// GPU completion marker tagging the in-flight frame's submit.
    /// Use as the `completion_marker` argument when deferring resource
    /// destroys; once the marker resolves, this frame's GPU work has
    /// finished and the resource is safe to destroy. Stamped from
    /// `IRenderBackend::pending_frame_completion_marker()` at the
    /// start of each `build_frame_passes`.
    uint64_t defer_marker = 0;

    /// Monotonic CPU-side frame counter, incremented per present.
    /// Use only as a frame index (e.g. RNG seed in RT shaders), never
    /// as a GPU-completion proxy — that's what `defer_marker` is for.
    uint64_t present_counter = 0;

    /// Camera trait of the view currently being dispatched. Set by the
    /// Renderer before each `IViewPipeline::emit` so pipelines can
    /// resolve attached stages (e.g. IRenderPath, future IPostProcess)
    /// without a back-pointer to the trait. Null between dispatches.
    IInterface* view_camera_trait = nullptr;

    // Scene-wide BVH built once per frame in build_frame_passes before
    // any view renders; consumed by paths when they stamp out
    // FrameGlobals. Zero when the view's scene has no BVH.
    uint64_t bvh_nodes_addr = 0;
    uint64_t bvh_shapes_addr = 0;
    uint32_t bvh_root = 0;
    uint32_t bvh_node_count = 0;
    uint32_t bvh_shape_count = 0;

    /// Convenience: assemble a FrameResolveContext for snippet-registry calls.
    FrameResolveContext make_resolve_context() const
    {
        return {render_ctx, resources, frame_buffer, defer_marker};
    }
};

} // namespace velk

#endif // VELK_SCENE_RENDER_PATH_FRAME_CONTEXT_H
