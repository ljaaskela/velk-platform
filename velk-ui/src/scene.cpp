#include "scene.h"

#include <velk-ui/interface/intf_renderer.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_store.h>

#include <algorithm>

namespace velk_ui {

namespace {

velk::IHierarchy* get_hierarchy(velk::Hierarchy& h)
{
    return velk::interface_cast<velk::IHierarchy>(h.get());
}

} // namespace

velk::vector<Scene *> &Scene::live_scenes()
{
    static velk::vector<Scene *> scenes;
    return scenes;
}

Scene::Scene()
{
    live_scenes().push_back(this);
}

Scene::~Scene()
{
    auto& scenes = live_scenes();
    scenes.erase(std::remove(scenes.begin(), scenes.end(), this), scenes.end());
}

void Scene::load(velk::IStore& store)
{
    ensure_hierarchy();

    // Find the first hierarchy in the store (convention: "hierarchy:<name>")
    // Try common names
    static const velk::string_view hierarchy_keys[] = {"hierarchy:scene", "hierarchy:main", "hierarchy:root"};
    velk::IObject::Ptr hierarchy_obj;
    for (auto& key : hierarchy_keys) {
        hierarchy_obj = store.find(key);
        if (hierarchy_obj) break;
    }

    if (!hierarchy_obj) return;

    auto* src = velk::interface_cast<velk::IHierarchy>(hierarchy_obj);
    if (!src) return;

    auto src_root = src->root();
    if (!src_root) return;

    // Replicate imported hierarchy into our scene
    set_root(src_root);
    replicate_children(*src, src_root);

    // Register all elements with renderer if one is set
    if (renderer_) {
        rebuild_visual_list();
        for (auto* elem : visual_list_) {
            register_visual(elem);
        }
    }
}

void Scene::set_renderer(IRenderer* renderer)
{
    renderer_ = renderer;
}

void Scene::set_viewport(const velk::aabb& viewport)
{
    viewport_ = viewport;
}

void Scene::update(const velk::UpdateInfo &info)
{
    auto* h = get_hierarchy(logical_);
    if (!h) return;

    // Clear changes from previous frame
    velk::vector<IElement *> changes;

    // Process dirty elements, check if layout is needed
    DirtyFlags flags = DirtyFlags::None;
    for (auto* elem : dirty_elements_) {
        flags |= elem->consume_dirty();
        if ((flags & DirtyFlags::Visual) != DirtyFlags::None) {
            changes.push_back(elem);
        }
    }

    dirty_elements_.clear();

    if ((flags & DirtyFlags::Layout) != DirtyFlags::None) {
        // Layout dirty
        solver_.solve(*h, viewport_);
    }
    if ((flags & DirtyFlags::ZOrder) != DirtyFlags::None) {
        // Draw order dirty
        visual_list_dirty_ = true;
    }
    if (visual_list_dirty_) {
        rebuild_visual_list();
    }
    // Push changes to renderer
    if (renderer_ && !changes.empty()) {
        renderer_->update_visuals(changes);
    }
}

void Scene::notify_dirty(IElement& element, DirtyFlags)
{
    dirty_elements_.push_back(&element);
}

velk::array_view<IElement*> Scene::get_visual_list()
{
    return {visual_list_.data(), visual_list_.size()};
}

void Scene::ensure_hierarchy()
{
    if (!initialized_) {
        logical_ = velk::create_hierarchy();
        initialized_ = true;
    }
}

void Scene::attach_element(const velk::IObject::Ptr& obj)
{
    auto* observer = velk::interface_cast<ISceneObserver>(obj);
    if (observer) {
        observer->on_attached(*this);
    }
    visual_list_dirty_ = true;
}

void Scene::detach_element(const velk::IObject::Ptr& obj)
{
    auto* observer = velk::interface_cast<ISceneObserver>(obj);
    if (observer) {
        observer->on_detached(*this);
    }
    visual_list_dirty_ = true;
}

void Scene::detach_subtree(const velk::IObject::Ptr& obj)
{
    auto* h = get_hierarchy(logical_);
    if (!h) return;

    velk::vector<velk::IObject::Ptr> stack;
    stack.push_back(obj);
    while (!stack.empty()) {
        auto node = stack.back();
        stack.pop_back();
        auto kids = h->children_of(node);
        for (auto& kid : kids) {
            stack.push_back(kid);
        }
        detach_element(node);
    }
}

void Scene::register_visual(IElement* elem)
{
    if (!renderer_ || !elem) return;
    renderer_->add_visual(velk::get_self<IElement>(elem));
}

void Scene::replicate_children(velk::IHierarchy& src, const velk::IObject::Ptr& parent)
{
    auto kids = src.children_of(parent);
    for (auto& kid : kids) {
        add(parent, kid);
        replicate_children(src, kid);
    }
}

// IHierarchy forwarding

velk::ReturnValue Scene::set_root(const velk::IObject::Ptr& root)
{
    ensure_hierarchy();

    // Detach old root and its subtree
    auto old_root = logical_.root();
    if (old_root) {
        detach_subtree(old_root.object());
    }

    auto rv = logical_.set_root(root);
    if (rv == velk::ReturnValue::Success && root) {
        attach_element(root);
    }
    return rv;
}

velk::ReturnValue Scene::add(const velk::IObject::Ptr& parent, const velk::IObject::Ptr& child)
{
    ensure_hierarchy();

    auto rv = logical_.add(parent, child);
    if (rv == velk::ReturnValue::Success) {
        attach_element(child);
    }
    return rv;
}

velk::ReturnValue Scene::insert(const velk::IObject::Ptr& parent, size_t index,
                                 const velk::IObject::Ptr& child)
{
    ensure_hierarchy();

    auto rv = logical_.insert(parent, index, child);
    if (rv == velk::ReturnValue::Success) {
        attach_element(child);
    }
    return rv;
}

velk::ReturnValue Scene::remove(const velk::IObject::Ptr& object)
{
    auto rv = logical_.remove(object);
    if (rv == velk::ReturnValue::Success) {
        detach_subtree(object);
    }
    return rv;
}

velk::ReturnValue Scene::replace(const velk::IObject::Ptr& old_child,
                                  const velk::IObject::Ptr& new_child)
{
    auto rv = logical_.replace(old_child, new_child);
    if (rv == velk::ReturnValue::Success) {
        detach_element(old_child);
        attach_element(new_child);
    }
    return rv;
}

void Scene::clear()
{
    auto r = root();
    if (r) {
        detach_subtree(r);
    }
    logical_.clear();
}

velk::IObject::Ptr Scene::root() const
{
    return logical_.root();
}

velk::IObject::Ptr Scene::parent_of(const velk::IObject::Ptr& object) const
{
    return logical_.parent_of(object);
}

velk::vector<velk::IObject::Ptr> Scene::children_of(const velk::IObject::Ptr& object) const
{
    auto h = logical_.operator velk::IHierarchy::Ptr();
    return h ? h->children_of(object) : velk::vector<velk::IObject::Ptr>{};
}

velk::IObject::Ptr Scene::child_at(const velk::IObject::Ptr& object, size_t index) const
{
    return logical_.child_at(object, index);
}

size_t Scene::child_count(const velk::IObject::Ptr& object) const
{
    return logical_.child_count(object);
}

void Scene::for_each_child(const velk::IObject::Ptr& object, void* context,
                            ChildVisitorFn visitor) const
{
    auto h = logical_.operator velk::IHierarchy::Ptr();
    if (h) {
        h->for_each_child(object, context, visitor);
    }
}

bool Scene::contains(const velk::IObject::Ptr& object) const
{
    return logical_.contains(object);
}

size_t Scene::size() const
{
    return logical_.size();
}

velk::IHierarchy::Node Scene::node_of(const velk::IObject::Ptr& object) const
{
    return logical_.node_of(object).hierarchy_node();
}

void Scene::rebuild_visual_list()
{
    visual_list_.clear();
    visual_list_dirty_ = false;
    if (auto r = root()) {
        collect_visual_list(r);
    }
}

void Scene::collect_visual_list(const velk::IObject::Ptr& obj)
{
    auto* element = velk::interface_cast<IElement>(obj);
    if (element) {
        visual_list_.push_back(element);
    }

    auto* h = get_hierarchy(logical_);
    if (!h) return;

    auto kids = h->children_of(obj);
    std::sort(kids.begin(), kids.end(), [](const velk::IObject::Ptr& a, const velk::IObject::Ptr& b) {
        auto ra = velk::read_state<IElement>(a);
        auto rb = velk::read_state<IElement>(b);
        int32_t za = ra ? ra->z_index : 0;
        int32_t zb = rb ? rb->z_index : 0;
        return za < zb;
    });

    for (auto& kid : kids) {
        collect_visual_list(kid);
    }
}

} // namespace velk_ui
