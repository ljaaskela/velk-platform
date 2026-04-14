#ifndef VELK_UI_INTF_SCENE_H
#define VELK_UI_INTF_SCENE_H

#include <velk/api/math_types.h>
#include <velk/array_view.h>
#include <velk/interface/intf_future.h>
#include <velk/vector.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk/interface/intf_store.h>

#include <velk-ui/interface/intf_element.h>
#include <velk-ui/types.h>

namespace velk::ui {

/**
 * @brief Filter for IScene::find_elements.
 *
 * An element matches the query if its IObjectStorage holds at least one
 * attachment implementing each interface in @c traits. Empty @c traits matches
 * all elements in the scene.
 */
struct ElementQuery
{
    vector<Uid> traits; ///< Interface UIDs the element's attachments must implement (AND).
};

/** @brief A single entry in the visual list stream. */
struct VisualListEntry
{
    VisualEntry type{VisualEntry::Element};
    IElement::Ptr element;
};

/**
 * @brief Holds element and visual change information for one frame.
 *
 * Returned by IScene::consume_state(). Owns strong references to all
 * elements, ensuring they stay alive for the duration of the frame
 * regardless of scene mutations.
 */
struct SceneState
{
    /** @brief Pre-order (depth-first) visual list for BeforeChildren visuals. */
    vector<VisualListEntry> visual_list;
    /** @brief Post-order visual list for AfterChildren visuals. */
    vector<VisualListEntry> after_visual_list;
    /** @brief Elements whose visual state has changed since the last update. Elements on this list are
     *         guaranteed to be either on visual_list or removed_list. */
    vector<IElement*> redraw_list;
    /** @brief Elements that were detached from the scene since last update. */
    vector<IElement::Ptr> removed_list;
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
     * @brief Returns a snapshot of the current scene state and clears change tracking.
     *
     * On the first call (or after draw order changes), redraw_list contains
     * all elements. After that, it contains only elements that changed since
     * the previous consume.
     */
    virtual SceneState consume_state() = 0;

    /** @brief Called by elements when a property changes. Accumulates dirty flags. */
    virtual void notify_dirty(IElement& element, DirtyFlags flags) = 0;

    /**
     * @brief Returns elements hit by a ray, in hit order (front to back).
     *
     * Only elements with an IInputTrait attachment participate in hit testing.
     * Coordinates are in world space.
     *
     * @param origin    Ray origin in world space.
     * @param direction Ray direction in world space.
     * @param max_count Maximum number of hits to return (0 = unlimited).
     */
    virtual vector<IElement::Ptr> ray_cast(vec3 origin, vec3 direction,
                                           size_t max_count = 0) const = 0;

    /**
     * @brief Returns elements matching @p query, in pre-order tree traversal.
     *
     * Walks the scene hierarchy depth-first and returns elements whose attachments
     * satisfy every interface UID in @c query.traits. An empty trait list matches
     * all elements.
     *
     * @param query     Filter describing required trait interfaces.
     * @param max_count Maximum number of matches to return (0 = unlimited).
     */
    virtual vector<IElement::Ptr> find_elements(const ElementQuery& query,
                                                size_t max_count = 0) const = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_SCENE_H
