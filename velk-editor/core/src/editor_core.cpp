#include "editor_core.h"

#include <velk/api/future.h>
#include <velk/api/hierarchy.h>
#include <velk/interface/intf_array_any.h>
#include <velk/plugins/importer/plugin.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/json.h>

#include <cstdio>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

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
    if (editing_) {
        // Where the object comes from, and the values its file sets.
        if (const Source* src = source_of(d.handle)) {
            out += ",\"source\":";
            append_quoted(out, src->source);
            out += ",\"entry\":";
            append_quoted(out, src->entry);
            const JsonValue* authored = document_.properties(src->source, src->entry);
            out += ",\"authored\":";
            out += authored ? json_write_inline(*authored) : string("{}");
        } else {
            out += ",\"runtime\":true";
        }
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

namespace {

/// A command of apply(), resolved.
struct Command
{
    RemoteHandle object = 0;
    string name;
    bool reset = false;
    JsonValue value;
    RemoteValue live; ///< The value to write.
    RemoteValue live_before;
};

/// An apply() in progress.
struct Applying
{
    explicit Applying(const IFuture::Ptr& future) : promise(future) {}
    vector<Command> commands;
    string merge;
    Promise promise;
};

string error_json(string_view message)
{
    string out = "{\"error\":";
    json_quote(out, message);
    out += "}";
    return out;
}

string handle_text(RemoteHandle h)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "#%llu", static_cast<unsigned long long>(h));
    return string(string_view(buf, std::strlen(buf)));
}

/// True for a store-file value that is not a plain value: a binding or a reference.
bool is_link(const JsonValue& v)
{
    return v.find("bind") || v.find("ref");
}

} // namespace

const EditorCore::Source* EditorCore::source_of(RemoteHandle object) const
{
    auto it = sources_.find(object);
    return it != sources_.end() ? &it->second : nullptr;
}

IFuture::Ptr EditorCore::fn_apply(string commands, string merge)
{
    IFuture::Ptr future = instance().create_future();
    auto state = std::make_shared<Applying>(future);
    state->merge = merge;
    auto fail = [&](string_view message) {
        state->promise.set_value(error_json(message));
        return future;
    };
    if (!client_ || !client_.is_connected()) {
        return fail("not attached");
    }
    JsonValue list;
    string parse_error;
    if (!json_parse(commands, list, parse_error) || !list.is_array()) {
        return fail(parse_error.empty() ? string_view("commands: expected a JSON array") : string_view(parse_error));
    }
    for (auto& c : list.as_array()) {
        Command cmd;
        const string_view op = c["op"].type() == JsonType::String ? string_view(c["op"].as_string()) : string_view();
        cmd.object = static_cast<RemoteHandle>(c["object"].as_number());
        cmd.name = c["name"].as_string();
        if ((op != "set" && op != "reset") || !cmd.object || cmd.name.empty()) {
            return fail("each command needs op (set or reset), object and name");
        }
        cmd.reset = op == "reset";
        if (!cmd.reset) {
            if (!c.find("value")) {
                return fail("set needs a value");
            }
            cmd.value = c["value"];
        }
        if (editing_) {
            const Source* src = source_of(cmd.object);
            if (!src) {
                return fail(handle_text(cmd.object) + " was created at runtime; its changes are not kept");
            }
            const JsonValue* current = document_.get(src->source, src->entry, cmd.name);
            if (current && is_link(*current)) {
                return fail(cmd.name + " of " + handle_text(cmd.object) + " is bound or a reference in "
                            + src->entry);
            }
        } else if (cmd.reset) {
            return fail("reset needs a launched application");
        }
        state->commands.push_back(std::move(cmd));
    }
    if (state->commands.empty()) {
        state->promise.set_value(string("{\"ok\":true}"));
        return future;
    }

    // Read the current values: their types, and what undo writes back.
    vector<RemotePropertyName> names;
    for (auto& c : state->commands) {
        names.push_back({c.object, c.name});
    }
    IEditorCore::WeakPtr weak = get_self<IEditorCore>();
    client_.get(names).then([weak, state](const RemotePropertyValues& got) {
        auto self = weak.lock();
        auto* core = static_cast<EditorCore*>(self.get());
        if (!core) {
            state->promise.set_value(error_json("the core is gone"));
            return;
        }
        if (got.items.size() != state->commands.size()) {
            state->promise.set_value(error_json("cannot read the properties"));
            return;
        }
        vector<Uid> reset_classes;
        for (size_t i = 0; i < state->commands.size(); ++i) {
            auto& c = state->commands[i];
            if (failed(got.items[i].status)) {
                state->promise.set_value(error_json(handle_text(c.object) + " has no property " + c.name));
                return;
            }
            c.live_before = got.items[i].value;
            if (c.reset) {
                auto cls = core->classes_.find(c.object);
                if (cls == core->classes_.end()) {
                    state->promise.set_value(error_json(handle_text(c.object) + ": class unknown"));
                    return;
                }
                reset_classes.push_back(cls->second);
            } else if (!core->json_value(json_write_inline(c.value), c.live_before.type, c.live)) {
                state->promise.set_value(error_json(json_write_inline(c.value) + " is not a value of " + c.name));
                return;
            }
        }
        auto commit = [weak, state]() {
            auto self = weak.lock();
            auto* core = static_cast<EditorCore*>(self.get());
            if (!core) {
                state->promise.set_value(error_json("the core is gone"));
                return;
            }
            Step step;
            step.merge = state->merge;
            for (auto& c : state->commands) {
                Change ch;
                ch.object = c.object;
                ch.name = c.name;
                ch.live_before = c.live_before;
                ch.live_after = c.live;
                if (const Source* src = core->source_of(c.object)) {
                    ch.source = src->source;
                    ch.entry = src->entry;
                    if (const JsonValue* before = core->document_.get(ch.source, ch.entry, ch.name)) {
                        ch.had_before = true;
                        ch.before = *before;
                    }
                }
                ch.has_after = !c.reset;
                ch.after = c.value;
                step.changes.push_back(std::move(ch));
            }
            if (core->editing_) {
                core->put_document(step, true);
            }
            vector<RemotePropertyValue> values;
            for (auto& c : state->commands) {
                values.push_back({c.object, c.name, {}, c.live});
            }
            core->client_.set(values).then([weak, state, step](const RemoteResults& results) mutable {
                auto self = weak.lock();
                auto* core = static_cast<EditorCore*>(self.get());
                if (!core) {
                    state->promise.set_value(error_json("the core is gone"));
                    return;
                }
                auto& changes = step.changes;
                for (size_t i = 0; i < changes.size(); ++i) {
                    if (i >= results.items.size() || failed(results.items[i])) {
                        // Take back the document side and the live writes that went through.
                        if (core->editing_) {
                            core->put_document(step, false);
                        }
                        vector<RemotePropertyValue> back;
                        for (size_t j = 0; j < changes.size() && j < results.items.size(); ++j) {
                            if (succeeded(results.items[j])) {
                                back.push_back({changes[j].object, changes[j].name, {}, changes[j].live_before});
                            }
                        }
                        if (!back.empty()) {
                            core->client_.set(back);
                        }
                        state->promise.set_value(
                            error_json("cannot set " + changes[i].name + " of " + handle_text(changes[i].object)));
                        return;
                    }
                }
                if (core->editing_) {
                    if (!core->merge_into_last(step)) {
                        step.id = ++core->next_step_;
                        core->undo_.push_back(std::move(step));
                    }
                    core->redo_.clear();
                    core->history_changed();
                }
                state->promise.set_value(string("{\"ok\":true}"));
            });
        };
        if (reset_classes.empty()) {
            commit();
            return;
        }
        // Reset writes the class default: read it from a new object of the class.
        core->client_.create(reset_classes).then([weak, state, commit](const RemoteObjects& created) {
            auto self = weak.lock();
            auto* core = static_cast<EditorCore*>(self.get());
            if (!core) {
                state->promise.set_value(error_json("the core is gone"));
                return;
            }
            vector<RemotePropertyName> defaults;
            vector<RemoteHandle> temps;
            size_t k = 0;
            for (auto& c : state->commands) {
                if (c.reset) {
                    const RemoteHandle temp = k < created.items.size() ? created.items[k].handle : 0;
                    ++k;
                    defaults.push_back({temp, c.name});
                    if (temp) {
                        temps.push_back(temp);
                    }
                }
            }
            core->client_.get(defaults).then([weak, state, commit, temps](const RemotePropertyValues& got) {
                auto self = weak.lock();
                auto* core = static_cast<EditorCore*>(self.get());
                if (core) {
                    core->client_.release(temps);
                }
                size_t k = 0;
                for (auto& c : state->commands) {
                    if (c.reset) {
                        if (k >= got.items.size() || failed(got.items[k].status)) {
                            state->promise.set_value(error_json("cannot read the default of " + c.name));
                            return;
                        }
                        c.live = got.items[k].value;
                        ++k;
                    }
                }
                commit();
            });
        });
    });
    return future;
}

void EditorCore::put_document(const Step& step, bool after)
{
    // Changes are written in order and taken back in reverse.
    const size_t n = step.changes.size();
    for (size_t i = 0; i < n; ++i) {
        const Change& c = after ? step.changes[i] : step.changes[n - 1 - i];
        if (c.entry.empty()) {
            continue;
        }
        const bool present = after ? c.has_after : c.had_before;
        document_.put(c.source, c.entry, c.name, present ? (after ? &c.after : &c.before) : nullptr);
    }
}

IFuture::Ptr EditorCore::replay(vector<Step>& from, vector<Step>& to, bool after)
{
    IFuture::Ptr future = instance().create_future();
    Promise promise(future);
    if (from.empty() || !client_ || !client_.is_connected()) {
        promise.set_value(ReturnValue::Fail);
        return future;
    }
    Step step = std::move(from.back());
    from.pop_back();
    put_document(step, after);
    vector<RemotePropertyValue> values;
    const size_t n = step.changes.size();
    for (size_t i = 0; i < n; ++i) {
        const Change& c = after ? step.changes[i] : step.changes[n - 1 - i];
        values.push_back({c.object, c.name, {}, after ? c.live_after : c.live_before});
    }
    to.push_back(std::move(step));
    history_changed();
    client_.set(values).then([promise](const RemoteResults& results) mutable {
        ReturnValue rv = ReturnValue::Success;
        for (auto r : results.items) {
            if (failed(r)) {
                rv = ReturnValue::Fail;
            }
        }
        promise.set_value(rv);
    });
    return future;
}

IFuture::Ptr EditorCore::fn_undo()
{
    return replay(undo_, redo_, false);
}

IFuture::Ptr EditorCore::fn_redo()
{
    return replay(redo_, undo_, true);
}

void EditorCore::history_changed()
{
    write_state<IEditorCore>(this, [&](IEditorCore::State& s) {
        s.can_undo = !undo_.empty();
        s.can_redo = !redo_.empty();
        s.modified = (undo_.empty() ? 0 : undo_.back().id) != saved_step_;
    });
}

bool EditorCore::merge_into_last(Step& step)
{
    if (step.merge.empty() || undo_.empty() || !redo_.empty()) {
        return false;
    }
    Step& last = undo_.back();
    if (last.merge != step.merge || last.id == saved_step_ || last.changes.size() != step.changes.size()) {
        return false;
    }
    for (size_t i = 0; i < step.changes.size(); ++i) {
        if (last.changes[i].object != step.changes[i].object || last.changes[i].name != step.changes[i].name) {
            return false;
        }
    }
    // The last step keeps its before values and takes the new after values.
    for (size_t i = 0; i < step.changes.size(); ++i) {
        Change& c = last.changes[i];
        c.has_after = step.changes[i].has_after;
        c.after = std::move(step.changes[i].after);
        c.live_after = std::move(step.changes[i].live_after);
    }
    return true;
}

string EditorCore::fn_save()
{
    if (!editing_) {
        return error_json("save needs a launched application");
    }
    string out = "{\"saved\":[";
    bool first = true;
    for (auto& source : document_.modified_sources()) {
        string error;
        if (!document_.save(source, error)) {
            history_changed();
            return error_json(error);
        }
        if (!first) {
            out += ",";
        }
        first = false;
        json_quote(out, source);
    }
    out += "]}";
    saved_step_ = undo_.empty() ? 0 : undo_.back().id;
    history_changed();
    return out;
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

template <class T, class Next>
void EditorCore::then(Future<T> future, Next step)
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
    return connect(host, port);
}

ReturnValue EditorCore::fn_launch(string folder)
{
    fn_detach();
    const uint16_t port = free_port();
#ifdef _WIN32
    const string exe = executable_folder() + "velk_run.exe";
#else
    const string exe = executable_folder() + "velk_run";
#endif
    char port_text[8];
    std::snprintf(port_text, sizeof(port_text), "%u", static_cast<unsigned>(port));
    const vector<string> args{folder};
    const vector<std::pair<string, string>> env{
        {string("VELK_EDITOR_PORT"), string(string_view(port_text, std::strlen(port_text)))},
        {string("VELK_TRACK_SOURCES"), string("1")}};
    if (!port || !child_.start(exe, args, env)) {
        VELK_LOG(E, "launch: cannot start %s", exe.c_str());
        return ReturnValue::Fail;
    }
    // Wait for the endpoint to listen.
    editing_ = true;
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!child_.running()) {
            VELK_LOG(E, "launch: velk_run %s exited", folder.c_str());
            break;
        }
        if (succeeded(connect("127.0.0.1", port))) {
            write_state<IEditorCore>(this, [](IEditorCore::State& s) { s.editing = true; });
            return ReturnValue::Success;
        }
    }
    editing_ = false;
    child_.kill();
    return ReturnValue::Fail;
}

ReturnValue EditorCore::connect(string_view host, uint32_t port)
{
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
    child_.kill();
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
    app_ = 0;
    editing_ = false;
    sources_.clear();
    classes_.clear();
    document_.clear();
    undo_.clear();
    redo_.clear();
    next_step_ = 0;
    saved_step_ = 0;
    write_state<IEditorCore>(this, [&](IEditorCore::State& s) {
        s.attached = false;
        s.target = {};
        s.editing = false;
        s.can_undo = false;
        s.can_redo = false;
        s.modified = false;
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
        if (r.name == "app") {
            app_ = r.object.handle;
        }
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
    classes_.clear();
    for (auto& d : described.items) {
        if (d.alive) {
            classes_[d.handle] = d.class_uid;
            walk_->sourced.push_back(d.handle);
        }
        for (auto& a : d.attachments) {
            classes_[a.handle] = a.class_uid;
            walk_->sourced.push_back(a.handle);
        }
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
    if (!editing_ || !app_ || walk_->sourced.empty()) {
        walk_done();
        return;
    }
    // Ask where each object and attachment comes from, in one round trip.
    vector<RemoteCall> calls;
    for (auto h : walk_->sourced) {
        calls.push_back({app_, string("source"), {make_remote_reference(h)}});
    }
    then(client_.call(calls), &EditorCore::sources_read);
}

void EditorCore::sources_read(const RemoteCallResults& results)
{
    sources_.clear();
    for (size_t i = 0; i < results.items.size() && i < walk_->sourced.size(); ++i) {
        auto& v = results.items[i].value;
        if (failed(results.items[i].status) || v.form != RemoteValue::Form::Array || v.elements.size() != 2) {
            continue;
        }
        Source src;
        if (!read_remote_value(v.elements[0], src.source) || !read_remote_value(v.elements[1], src.entry)) {
            continue;
        }
        if (!document_.has(src.source)) {
            string error;
            if (!document_.load(src.source, error)) {
                VELK_LOG(E, "editor core: %s", error.c_str());
                continue;
            }
        }
        if (document_.has_entry(src.source, src.entry)) {
            sources_[walk_->sourced[i]] = std::move(src);
        }
    }
    walk_done();
}

void EditorCore::walk_done()
{
    walk_.reset();
    write_state<IEditorCore>(this, [](IEditorCore::State& s) { ++s.revision; });
}

} // namespace velk::editor
