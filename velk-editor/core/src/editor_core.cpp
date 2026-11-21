#include "editor_core.h"

#include <velk/api/velk.h>

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

ReturnValue EditorCore::attach(string_view host, uint16_t port)
{
    detach();
    load_remote_plugin();
    client_ = create_remote_client();
    if (!client_ || failed(client_.connect(host, port))) {
        client_ = {};
        return ReturnValue::Fail;
    }
    IEditorCore::WeakPtr weak = get_self<IEditorCore>();
    disconnected_ = ScopedHandler(client_.as_ptr<IRemoteClient>()->on_disconnected(), [weak]() {
        if (auto self = weak.lock()) {
            auto* core = static_cast<EditorCore*>(self.get());
            core->walk_.reset();
            core->tree_.clear();
            invoke_event(core->get_interface(IInterface::UID), "on_detached");
            invoke_event(core->get_interface(IInterface::UID), "on_tree_changed");
        }
    });
    invoke_event(get_interface(IInterface::UID), "on_attached");
    refresh();
    return ReturnValue::Success;
}

void EditorCore::detach()
{
    if (client_) {
        client_.disconnect();
    }
    disconnected_ = {};
    client_ = {};
    walk_.reset();
}

bool EditorCore::is_attached() const
{
    return client_ && client_.is_connected();
}

void EditorCore::refresh()
{
    if (!is_attached() || walk_) {
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

    vector<EditorNode> tree;
    for (size_t i = 0; i < walk_->roots.size(); ++i) {
        tree.push_back({walk_->roots[i].object.handle, 0, walk_->roots[i].name});
        if (walk_->hierarchy_roots[i]) {
            add_subtree(walk_->hierarchy_roots[i], 1, tree);
        }
    }
    tree_ = std::move(tree);
    walk_.reset();
    invoke_event(get_interface(IInterface::UID), "on_tree_changed");
}

void EditorCore::add_subtree(RemoteHandle object, uint32_t depth, vector<EditorNode>& out) const
{
    auto label = walk_->labels.find(object);
    out.push_back({object, depth, label != walk_->labels.end() ? label->second : string("?")});
    auto kids = walk_->children.find(object);
    if (kids == walk_->children.end()) {
        return;
    }
    for (auto child : kids->second) {
        add_subtree(child, depth + 1, out);
    }
}

} // namespace velk::editor
