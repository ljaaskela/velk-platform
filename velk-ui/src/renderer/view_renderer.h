#ifndef VELK_UI_VIEW_RENDERER_H
#define VELK_UI_VIEW_RENDERER_H

#include <velk/vector.h>

#include <unordered_map>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/render_types.h>
#include <velk-ui/interface/intf_element.h>
#include <velk-ui/interface/intf_scene.h>

#include "batch_builder.h"
#include "frame_data_manager.h"
#include "gpu_resource_manager.h"

namespace velk::ui {

class FrameSnippetRegistry;

/** @brief Shared non-owning context passed to per-view sub-renderers. */
struct FrameContext
{
    IRenderBackend* backend = nullptr;
    IRenderContext* render_ctx = nullptr;
    FrameDataManager* frame_buffer = nullptr;
    GpuResourceManager* resources = nullptr;
    BatchBuilder* batch_builder = nullptr; ///< Shared visual-command cache.
    FrameSnippetRegistry* snippets = nullptr; ///< Shared material / shadow-tech / intersect snippet registry.
    const std::unordered_map<uint64_t, PipelineId>* pipeline_map = nullptr;
    IGpuResourceObserver* observer = nullptr; ///< Renderer as observer of GPU resources.
    uint64_t present_counter = 0;
    uint64_t latency_frames = 0;

    // Scene-wide BVH built once per frame in build_frame_passes before
    // any view renders; consumed by sub-renderers when they stamp out
    // FrameGlobals. Zero when the view's scene has no BVH (or BVH build
    // failed). See scene_collector::build_scene_bvh.
    uint64_t bvh_nodes_addr = 0;
    uint64_t bvh_shapes_addr = 0;
    uint32_t bvh_root = 0;
    uint32_t bvh_node_count = 0;
    uint32_t bvh_shape_count = 0;
};

/** @brief Bundles a render target owned by a pass. */
struct ViewRenderTarget
{
    IRenderTarget::Ptr target;
};

/** @brief Discriminator for the kind of pass a sub-renderer produces. */
enum class PassKind
{
    Raster,       ///< Classic vkCmdBeginRenderPass + draw calls against a surface or RTT texture.
    ComputeBlit,  ///< Compute dispatch writing to a storage image, then blit to a surface.
    GBufferFill,  ///< Raster draws into a multi-attachment G-buffer group (no surface blit).
    Compute,      ///< Pure compute dispatch, no blit. Result consumed by a later pass via sampled image / storage.
    Blit,         ///< Pure blit from a source texture to a surface rect. Used for debug overlays.
};

/**
 * @brief A unit of GPU work produced by a sub-renderer and submitted by
 *        Renderer in a single command buffer alongside the other passes.
 *
 * The structure is a tagged union (see PassKind). Unused fields for the
 * non-active kind are zero-initialised.
 */
struct RenderPass
{
    PassKind kind = PassKind::Raster;

    // Raster fields
    ViewRenderTarget target;
    rect viewport;
    vector<DrawCall> draw_calls;

    // GBufferFill fields (PassKind::GBufferFill). When kind ==
    // GBufferFill, `gbuffer_group` is the target instead of `target`,
    // and `draw_calls` + `viewport` above apply as usual.
    RenderTargetGroup gbuffer_group = 0;

    // ComputeBlit fields
    DispatchCall compute{};
    uint64_t blit_surface_id = 0;
    TextureId blit_source = 0;
    rect blit_dst_rect{};

    // Optional: after the color blit, copy this group's depth attachment
    // into blit_surface_id's depth buffer. Zero = no depth blit. Used by
    // the deferred compositor so forward draws on the surface can
    // depth-test against the deferred scene.
    RenderTargetGroup blit_depth_source_group = 0;
};

/**
 * @brief Renderer-owned per-view state.
 *
 * Visible to all sub-renderers. Each sub-renderer reads/writes only the
 * fields relevant to its path (raster fields for the rasterizer, rt_* for
 * the ray tracer); they do not conflict in practice. On view removal, each
 * sub-renderer releases its own per-view allocations via on_view_removed.
 */
struct ViewEntry
{
    IElement::Ptr camera_element;
    IWindowSurface::Ptr surface;
    rect viewport;

    // Raster-scoped
    bool batches_dirty = true;
    int cached_width = 0;
    int cached_height = 0;
    vector<BatchBuilder::Batch> batches;

    // Ray-traced output texture (RT-scoped; lives in the bindless storage
    // image array). Reallocated on viewport size change.
    TextureId rt_output_tex = 0;
    int rt_width = 0;
    int rt_height = 0;

    // Deferred G-buffer (raster-scoped). Allocated once per view at the
    // current viewport size, reallocated on resize. Attachments follow
    // the canonical layout in velk-render/gbuffer.h. 0 = not yet created.
    RenderTargetGroup gbuffer_group = 0;
    int gbuffer_width = 0;
    int gbuffer_height = 0;

    // Deferred lighting output (DeferredLighter-scoped). Storage image
    // the compute lighting pass writes shaded color into; subsequently
    // blitted to the surface. Reallocated on viewport resize. 0 = unset.
    TextureId deferred_output_tex = 0;
    int deferred_width = 0;
    int deferred_height = 0;

    // Transient: GPU address of this view's FrameGlobals block for the
    // current frame. Written by the Rasterizer's raster-pass build and
    // read by DeferredLighter so its compute shader can reach scene-wide
    // state (inv_vp, BVH) via the canonical GlobalData layout.
    uint64_t frame_globals_addr = 0;
};

/**
 * @brief Interface for a per-view renderer.
 *
 * An IViewRenderer knows how to turn a single view (camera + surface +
 * viewport + scene state) into zero or more RenderPasses. The outer Renderer
 * instantiates one concrete implementation per supported path (rasterizer,
 * ray tracer) and dispatches each view to the right one based on the
 * camera's `render_path` property.
 *
 * TODO(plugin-view-renderers): today this is an in-DLL-only split. For a
 * plugin to provide its own render path, the following would have to move:
 *   - Promote this interface to a public `IViewRenderer` under
 *     velk-ui/include with VELK_CLASS_UID + `Interface<>` CRTP.
 *   - Move `GpuResourceManager` and `FrameDataManager` to velk-render as
 *     stable public interfaces (they are generic GPU plumbing that also
 *     doesn't really belong in velk-ui), and replace the concrete pointers
 *     in FrameContext with those interfaces.
 *   - Decide whether `BatchBuilder` stays private (rasterizer-specific) or
 *     gets a narrow public slice (the gpu_resources tracking used by
 *     consume_scenes).
 *   - Replace the hard-wired rasterizer_/ray_tracer_ members on Renderer
 *     with a registry keyed by the camera's render_path value.
 * Deferred until a plugin actually wants a custom path; the shape of that
 * request will dictate the right public types.
 */
class IViewRenderer
{
public:
    virtual ~IViewRenderer() = default;

    /**
     * @brief Appends zero or more passes for @p view to @p out_passes.
     *
     * Called once per frame per view. The implementation may maintain
     * per-view state (caches, storage textures) keyed off the ViewEntry.
     */
    virtual void build_passes(ViewEntry& view,
                              const SceneState& scene_state,
                              FrameContext& ctx,
                              vector<RenderPass>& out_passes) = 0;

    /**
     * @brief Emits any extra passes that are not per-view (e.g. raster
     *        render-to-texture passes that serve multiple views).
     *
     * Called once per frame after all build_passes calls have completed, so
     * implementations can observe the full per-frame shape/batch state before
     * emitting. Default no-op.
     */
    virtual void build_shared_passes(FrameContext& /*ctx*/,
                                     vector<RenderPass>& /*out_passes*/) {}

    /** @brief Hook called when a view is removed. Release per-view state. */
    virtual void on_view_removed(ViewEntry& /*view*/, FrameContext& /*ctx*/) {}

    /**
     * @brief Hook called when an element is detached from a scene.
     *
     * Fires once per detached element during the per-frame consume pass,
     * before any build_passes call. Implementations release any per-element
     * state they cache (e.g. the rasterizer's render-target texture for
     * elements with a RenderCache trait). Default no-op.
     */
    virtual void on_element_removed(IElement* /*elem*/, FrameContext& /*ctx*/) {}

    /** @brief Hook called on Renderer shutdown. Release all remaining state. */
    virtual void shutdown(FrameContext& /*ctx*/) {}
};

} // namespace velk::ui

#endif // VELK_UI_VIEW_RENDERER_H
