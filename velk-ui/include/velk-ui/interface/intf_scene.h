#ifndef VELK_UI_INTF_SCENE_H
#define VELK_UI_INTF_SCENE_H

#include <velk/api/math_types.h>
#include <velk/array_view.h>
#include <velk/interface/intf_future.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk/interface/intf_store.h>

#include <velk-ui/interface/intf_element.h>
#include <velk-ui/types.h>

namespace velk::ui {

/**
 * @brief Holds element and visual change information for one frame.
 *
 * Returned by IScene::consume_state(). visual_list is the full z-sorted
 * draw order. redraw_list contains elements whose visuals changed since
 * the last consume. removed_list contains elements that were detached;
 * the scene keeps them alive until consumed, then releases them.
 */
struct SceneState
{
    array_view<IElement*> visual_list;
    array_view<IElement*> redraw_list;
    array_view<IObject::Ptr> removed_list;
};

/**
 * @brief Owns the element hierarchy, runs layout, and tracks visual changes.
 *
 * Presents an IHierarchy interface externally. Internally owns a logical hierarchy
 * (parent/child ownership) and a z-sorted visual list for draw order. Elements
 * notify the scene of property changes via notify_dirty; the scene batches these
 * and processes them in update().
 *
 * The renderer pulls changes via consume_state() during render().
 */
class IScene : public Interface<IScene, IHierarchy>
{
public:
    /** @brief Loads a scene from a resource URI (e.g. "app://scenes/my_scene.json"). */
    virtual IFuture::Ptr load_from(string_view path) = 0;

    /** @brief Imports elements from a store and replicates them into the scene hierarchy. */
    virtual void load(IStore& store) = 0;

    /** @brief Sets the layout bounds for this scene. */
    virtual void set_geometry(aabb geometry) = 0;

    /**
     * @brief Processes one frame: runs layout solver, collects visual changes,
     *        and rebuilds the visual list if draw order changed.
     */
    virtual void update(const UpdateInfo& info) = 0;

    /**
     * @brief Returns the current scene state and clears change tracking.
     *
     * On the first call (or after draw order changes), redraw_list contains
     * all elements. After that, it contains only elements that changed since
     * the previous consume.
     */
    virtual SceneState consume_state() = 0;

    /** @brief Called by elements when a property changes. Accumulates dirty flags. */
    virtual void notify_dirty(IElement& element, DirtyFlags flags) = 0;

    /** @brief Returns all elements in z-sorted draw order. Valid until the next update(). */
    virtual array_view<IElement*> get_visual_list() = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_SCENE_H
