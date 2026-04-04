#ifndef VELK_UI_API_SCENE_H
#define VELK_UI_API_SCENE_H

#include <velk/api/future.h>
#include <velk/api/hierarchy.h>

#include <velk-ui/api/element.h>
#include <velk-ui/interface/intf_scene.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around IScene.
 *
 * Provides null-safe access to scene operations. All methods
 * return safe defaults when the underlying object is null.
 *
 *   auto scene = create_scene("app://scenes/my_scene.json");
 *   scene.set_geometry(aabb::from_size({800, 600}));
 */
class Scene : public Hierarchy
{
public:
    /** @brief Default-constructed Scene wraps no object. */
    Scene() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement IScene. */
    explicit Scene(IObject::Ptr obj) : Hierarchy(check_object<IScene>(obj)) {}

    /** @brief Wraps an existing IScene pointer. */
    explicit Scene(IScene::Ptr scene) : Hierarchy(as_object(scene)) {}

    /** @brief Implicit conversion to IScene::Ptr. */
    operator IScene::Ptr() const { return as_ptr<IScene>(); }

    /** @brief Implicit conversion to IHierarchy::Ptr. */
    operator IHierarchy::Ptr() const { return as_ptr<IHierarchy>(); }

    /**
     * @brief Loads a scene from a resource URI (e.g. "app://scenes/my_scene.json").
     * @param path Resource URI to load from.
     * @return A future that resolves with the load result.
     */
    Future<ReturnValue> load_from(string_view path)
    {
        return Future<ReturnValue>(with<IScene>([&](auto& s) { return s.load_from(path); }));
    }

    /** @brief Sets the layout bounds for this scene. */
    void set_geometry(aabb geometry)
    {
        with<IScene>([&](auto& s) { s.set_geometry(geometry); });
    }

    /** @brief Returns the current scene state and clears change tracking. */
    SceneState consume_state()
    {
        SceneState state;
        with<IScene>([&](auto& s) { state = s.consume_state(); });
        return state;
    }

    /** @brief Processes one frame: runs layout, collects changes. */
    void update(const UpdateInfo& info)
    {
        with<IScene>([&](auto& s) { s.update(info); });
    }

    /** @brief Returns the root element, or empty. */
    Element root() const { return Element(Hierarchy::root()); }

    /** @brief Returns the Element wrapping the given object, or empty. */
    Element node_of(const IObject::Ptr& object) const
    {
        return Element(Hierarchy::node_of(object));
    }

    /** @brief Returns the parent element, or empty if root or not found. */
    Element parent_of(const IObject::Ptr& object) const
    {
        return Element(Hierarchy::parent_of(object));
    }

    /** @brief Returns the child element at the given index, or empty. */
    Element child_at(const IObject::Ptr& object, size_t index) const
    {
        return Element(Hierarchy::child_at(object, index));
    }
};

/**
 * @brief Creates a new scene, optionally loading from a resource URI.
 * @param path Resource URI (e.g. "app://scenes/my_scene.json"). Empty for an empty scene.
 */
inline Scene create_scene(string_view path = {})
{
    auto obj = instance().create<IObject>(ClassId::Scene);
    Scene s(std::move(obj));
    if (s && !path.empty()) {
        s.load_from(path).get_result();
    }
    return s;
}

} // namespace velk::ui

#endif // VELK_UI_API_SCENE_H
