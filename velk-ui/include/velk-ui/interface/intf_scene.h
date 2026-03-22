#ifndef VELK_UI_INTF_SCENE_H
#define VELK_UI_INTF_SCENE_H

#include <velk-ui/interface/intf_element.h>
#include <velk-ui/interface/intf_renderer.h>
#include <velk-ui/types.h>
#include <velk/array_view.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk/interface/intf_store.h>

namespace velk_ui {

/**
 * @brief Owns the element hierarchy, runs layout, and pushes changes to the renderer.
 *
 * Presents an IHierarchy interface externally. Internally owns a logical hierarchy
 * (parent/child ownership) and a z-sorted visual list for draw order. Elements
 * notify the scene of property changes via notify_dirty; the scene batches these
 * and processes them in update().
 */
class IScene : public velk::Interface<IScene, velk::IHierarchy>
{
public:
    /** @brief Imports elements from a store and replicates them into the scene hierarchy. */
    virtual void load(velk::IStore& store) = 0;

    /** @brief Sets the renderer that will receive visual updates. May be null. */
    virtual void set_renderer(IRenderer* renderer) = 0;

    /** @brief Sets the root-level available space for layout. */
    virtual void set_viewport(const velk::aabb& viewport) = 0;

    /**
     * @brief Processes one frame: runs layout solver, collects visual changes,
     *        rebuilds the visual list if needed, and pushes changes to the renderer.
     */
    virtual void update(const velk::UpdateInfo &info) = 0;

    /** @brief Called by elements when a property changes. Accumulates dirty flags. */
    virtual void notify_dirty(IElement& element, DirtyFlags flags) = 0;

    /** @brief Returns all elements in z-sorted draw order. Valid until the next update(). */
    virtual velk::array_view<IElement*> get_visual_list() = 0;
};

} // namespace velk_ui

#endif // VELK_UI_INTF_SCENE_H
