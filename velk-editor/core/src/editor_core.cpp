#include "editor_core.h"

#include <velk/api/future.h>
#include <velk/api/hierarchy.h>
#include <velk/interface/intf_array_any.h>
#include <velk/plugins/importer/plugin.h>
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

void append_quoted(string& out, string_view s)
{
    out += "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += "\\";
        }
        if (static_cast<unsigned char>(c) >= 0x20) {
            out += string_view(&c, 1);
        }
    }
    out += "\"";
}

void append_number(string& out, uint64_t n)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(n));
    out += string_view(buf, std::strlen(buf));
}

size_t element_size_of(Uid element_type)
{
    auto probe = instance().create_any(element_type);
    return probe ? probe->get_data_size(element_type) : 0;
}

/// A value read from the application, as an IAny of its type; null for types not registered here.
IAny::Ptr any_from(const RemoteValue& v)
{
    if (v.form != RemoteValue::Form::String && !instance().type_registry().find_factory(v.type)) {
        return nullptr;
    }
    switch (v.form) {
    case RemoteValue::Form::Bytes: {
        auto any = instance().create_any(v.type);
        if (!any || any->get_data_size(v.type) != v.data.size()
            || failed(any->set_data(v.data.data(), v.data.size(), v.type))) {
            return nullptr;
        }
        return any;
    }
    case RemoteValue::Form::String: {
        string s(string_view(reinterpret_cast<const char*>(v.data.data()), v.data.size()));
        auto any = instance().create_any(type_uid<string>());
        return any && succeeded(any->set_data(&s, sizeof(s), type_uid<string>())) ? any : nullptr;
    }
    case RemoteValue::Form::Array: {
        auto any = instance().create_any(v.type);
        auto* array = interface_cast<IArrayAny>(any);
        if (!array) {
            return nullptr;
        }
        if (!v.data.empty()) {
            const size_t size = element_size_of(v.element_type);
            if (!size || v.data.size() % size
                || failed(array->set_from_buffer(v.data.data(), v.data.size() / size, v.element_type))) {
                return nullptr;
            }
            return any;
        }
        for (auto& e : v.elements) {
            auto element = any_from(e);
            if (!element || failed(array->push_back(*element))) {
                return nullptr;
            }
        }
        return any;
    }
    default:
        return nullptr;
    }
}

/// @p any, a value of @p type, as it is sent to the application.
bool remote_from(const IAny& any, Uid type, RemoteValue& out)
{
    out = {};
    out.type = type;
    if (type == type_uid<string>()) {
        string s;
        if (failed(any.get_data(&s, sizeof(s), type))) {
            return false;
        }
        out.form = RemoteValue::Form::String;
        out.data.resize(s.size());
        if (!s.empty()) {
            std::memcpy(out.data.data(), s.data(), s.size());
        }
        return true;
    }
    if (auto* array = interface_cast<IArrayAny>(const_cast<IAny*>(&any))) {
        out.form = RemoteValue::Form::Array;
        out.element_type = array->element_type();
        const size_t n = array->array_size();
        const size_t size = element_size_of(out.element_type);
        if (size) {
            out.data.resize(n * size);
            if (n == 0 || succeeded(array->get_buffer(out.data.data(), n, out.element_type))) {
                return true;
            }
            out.data.clear();
        }
        out.elements.resize(n);
        for (size_t i = 0; i < n; ++i) {
            auto element = instance().create_any(out.element_type);
            if (!element || failed(array->get_at(i, *element))
                || !remote_from(*element, out.element_type, out.elements[i])) {
                return false;
            }
        }
        return true;
    }
    const size_t size = any.get_data_size(type);
    if (!size) {
        return false;
    }
    out.form = RemoteValue::Form::Bytes;
    out.data.resize(size);
    return succeeded(any.get_data(out.data.data(), size, type));
}

} // namespace

IJsonValueCodec* EditorCore::codec() const
{
    if (!codec_) {
        codec_ = instance().create<IJsonValueCodec>(::velk::ClassId::JsonImporter);
    }
    return codec_.get();
}

string EditorCore::value_json(const RemoteValue& value) const
{
    if (value.form == RemoteValue::Form::Object) {
        // Object references by handle until objects carry their store ids (#85).
        string out;
        if (value.object) {
            out += "{\"ref\":";
            append_number(out, value.object);
            out += "}";
        } else {
            out += "null";
        }
        return out;
    }
    auto any = any_from(value);
    auto* c = codec();
    string json = any && c ? c->to_json(*any, value.type) : string();
    return json.empty() ? string("null") : json;
}

bool EditorCore::json_value(string_view json, Uid type, RemoteValue& out) const
{
    auto* c = codec();
    auto any = c ? c->from_json(json, type) : nullptr;
    return any && remote_from(*any, type, out);
}

string EditorCore::description_json(const RemoteDescription& d) const
{
    string out = "{\"object\":";
    append_number(out, d.handle);
    if (!d.alive) {
        out += ",\"alive\":false}";
        return out;
    }
    out += ",\"class\":";
    append_quoted(out, d.class_name);
    out += ",\"properties\":{";
    bool first = true;
    for (auto& p : d.properties) {
        if (!first) {
            out += ",";
        }
        first = false;
        append_quoted(out, p.name);
        out += ":";
        out += succeeded(p.status) ? value_json(p.value) : string("null");
    }
    out += "},\"read_only\":[";
    first = true;
    for (auto& p : d.properties) {
        if (p.flags & ObjectFlags::ReadOnly) {
            if (!first) {
                out += ",";
            }
            first = false;
            append_quoted(out, p.name);
        }
    }
    out += "],\"attachments\":[";
    for (size_t i = 0; i < d.attachments.size(); ++i) {
        if (i) {
            out += ",";
        }
        out += "{\"object\":";
        append_number(out, d.attachments[i].handle);
        out += ",\"class\":";
        append_quoted(out, class_name_of(d.attachments[i].class_uid));
        out += "}";
    }
    out += "]";
    if (d.is_hierarchy) {
        out += ",\"hierarchy_root\":";
        append_number(out, d.hierarchy_root.handle);
    }
    out += "}";
    return out;
}

IFuture::Ptr EditorCore::fn_describe(uint64_t object)
{
    IFuture::Ptr future = instance().create_future();
    Promise promise(future);
    if (!client_ || !client_.is_connected()) {
        promise.set_value(string("{\"error\":\"not attached\"}"));
        return future;
    }
    IEditorCore::WeakPtr weak = get_self<IEditorCore>();
    const RemoteHandle handles[] = {object};
    client_.describe(handles).then([weak, promise](const RemoteDescriptions& d) mutable {
        auto self = weak.lock();
        auto* core = static_cast<EditorCore*>(self.get());
        promise.set_value(core && !d.items.empty() ? core->description_json(d.items[0]) : string("null"));
    });
    return future;
}

IFuture::Ptr EditorCore::fn_set(uint64_t object, string name, string json)
{
    IFuture::Ptr future = instance().create_future();
    Promise promise(future);
    if (!client_ || !client_.is_connected()) {
        promise.set_value(ReturnValue::Fail);
        return future;
    }
    // The property's type comes from its current value.
    IEditorCore::WeakPtr weak = get_self<IEditorCore>();
    const RemotePropertyName names[] = {{object, name}};
    client_.get(names).then([weak, promise, object, name, json](const RemotePropertyValues& got) mutable {
        auto self = weak.lock();
        auto* core = static_cast<EditorCore*>(self.get());
        RemoteValue value;
        if (!core || got.items.empty() || failed(got.items[0].status)
            || !core->json_value(json, got.items[0].value.type, value)) {
            promise.set_value(ReturnValue::Fail);
            return;
        }
        const RemotePropertyValue values[] = {{object, name, {}, value}};
        core->client_.set(values).then([promise](const RemoteResults& results) mutable {
            promise.set_value(results.items.empty() ? ReturnValue::Fail : results.items[0]);
        });
    });
    return future;
}

IFuture::Ptr EditorCore::fn_capture(uint32_t window)
{
    IFuture::Ptr future = instance().create_future();
    Promise promise(future);
    if (!client_ || !client_.is_connected()) {
        promise.set_value(vector<uint8_t>{});
        return future;
    }
    IEditorCore::WeakPtr weak = get_self<IEditorCore>();
    client_.roots().then([weak, promise, window](const RemoteRoots& roots) mutable {
        auto self = weak.lock();
        auto* core = static_cast<EditorCore*>(self.get());
        RemoteHandle app = 0;
        for (auto& r : roots.items) {
            if (r.name == "app") {
                app = r.object.handle;
            }
        }
        if (!core || !app) {
            promise.set_value(vector<uint8_t>{});
            return;
        }
        const RemoteCall calls[] = {{app, "capture", {make_remote_value(window)}}};
        core->client_.call(calls).then([promise](const RemoteCallResults& results) mutable {
            vector<uint8_t> png;
            if (!results.items.empty() && results.items[0].value.form == RemoteValue::Form::Array) {
                png = results.items[0].value.data;
            }
            promise.set_value(png);
        });
    });
    return future;
}

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
