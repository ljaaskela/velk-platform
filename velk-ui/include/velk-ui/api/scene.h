#ifndef VELK_UI_API_SCENE_H
#define VELK_UI_API_SCENE_H

#include <velk/api/future.h>
#include <velk/api/hierarchy.h>

#include <velk-ui/api/element.h>
#include <velk-ui/interface/intf_scene.h>
#include <velk-ui/plugin.h>

namespace velk_ui {

/**
 * @brief Convenience wrapper around IScene.
 *
 * Provides null-safe access to scene operations. All methods
 * return safe defaults when the underlying object is null.
 *
 *   auto scene = create_scene("app://scenes/my_scene.json");
 *   scene.set_renderer(renderer);
 *   scene.set_viewport(viewport);
 */
class Scene : public velk::Hierarchy
{
public:
    /** @brief Default-constructed Scene wraps no object. */
    Scene() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement IScene. */
    explicit Scene(velk::IObject::Ptr obj) : velk::Hierarchy(check_object<IScene>(obj)) {}

    /** @brief Wraps an existing IScene pointer. */
    explicit Scene(IScene::Ptr scene) : velk::Hierarchy(interface_pointer_cast<velk::IObject>(scene)) {}

    /** @brief Implicit conversion to IScene::Ptr. */
    operator IScene::Ptr() const { return as_ptr<IScene>(); }

    /** @brief Implicit conversion to IHierarchy::Ptr. */
    operator velk::IHierarchy::Ptr() const { return as_ptr<velk::IHierarchy>(); }

    /**
     * @brief Loads a scene from a resource URI (e.g. "app://scenes/my_scene.json").
     * @param path Resource URI to load from.
     * @return A future that resolves with the load result.
     */
    velk::Future<velk::ReturnValue> load_from(velk::string_view path)
    {
        return velk::Future<velk::ReturnValue>(with<IScene>([&](auto& s) { return s.load_from(path); }));
    }

    /** @brief Sets the renderer that will receive visual updates. May be null. */
    void set_renderer(const IRenderer::Ptr& renderer)
    {
        with<IScene>([&](auto& s) { s.set_renderer(renderer); });
    }

    /** @brief Processes one frame: runs layout, collects changes, pushes to renderer. */
    void update(const velk::UpdateInfo& info)
    {
        with<IScene>([&](auto& s) { s.update(info); });
    }

    /** @brief Returns the root element, or empty. */
    Element root() const { return Element(velk::Hierarchy::root()); }

    /** @brief Returns the Element wrapping the given object, or empty. */
    Element node_of(const velk::IObject::Ptr& object) const
    {
        return Element(velk::Hierarchy::node_of(object));
    }

    /** @brief Returns the parent element, or empty if root or not found. */
    Element parent_of(const velk::IObject::Ptr& object) const
    {
        return Element(velk::Hierarchy::parent_of(object));
    }

    /** @brief Returns the child element at the given index, or empty. */
    Element child_at(const velk::IObject::Ptr& object, size_t index) const
    {
        return Element(velk::Hierarchy::child_at(object, index));
    }
};

/**
 * @brief Creates a new scene, optionally loading from a resource URI.
 * @param path Resource URI (e.g. "app://scenes/my_scene.json"). Empty for an empty scene.
 */
inline Scene create_scene(velk::string_view path = {})
{
    auto obj = velk::instance().create<velk::IObject>(ClassId::Scene);
    Scene s(std::move(obj));
    if (s && !path.empty()) {
        s.load_from(path).get_result();
    }
    return s;
}

} // namespace velk_ui

#endif // VELK_UI_API_SCENE_H
