#ifndef VELK_UI_SCENE_H
#define VELK_UI_SCENE_H

#include "layout_solver.h"

#include <velk/api/hierarchy.h>
#include <velk/ext/object.h>
#include <velk/vector.h>

#include <velk-ui/interface/intf_element.h>
#include <velk-ui/interface/intf_renderer.h>
#include <velk-ui/interface/intf_scene.h>
#include <velk-ui/interface/intf_scene_observer.h>

namespace velk_ui {

class Scene : public velk::ext::Object<Scene, IScene>
{
public:
    VELK_CLASS_UID("c9f5e3a4-0b6d-4f8c-ae7f-3d4e5a6b7c8d", "Scene");

    Scene();
    ~Scene() override;

    // IScene
    void load(velk::IStore& store) override;
    void set_renderer(IRenderer* renderer) override;
    void set_viewport(const velk::aabb& viewport) override;
    void update(const velk::UpdateInfo& info) override;

    void notify_dirty(IElement& element, DirtyFlags flags) override;
    velk::array_view<IElement*> get_visual_list() override;

    // IHierarchy forwarding
    velk::ReturnValue set_root(const velk::IObject::Ptr& root) override;
    velk::ReturnValue add(const velk::IObject::Ptr& parent, const velk::IObject::Ptr& child) override;
    velk::ReturnValue insert(const velk::IObject::Ptr& parent, size_t index,
                             const velk::IObject::Ptr& child) override;
    velk::ReturnValue remove(const velk::IObject::Ptr& object) override;
    velk::ReturnValue replace(const velk::IObject::Ptr& old_child,
                              const velk::IObject::Ptr& new_child) override;
    void clear() override;
    velk::IObject::Ptr root() const override;
    velk::IObject::Ptr parent_of(const velk::IObject::Ptr& object) const override;
    velk::vector<velk::IObject::Ptr> children_of(const velk::IObject::Ptr& object) const override;
    velk::IObject::Ptr child_at(const velk::IObject::Ptr& object, size_t index) const override;
    size_t child_count(const velk::IObject::Ptr& object) const override;
    void for_each_child(const velk::IObject::Ptr& object, void* context,
                        ChildVisitorFn visitor) const override;
    bool contains(const velk::IObject::Ptr& object) const override;
    size_t size() const override;
    Node node_of(const velk::IObject::Ptr& object) const override;

    static velk::vector<Scene*>& live_scenes();

private:
    void ensure_hierarchy();
    void attach_element(const velk::IObject::Ptr& obj);
    void detach_element(const velk::IObject::Ptr& obj);
    void detach_subtree(const velk::IObject::Ptr& obj);
    void register_visual(IElement* elem);
    void replicate_children(velk::IHierarchy& src, const velk::IObject::Ptr& parent);
    void rebuild_visual_list();
    void collect_visual_list(const velk::IObject::Ptr& obj);

    velk::Hierarchy logical_;
    LayoutSolver solver_;

    IRenderer* renderer_ = nullptr;
    velk::aabb viewport_{};

    velk::vector<IElement*> dirty_elements_;
    velk::vector<IElement*> visual_list_;
    bool visual_list_dirty_ = true;
    bool initialized_ = false;
};

} // namespace velk_ui

#endif // VELK_UI_SCENE_H
