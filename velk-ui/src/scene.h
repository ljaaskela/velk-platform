#ifndef VELK_UI_SCENE_H
#define VELK_UI_SCENE_H

#include "layout_solver.h"

#include <velk/api/event.h>
#include <velk/api/hierarchy.h>
#include <velk/ext/object.h>
#include <velk/vector.h>

#include <velk-ui/interface/intf_element.h>
#include <velk-ui/interface/intf_scene.h>
#include <velk-ui/interface/intf_scene_observer.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

class Scene : public ::velk::ext::Object<Scene, IScene>
{
public:
    VELK_CLASS_UID(ClassId::Scene, "Scene");

    Scene();
    ~Scene() override;

    // IScene
    IFuture::Ptr load_from(string_view path) override;
    void load(IStore& store) override;
    void set_geometry(aabb geometry) override;
    void update(const UpdateInfo& info) override;
    SceneState consume_state() override;

    void notify_dirty(IElement& element, DirtyFlags flags) override;
    array_view<IElement*> get_visual_list() override;

    // IHierarchy forwarding
    ReturnValue set_root(const IObject::Ptr& root) override;
    ReturnValue add(const IObject::Ptr& parent, const IObject::Ptr& child) override;
    ReturnValue insert(const IObject::Ptr& parent, size_t index,
                             const IObject::Ptr& child) override;
    ReturnValue remove(const IObject::Ptr& object) override;
    ReturnValue replace(const IObject::Ptr& old_child,
                              const IObject::Ptr& new_child) override;
    void clear() override;
    IObject::Ptr root() const override;
    IObject::Ptr parent_of(const IObject::Ptr& object) const override;
    vector<IObject::Ptr> children_of(const IObject::Ptr& object) const override;
    IObject::Ptr child_at(const IObject::Ptr& object, size_t index) const override;
    size_t child_count(const IObject::Ptr& object) const override;
    void for_each_child(const IObject::Ptr& object, void* context,
                        ChildVisitorFn visitor) const override;
    bool contains(const IObject::Ptr& object) const override;
    size_t size() const override;
    Node node_of(const IObject::Ptr& object) const override;

    static vector<Scene*>& live_scenes();

private:
    void ensure_hierarchy();
    void attach_element(const IObject::Ptr& obj);
    void detach_element(const IObject::Ptr& obj);
    void detach_subtree(const IObject::Ptr& obj);
    void replicate_children(IHierarchy& src, const IObject::Ptr& parent);
    void rebuild_visual_list();
    void collect_visual_list(const IObject::Ptr& obj);

    Hierarchy logical_;
    LayoutSolver solver_;
    aabb geometry_{};

    void set_dirty(DirtyFlags flags) { dirty_ |= flags; }

    vector<IElement*> dirty_elements_;
    vector<IElement*> visual_list_;
    vector<IElement*> redraw_list_;
    vector<IObject::Ptr> removed_list_;

    // Double-buffered for consume_state(): returned views stay valid until next consume.
    vector<IElement*> consumed_redraw_;
    vector<IObject::Ptr> consumed_removed_;
    DirtyFlags dirty_ = DirtyFlags::All;
    bool initialized_ = false;
};

} // namespace velk::ui

#endif // VELK_UI_SCENE_H
