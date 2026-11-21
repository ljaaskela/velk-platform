#include "editor_core.h"

#include <velk/api/hierarchy.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>

#include <cstdio>
#include <cstring>

namespace velk::editor {

namespace {

string class_name_of(Uid uid)
{
    if (const ClassInfo* info = instance().type_registry().get_class_info(uid)) {
        return string(info->name);
    }
    return string("?");
}

} // namespace

EditorCore::EditorCore()
{
    tree_ = instance().create<IHierarchy>(::velk::ClassId::Hierarchy);
    top_ = instance().create<IObject>(ClassId::EditorNode);
    if (tree_ && top_) {
        tree_->set_root(top_);
    }
    write_state<IEditorCore>(this, [&](IEditorCore::State& s) { set_object_ref(s.tree, tree_); });
}

template <class T, class Step>
void EditorCore::then(Future<T> future, Step step)
{
    IEditorCore::WeakPtr weak = get_self<IEditorCore>();
    const uint32_t generation = generation_;
    future.then([weak, generation, step](const T& value) {
        auto self = weak.lock();
        auto* core = static_cast<EditorCore*>(self.get());
        if (core && core->walk_ && core->walk_->generation == generation) {
            (core->*step)(value);
        }
    });
}

ReturnValue EditorCore::fn_attach(string host, uint32_t port)
{
    fn_detach();
    load_remote_plugin();
    client_ = create_remote_client();
    if (!client_ || failed(client_.connect(host, static_cast<uint16_t>(port)))) {
        client_ = {};
        return ReturnValue::Fail;
    }
    IEditorCore::WeakPtr weak = get_self<IEditorCore>();
    disconnected_ = ScopedHandler(client_.as_ptr<IRemoteClient>()->on_disconnected(), [weak]() {
        if (auto self = weak.lock()) {
            static_cast<EditorCore*>(self.get())->detached();
        }
    });
    char target[96];
    std::snprintf(target, sizeof(target), "%.*s:%u", static_cast<int>(host.size()), host.data(),
                  static_cast<unsigned>(port));
    write_state<IEditorCore>(this, [&](IEditorCore::State& s) {
        s.attached = true;
        s.target = string(string_view(target, std::strlen(target)));
    });
    write_state<IEditorNode>(interface_cast<IEditorNode>(top_), [&](IEditorNode::State& s) {
        s.label = string(string_view(target, std::strlen(target)));
    });
    fn_refresh();
    return ReturnValue::Success;
}

void EditorCore::fn_detach()
{
    if (client_) {
        client_.disconnect();
    }
    disconnected_ = {};
    client_ = {};
    detached();
}

void EditorCore::detached()
{
    walk_.reset();
    nodes_.clear();
    if (tree_ && top_) {
        tree_->clear();
        tree_->set_root(top_);
    }
    write_state<IEditorCore>(this, [&](IEditorCore::State& s) {
        s.attached = false;
        s.target = {};
        ++s.revision;
    });
}

void EditorCore::fn_refresh()
{
    if (!client_ || !client_.is_connected() || walk_) {
        return;
    }
    walk_ = std::make_unique<Walk>();
    walk_->generation = ++generation_;
    then(client_.roots(), &EditorCore::roots_read);
}

void EditorCore::roots_read(const RemoteRoots& roots)
{
    walk_->roots = roots.items;
    vector<RemoteHandle> handles;
    for (auto& r : roots.items) {
        handles.push_back(r.object.handle);
    }
    then(client_.describe(handles), &EditorCore::roots_described);
}

void EditorCore::roots_described(const RemoteDescriptions& described)
{
    for (size_t i = 0; i < walk_->roots.size(); ++i) {
        RemoteHandle top = 0;
        if (i < described.items.size() && described.items[i].is_hierarchy) {
            top = described.items[i].hierarchy_root.handle;
        }
        walk_->hierarchy_roots.push_back(top);
        if (top) {
            walk_->frontier.push_back({walk_->roots[i].object.handle, top});
            walk_->objects.push_back(top);
        }
    }
    next_level();
}

void EditorCore::next_level()
{
    if (walk_->frontier.empty()) {
        then(client_.describe(walk_->objects), &EditorCore::objects_described);
        return;
    }
    then(client_.children(walk_->frontier), &EditorCore::level_read);
}

void EditorCore::level_read(const RemoteChildrenList& level)
{
    vector<RemoteNode> next;
    for (auto& item : level.items) {
        auto& kids = walk_->children[item.node.node];
        for (auto& child : item.children) {
            kids.push_back(child.handle);
            walk_->objects.push_back(child.handle);
            next.push_back({item.node.hierarchy, child.handle});
        }
    }
    walk_->frontier = std::move(next);
    next_level();
}

IObject::Ptr EditorCore::node_for(RemoteHandle object, string_view label)
{
    auto it = nodes_.find(object);
    IObject::Ptr node = it != nodes_.end() ? it->second : instance().create<IObject>(ClassId::EditorNode);
    if (node) {
        write_state<IEditorNode>(interface_cast<IEditorNode>(node), [&](IEditorNode::State& s) {
            s.object = object;
            if (string_view(s.label) != label) {
                s.label = string(label);
            }
        });
        next_nodes_[object] = node;
    }
    return node;
}

void EditorCore::add_subtree(const IObject::Ptr& parent, RemoteHandle object)
{
    auto label = walk_->labels.find(object);
    auto node = node_for(object, label != walk_->labels.end() ? string_view(label->second) : string_view("?"));
    if (!node) {
        return;
    }
    tree_->add(parent, node);
    auto kids = walk_->children.find(object);
    if (kids == walk_->children.end()) {
        return;
    }
    for (auto child : kids->second) {
        add_subtree(node, child);
    }
}

void EditorCore::objects_described(const RemoteDescriptions& described)
{
    for (auto& d : described.items) {
        string label = d.alive ? d.class_name : string("(gone)");
        if (!d.attachments.empty()) {
            label += " [";
            for (size_t i = 0; i < d.attachments.size(); ++i) {
                if (i) {
                    label += ", ";
                }
                label += class_name_of(d.attachments[i].class_uid);
            }
            label += "]";
        }
        walk_->labels[d.handle] = std::move(label);
    }

    next_nodes_.clear();
    if (tree_ && top_) {
        tree_->clear();
        tree_->set_root(top_);
        for (size_t i = 0; i < walk_->roots.size(); ++i) {
            auto root = node_for(walk_->roots[i].object.handle, walk_->roots[i].name);
            if (!root) {
                continue;
            }
            tree_->add(top_, root);
            if (walk_->hierarchy_roots[i]) {
                add_subtree(root, walk_->hierarchy_roots[i]);
            }
        }
    }
    nodes_ = std::move(next_nodes_);
    next_nodes_.clear();
    walk_.reset();
    write_state<IEditorCore>(this, [](IEditorCore::State& s) { ++s.revision; });
}

} // namespace velk::editor
