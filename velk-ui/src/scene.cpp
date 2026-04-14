#include "scene.h"

#include <velk/api/future.h>
#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_object_storage.h>
#include <velk/interface/intf_store.h>

#include <velk-ui/interface/intf_input_trait.h>
#include <velk/interface/resource/intf_resource.h>
#include <velk/interface/resource/intf_resource_store.h>
#include <velk/plugins/importer/api/importer.h>

#include <algorithm>

#ifdef VELK_LAYOUT_DEBUG
#define LAYOUT_LOG(...) VELK_LOG(I, __VA_ARGS__)
#else
#define LAYOUT_LOG(...) ((void)0)
#endif

namespace velk::ui {

namespace {

IHierarchy* get_hierarchy(Hierarchy& h)
{
    return interface_cast<IHierarchy>(h.get());
}

} // namespace

vector<Scene*>& Scene::live_scenes()
{
    static vector<Scene*> scenes;
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

IFuture::Ptr Scene::load_from(string_view path)
{
    VELK_PERF_SCOPE("scene.load_from");
    auto promise = make_promise();

    auto file = instance().resource_store().get_resource<IFile>(path);
    if (!file) {
        VELK_LOG(E, "Scene::load_from: failed to resolve resource '%.*s'",
                 static_cast<int>(path.size()), path.data());
        promise.set_value(ReturnValue::Fail);
        return promise.get_future<ReturnValue>();
    }

    string json;
    if (failed(file->read_text(json))) {
        VELK_LOG(E, "Scene::load_from: failed to read '%.*s'",
                 static_cast<int>(path.size()), path.data());
        promise.set_value(ReturnValue::Fail);
        return promise.get_future<ReturnValue>();
    }

    auto importer = create_json_importer();
    auto result = importer.import_from(json);

    for (auto& err : result.errors) {
        VELK_LOG(E, "Scene::load_from: import error: %s", err.c_str());
    }

    if (!result.store) {
        promise.set_value(ReturnValue::Fail);
        return promise.get_future<ReturnValue>();
    }

    load(*result.store);

    promise.set_value(ReturnValue::Success);
    return promise.get_future<ReturnValue>();
}

void Scene::load(IStore& store)
{
    ensure_hierarchy();

    // Find the first hierarchy in the store (convention: "hierarchy:<name>")
    static const string_view hierarchy_keys[] = {"hierarchy:scene", "hierarchy:main", "hierarchy:root"};
    IObject::Ptr hierarchy_obj;
    for (auto& key : hierarchy_keys) {
        hierarchy_obj = store.find(key);
        if (hierarchy_obj) {
            break;
        }
    }

    if (!hierarchy_obj) {
        return;
    }

    auto* src = interface_cast<IHierarchy>(hierarchy_obj);
    if (!src) {
        return;
    }

    auto src_root = src->root();
    if (!src_root) {
        return;
    }

    // Replicate imported hierarchy into our scene
    set_root(src_root);
    replicate_children(*src, src_root);
}

void Scene::set_geometry(aabb geometry)
{
    if (geometry_ != geometry) {
        geometry_ = geometry;
        std::unique_lock lock(state_mutex_);
        set_dirty(DirtyFlags::Layout);
        LAYOUT_LOG("Scene::set_geometry: %.0fx%.0f", geometry.extent.width, geometry.extent.height);
    }
}

void Scene::update(const UpdateInfo& info)
{
    VELK_PERF_EVENT(Update);
    VELK_PERF_SCOPE("scene.update");
    auto* h = get_hierarchy(logical_);
    if (!h) {
        return;
    }

    std::unique_lock lock(state_mutex_);

    // Merge per-element dirty flags into scene-level flags
    for (auto* elem : dirty_elements_) {
        auto f = elem->consume_dirty();
        dirty_ |= f;
        if (f != DirtyFlags::None) {
            redraw_list_.push_back(elem);
        }
    }
    dirty_elements_.clear();

    if ((dirty_ & DirtyFlags::DrawOrder) != DirtyFlags::None) {
        rebuild_visual_list();
    }

    if ((dirty_ & DirtyFlags::Layout) != DirtyFlags::None) {
        solver_.solve(*h, geometry_);
        // Layout changed, all elements need redraw
        redraw_list_.clear();
        for (auto& entry : visual_list_) {
            if (entry.type == VisualEntry::Element) {
                redraw_list_.push_back(entry.element.get());
            }
        }
    }

    dirty_ = DirtyFlags::None;
}

SceneState Scene::consume_state()
{
    std::unique_lock lock(state_mutex_);

    SceneState state;
    state.visual_list = visual_list_;
    state.after_visual_list = after_visual_list_;
    state.redraw_list = std::move(redraw_list_);
    state.removed_list = std::move(removed_list_);
    return state;
}

void Scene::notify_dirty(IElement& element, DirtyFlags)
{
    std::unique_lock lock(state_mutex_);
    dirty_elements_.push_back(&element);
}

vector<IElement::Ptr> Scene::ray_cast(vec3 origin, vec3 /*direction*/, size_t max_count) const
{
    std::shared_lock lock(state_mutex_);

    vector<IElement::Ptr> hits;

    // Walk in reverse z-order (topmost first)
    for (size_t i = visual_list_.size(); i > 0; --i) {
        auto& entry = visual_list_[i - 1];
        if (entry.type != VisualEntry::Element) {
            continue;
        }
        auto& elem = entry.element;

        // Only elements with an input trait participate in hit testing
        auto* storage = interface_cast<IObjectStorage>(elem);
        if (!storage) {
            continue;
        }
        bool has_trait = false;
        for (size_t j = 0; j < storage->attachment_count(); ++j) {
            if (interface_cast<IInputTrait>(storage->get_attachment(j))) {
                has_trait = true;
                break;
            }
        }
        if (!has_trait) {
            continue;
        }

        // 2D AABB test against the ray origin (x, y)
        auto reader = read_state<IElement>(elem);
        if (!reader) {
            continue;
        }
        float wx = reader->world_matrix.m[12];
        float wy = reader->world_matrix.m[13];
        float w = reader->size.width;
        float h = reader->size.height;

        if (origin.x >= wx && origin.x < wx + w &&
            origin.y >= wy && origin.y < wy + h) {
            hits.push_back(elem);
            if (max_count > 0 && hits.size() >= max_count) {
                break;
            }
        }
    }

    return hits;
}

vector<IElement::Ptr> Scene::find_elements(const ElementQuery& query, size_t max_count) const
{
    auto root = logical_.root();
    if (!root) {
        return {};
    }

    // Walk the hierarchy depth-first, pre-order.
    vector<IElement::Ptr> matches;
    vector<IObject::Ptr> stack;
    stack.push_back(root);

    while (!stack.empty()) {
        auto obj = std::move(stack.back());
        stack.pop_back();

        if (auto elem = interface_pointer_cast<IElement>(obj)) {
            // Check that every requested trait is present.
            bool match = true;
            if (!query.traits.empty()) {
                auto* storage = interface_cast<IObjectStorage>(elem);
                if (!storage) {
                    match = false;
                } else {
                    for (auto trait_uid : query.traits) {
                        if (!storage->find_attachment(AttachmentQuery{trait_uid})) {
                            match = false;
                            break;
                        }
                    }
                }
            }
            if (match) {
                matches.push_back(elem);
                if (max_count > 0 && matches.size() >= max_count) {
                    return matches;
                }
            }
        }

        // Push children in reverse so we visit them in order.
        if (auto children = logical_.children_of(obj); !children.empty()) {
            stack.reserve(stack.size() + children.size());
            for (size_t i = children.size(); i > 0; --i) {
                stack.push_back(children[i - 1]);
            }
        }
    }

    return matches;
}

void Scene::ensure_hierarchy()
{
    if (!initialized_) {
        logical_ = create_hierarchy();
        initialized_ = true;
    }
}

void Scene::attach_element(const IObject::Ptr& obj)
{
    if (auto* observer = interface_cast<ISceneObserver>(obj)) {
        observer->on_attached(*this);
    }
    std::unique_lock lock(state_mutex_);
    set_dirty(DirtyFlags::DrawOrder);
}

void Scene::detach_element(const IObject::Ptr& obj)
{
    if (auto* observer = interface_cast<ISceneObserver>(obj)) {
        observer->on_detached(*this);
    }
    std::unique_lock lock(state_mutex_);
    removed_list_.push_back(interface_pointer_cast<IElement>(obj));
    set_dirty(DirtyFlags::DrawOrder);
}

void Scene::detach_subtree(const IObject::Ptr& obj)
{
    auto* h = get_hierarchy(logical_);
    if (!h) {
        return;
    }

    vector<IObject::Ptr> stack;
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

void Scene::replicate_children(IHierarchy& src, const IObject::Ptr& parent)
{
    auto kids = src.children_of(parent);
    for (auto& kid : kids) {
        add(parent, kid);
        replicate_children(src, kid);
    }
}

// IHierarchy forwarding

ReturnValue Scene::set_root(const IObject::Ptr& root)
{
    ensure_hierarchy();

    // Detach old root and its subtree
    auto old_root = logical_.root();
    if (old_root) {
        detach_subtree(old_root.object());
    }

    auto rv = logical_.set_root(root);
    if (rv == ReturnValue::Success && root) {
        attach_element(root);
    }
    return rv;
}

ReturnValue Scene::add(const IObject::Ptr& parent, const IObject::Ptr& child)
{
    ensure_hierarchy();

    auto rv = logical_.add(parent, child);
    if (rv == ReturnValue::Success) {
        attach_element(child);
    }
    return rv;
}

ReturnValue Scene::insert(const IObject::Ptr& parent, size_t index,
                                const IObject::Ptr& child)
{
    ensure_hierarchy();

    auto rv = logical_.insert(parent, index, child);
    if (rv == ReturnValue::Success) {
        attach_element(child);
    }
    return rv;
}

ReturnValue Scene::remove(const IObject::Ptr& object)
{
    auto rv = logical_.remove(object);
    if (rv == ReturnValue::Success) {
        detach_subtree(object);
    }
    return rv;
}

ReturnValue Scene::replace(const IObject::Ptr& old_child, const IObject::Ptr& new_child)
{
    auto rv = logical_.replace(old_child, new_child);
    if (rv == ReturnValue::Success) {
        detach_element(old_child);
        attach_element(new_child);
    }
    return rv;
}

void Scene::clear()
{
    if (auto r = root()) {
        detach_subtree(r);
    }
    logical_.clear();
}

IObject::Ptr Scene::root() const
{
    return logical_.root();
}

IObject::Ptr Scene::parent_of(const IObject::Ptr& object) const
{
    return logical_.parent_of(object);
}

vector<IObject::Ptr> Scene::children_of(const IObject::Ptr& object) const
{
    auto h = logical_.operator IHierarchy::Ptr();
    return h ? h->children_of(object) : vector<IObject::Ptr>{};
}

IObject::Ptr Scene::child_at(const IObject::Ptr& object, size_t index) const
{
    return logical_.child_at(object, index);
}

size_t Scene::child_count(const IObject::Ptr& object) const
{
    return logical_.child_count(object);
}

void Scene::for_each_child(const IObject::Ptr& object, void* context, ChildVisitorFn visitor) const
{
    auto h = logical_.operator IHierarchy::Ptr();
    if (h) {
        h->for_each_child(object, context, visitor);
    }
}

bool Scene::contains(const IObject::Ptr& object) const
{
    return logical_.contains(object);
}

size_t Scene::size() const
{
    return logical_.size();
}

IHierarchy::Node Scene::node_of(const IObject::Ptr& object) const
{
    return logical_.node_of(object).hierarchy_node();
}

void Scene::rebuild_visual_list()
{
    VELK_PERF_SCOPE("scene.rebuild_visual_list");
    visual_list_.clear();
    after_visual_list_.clear();
    if (auto r = root()) {
        collect_visual_list(r);
    }
}

void Scene::collect_visual_list(const IObject::Ptr& obj)
{
    auto elem = interface_pointer_cast<IElement>(obj);
    auto elem_state = read_state<IElement>(elem);
    bool cached = elem_state && elem_state->cache_mode == CacheMode::Cached;

    if (cached) {
        visual_list_.push_back({VisualEntry::PushCache, elem});
    }
    if (elem) {
        visual_list_.push_back({VisualEntry::Element, elem});
    }

    auto* h = get_hierarchy(logical_);
    if (!h) {
        return;
    }

    auto kids = h->children_of(obj);
    std::sort(kids.begin(), kids.end(), [](const IObject::Ptr& a, const IObject::Ptr& b) {
        auto ra = read_state<IElement>(a);
        auto rb = read_state<IElement>(b);
        int32_t za = ra ? ra->z_index : 0;
        int32_t zb = rb ? rb->z_index : 0;
        return za < zb;
    });

    for (auto& kid : kids) {
        collect_visual_list(kid);
    }

    if (elem) {
        after_visual_list_.push_back({VisualEntry::Element, elem});
    }
    if (cached) {
        after_visual_list_.push_back({VisualEntry::PopCache, elem});
    }
}

} // namespace velk::ui
