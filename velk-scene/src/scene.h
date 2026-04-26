#ifndef VELK_SCENE_SCENE_H
#define VELK_SCENE_SCENE_H

#include <velk/api/event.h>
#include <velk/api/hierarchy.h>
#include <velk/ext/object.h>
#include <velk/vector.h>

#include <velk-scene/interface/intf_element.h>
#include <velk-scene/interface/intf_scene.h>
#include <velk-scene/interface/intf_scene_observer.h>
#include <velk-scene/plugin.h>

#include <shared_mutex>

namespace velk::impl {

class Scene : public ::velk::ext::Object<Scene, IScene>
{
public:
    VELK_CLASS_UID(::velk::ClassId::Scene, "Scene");

    Scene();
    ~Scene() override;

    // IScene
    IFuture::Ptr load_from(string_view path) override;
    ReturnValue load(IStore& store, IElement* parent = nullptr) override;
    void set_geometry(aabb geometry) override;
    aabb get_geometry() const override { return geometry_; }
    void update(const UpdateInfo& info) override;
    SceneState consume_state() override;

    void notify_dirty(IElement& element, DirtyFlags flags) override;
    DirtyFlags pending_dirty() const override
    {
        std::shared_lock lock(state_mutex_);
        return dirty_;
    }
    vector<IElement::Ptr> ray_cast(vec3 origin, vec3 direction,
                                   size_t max_count = 0,
                                   const ElementQuery& filter = {}) const override;
    vector<IElement::Ptr> find_elements(const ElementQuery& query,
                                        size_t max_count = 0) const override;
    ::velk::IBvh::Ptr get_default_bvh() const override;

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

private:
    void ensure_hierarchy();
    void attach_element(const IObject::Ptr& obj);
    void detach_element(const IObject::Ptr& obj);
    void detach_subtree(const IObject::Ptr& obj);
    void replicate_children(IHierarchy& src, const IObject::Ptr& parent);

    Hierarchy logical_;
    aabb geometry_{};

    void set_dirty(DirtyFlags flags) { dirty_ |= flags; }

    mutable std::shared_mutex state_mutex_;  ///< Protects the lists below.
    vector<IElement*> dirty_elements_;
    vector<IElement*> redraw_list_;
    vector<IElement::Ptr> removed_list_;

    DirtyFlags dirty_ = DirtyFlags::All;
    bool initialized_ = false;
};

} // namespace velk::impl

#endif // VELK_UI_SCENE_H
