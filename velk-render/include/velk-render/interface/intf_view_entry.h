#ifndef VELK_RENDER_INTF_VIEW_ENTRY_H
#define VELK_RENDER_INTF_VIEW_ENTRY_H

#include <velk/interface/intf_interface.h>
#include <velk/uid.h>

#include <velk-render/interface/intf_render_state.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/render_types.h>

namespace velk {

/**
 * @brief Per-view identity carried through the renderer (velk-render-pure).
 *
 * Hive-pooled. Carries the view's surface + viewport plus the
 * cross-frame state the renderer mutates (`batches_dirty`, cached
 * size). Pipelines key per-view state off the raw `IViewEntry*` and
 * subscribe through the inherited `IRenderState` surface for change
 * notifications. Scene-aware bits (camera element, scene reference)
 * are kept by the Renderer in a parallel scene-side slot keyed by
 * `IViewEntry*`; paths never need them.
 *
 * Stable address. The hive-pool slot is stable for the view's
 * lifetime, so paths can use `IViewEntry*` as a key in their per-view
 * state maps.
 *
 * Chain: IInterface -> IRenderState -> IViewEntry
 */
class IViewEntry
    : public Interface<IViewEntry, IRenderState,
                       VELK_UID("66ffcbdc-e0c4-4b7b-903f-fca1d2fe8c53")>
{
public:
    /// Surface this view targets.
    virtual IWindowSurface::Ptr surface() const = 0;
    virtual void set_surface(IWindowSurface::Ptr surface) = 0;

    /// Normalized viewport rect within the surface (origin + extent in
    /// [0, 1]).
    virtual rect viewport() const = 0;
    virtual void set_viewport(rect viewport) = 0;

    /// Cross-cutting flag the renderer flips when the scene's visual
    /// set changes. The scene-side preparer reads + clears it before
    /// rebuilding batches.
    virtual bool batches_dirty() const = 0;
    virtual void set_batches_dirty(bool dirty) = 0;

    /// Renderer-side cache of last-seen surface size for resize
    /// detection.
    virtual int cached_width() const = 0;
    virtual int cached_height() const = 0;
    virtual void set_cached_size(int width, int height) = 0;

    /// Renderer-side cache of the last-seen surface native handle, for
    /// detecting a platform window swap (Android suspend/resume) that needs
    /// a full surface recreate rather than just a swapchain resize. Initial
    /// value is the "unsynced" sentinel (UINT64_MAX) so the first prepare
    /// adopts the current handle without triggering a recreate.
    virtual uint64_t cached_native_handle() const = 0;
    virtual void set_cached_native_handle(uint64_t handle) = 0;

    /// Producer-side: fires `IRenderState::on_render_state_changed` on
    /// all subscribers. Called by `ViewPreparer` when anything that
    /// affects the cached pass changes — structural batch rebuild or
    /// camera move (frustum-cull output + FrameGlobals contents).
    virtual void notify_view_changed() = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_VIEW_ENTRY_H
