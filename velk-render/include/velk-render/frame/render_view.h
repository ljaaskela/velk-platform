#ifndef VELK_RENDER_FRAME_RENDER_VIEW_H
#define VELK_RENDER_FRAME_RENDER_VIEW_H

#include <velk/api/math_types.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_batch.h>
#include <velk-render/frustum.h>
#include <velk-render/gpu_data.h>
#include <velk-render/render_types.h>

#include <cstdint>

namespace velk {

/**
 * @brief Resolved environment for a view (sky / IBL / fallback miss).
 *
 * Filled by the scene-side preparer from a camera's `IEnvironment`
 * attachment. Zero/null when the camera has no environment.
 */
struct ViewEnv
{
    uint32_t texture_id = 0;      ///< Bindless equirect HDR id (0 = no env).
    uint32_t material_id = 0;     ///< Snippet id of the env material (0 = none).
    float    intensity = 0.f;     ///< Env exposure multiplier; pushed inline to the RT / deferred compute.
    float    rotation_rad = 0.f;  ///< Env yaw rotation in radians; pushed inline alongside intensity.
};

/**
 * @brief Per-frame snapshot of resolved scene state for one view.
 *
 * `RenderView` is the path-facing input: everything a render path needs
 * to emit GPU passes for this view, with no further reach-back into
 * `IScene` / `IElement`. The scene-side `ViewPreparer` materializes it
 * from a `IViewEntry` + scene state once per view per frame.
 *
 * Filled eagerly: every applicable field is populated regardless of
 * which path is going to consume it. Per-path declared `needs()` is a
 * future optimization (see Phase 3 design notes); for now collection
 * cost is small enough vs path overhead that always-fill keeps the
 * code simple.
 */
struct RenderView
{
    // Camera (resolved from ICamera + IElement world transform)
    mat4 view_projection = mat4::identity();
    mat4 inverse_view_projection = mat4::identity();
    vec3 cam_pos = {};
    /// Optional view-space frustum for batch-level culling. Empty when
    /// no camera is attached (ortho fallback, no culling).
    ::velk::render::Frustum frustum{};
    bool has_frustum = false;

    /// Viewport rect in pixels (origin + extent within the view's surface).
    rect viewport{};
    /// Width / height of the viewport in pixels (== viewport.width/height
    /// rounded; cached separately for shader push-constant convenience).
    int width = 0;
    int height = 0;

    /// Element base of this view's FrameGlobals within the shared globals
    /// arena (set = 1 slot 2). Pushed to the direct-push compute shaders
    /// (deferred lighting / denoise / spatial) so they read
    /// velk_globals.data[view_globals_base] instead of a device address.
    /// Derived as region.offset / sizeof(FrameGlobals) at prepare time.
    uint32_t view_globals_base = 0;

    /// How this view reaches the scene BVH (empty when no BVH).
    BvhBinding bvh{};

    /// Camera environment, if any.
    ViewEnv env{};

    /// Forward-only env batch (fullscreen quad with env material) when
    /// the camera has an environment. Forward path prepends one
    /// DrawCall built from this before its main batches; deferred and
    /// RT use the env via their compute shaders, not as a draw, and
    /// ignore this. Null when the camera has no environment.
    IBatch::Ptr env_batch;

    /// Scene lights for this view. `flags[1]` carries the registered
    /// shadow-tech id (or 0 for no shadow); both RT and deferred paths
    /// compose `velk_eval_shadow` from the snippet registry to match.
    vector<GpuLight> lights;

    /// Element base of this view's light array within the shared light
    /// arena (set = 1 slot 5). The RT and deferred compute shaders read
    /// velk_lights.data[lights_base + i]. Persistent region, so the base is
    /// stable across frames (cached passes bake it); derived as
    /// region.offset / sizeof(GpuLight) at prepare time. 0 when no lights.
    uint32_t lights_base = 0;

    /// Raster batch cache for this view. Owned by the preparer; the
    /// span here is valid for the duration of the path's
    /// `build_passes` call and invalidated by the next view's prepare.
    /// Forward and Deferred paths consume; RT does not.
    const vector<IBatch::Ptr>* batches = nullptr;

    /// RT primary-buffer shapes for this view. Each entry has its
    /// `material_id` / `material_base` / `texture_id` / shape_kind
    /// fields pre-resolved through the frame snippet registry.
    /// Mesh-kind shapes have `mesh_instance_base` set to their record in
    /// the shared mesh-instance arena. Order is enumeration-order (no plane
    /// sort); RT path back-to-front-sorts a local copy.
    vector<RtShape> shapes;
};

} // namespace velk

#endif // VELK_RENDER_FRAME_RENDER_VIEW_H
