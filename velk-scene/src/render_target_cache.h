#ifndef VELK_UI_RENDER_TARGET_CACHE_H
#define VELK_UI_RENDER_TARGET_CACHE_H

#include <velk-render/interface/intf_render_graph.h>
#include <velk-render/interface/intf_render_path.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_view_entry.h>

#include <unordered_map>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/render_types.h>
#include <velk-scene/interface/intf_element.h>

namespace velk { class BatchBuilder; }

namespace velk {

class IElement;

/**
 * @brief Backing store for render-to-texture (RTT) subtrees.
 *
 * Lifetime sits on Renderer (one cache per renderer). Both raster paths
 * (Forward, Deferred G-buffer) call ensure() before building their main
 * draw passes so any TextureVisual sampling from an RTT sees a non-zero
 * texture id. ForwardPath's build_shared_passes drains emit_passes() to
 * actually render the RTT subtrees.
 */
class RenderTargetCache
{
public:
    /**
     * @brief Allocates / resizes the backend texture for every RTT
     *        subtree currently in the BatchBuilder's render_target_passes.
     *
     * Idempotent across frames. Must run before the per-view main
     * pass's build_draw_calls so the RTT's render_target_id is set on
     * its IRenderTarget object.
     */
    void ensure(FrameContext& ctx, BatchBuilder& batch_builder);

    /**
     * @brief Emits one Raster pass per RTT subtree into @p graph.
     *
     * Called from Renderer once per frame after every view has finished
     * its build_passes. The emitted passes target the RTT textures, so
     * they must run before any surface pass that samples them. Today
     * RTT consumers read via bindless textures (graph-invisible), so
     * Renderer continues to call `emit_passes` *before* per-view
     * pipeline emit to maintain producer-before-consumer ordering.
     */
    void emit_passes(FrameContext& ctx, BatchBuilder& batch_builder,
                     IRenderGraph& graph);

    /** @brief Releases the cached entry for @p elem, if any. */
    void on_element_removed(IElement* elem, FrameContext& ctx);

    /** @brief Destroys all cached textures during renderer shutdown. */
    void shutdown(FrameContext& ctx);

private:
    struct Entry
    {
        /// Weak reference to the user-supplied RenderTexture. Strong
        /// references live on the trait state's `render_target` ObjectRef
        /// (and any consumer-side bindings). When the wrapper's last
        /// reference drops, the manager observer chain auto-defers the
        /// backend handle for destroy — the cache holds no ownership.
        IRenderTarget::WeakPtr target;
        int width = 0;
        int height = 0;
        PixelFormat format = PixelFormat::Surface;
        bool dirty = true;
    };

    std::unordered_map<IElement*, Entry> entries_;

    /// Per-RTT scratch IViewEntry. Keyed by element pointer so paths
    /// that key per-view state by `IViewEntry*` keep their state stable
    /// across frames for the same RTT element. Lazy-created on first
    /// emit per element; hive-pooled.
    std::unordered_map<IElement*, IViewEntry::Ptr> view_entries_;

    /// Lazy-instantiated ForwardPath used to render RTT subtrees.
    /// Holding the path here keeps every line of forward composition
    /// inside ForwardPath itself; RenderTargetCache only emits batches
    /// + globals and lets the path produce passes.
    IRenderPath::Ptr forward_path_;
};

} // namespace velk

#endif // VELK_UI_RENDER_TARGET_CACHE_H
