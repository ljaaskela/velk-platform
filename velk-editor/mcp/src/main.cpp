// velk_editor_mcp [service=PORT]
//
// MCP server (stdio) for AI agents. Talks to the core of a running editor on PORT (47900 by
// default), or, with no editor running, to a core of its own. Tools: launch, attach, tree,
// describe, apply, undo, redo, save, screenshot. Values are store-file JSON. Logs go to stderr; stdout
// carries only protocol.

#include <velk/json.h>

#include <velk/api/future.h>
#include <velk/api/velk.h>
#include <velk/plugins/remote/api/remote.h>
#include <velk/string.h>

#include <velk-editor/core/intf_editor_core.h>
#include <velk-editor/core/plugin.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace velk;

namespace {

constexpr uint16_t kDefaultServicePort = 47900;

string base64(const vector<uint8_t>& data)
{
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string out;
    size_t i = 0;
    char quad[4];
    for (; i + 2 < data.size(); i += 3) {
        const uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        quad[0] = table[(n >> 18) & 63]; quad[1] = table[(n >> 12) & 63];
        quad[2] = table[(n >> 6) & 63]; quad[3] = table[n & 63];
        out += string_view(quad, 4);
    }
    if (i < data.size()) {
        const uint32_t n = (data[i] << 16) | (i + 1 < data.size() ? data[i + 1] << 8 : 0);
        quad[0] = table[(n >> 18) & 63]; quad[1] = table[(n >> 12) & 63];
        quad[2] = i + 1 < data.size() ? table[(n >> 6) & 63] : '=';
        quad[3] = '=';
        out += string_view(quad, 4);
    }
    return out;
}

// ---- The core, reached through velk_remote --------------------------------------------------

/** @brief Runs updates until @p f resolves, or @p seconds pass. */
template <class T>
bool wait(Future<T>& f, int seconds = 30)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!f.is_ready() && std::chrono::steady_clock::now() < until) {
        instance().update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return f.is_ready();
}

class Session
{
public:
    explicit Session(uint16_t service_port)
    {
        auto& plugins = instance().plugin_registry();
        load_remote_plugin();
        plugins.load_plugin("plugin:velk_importer");
        client_ = create_remote_client();
        if (failed(client_.connect("127.0.0.1", service_port))) {
            // No editor running: hold a core here. velk_ui's type extensions write its values.
            std::fprintf(stderr, "velk_editor_mcp: no editor on port %u, using a core of its own\n",
                         static_cast<unsigned>(service_port));
            plugins.load_plugin("plugin:velk_render");
            plugins.load_plugin("plugin:velk_scene");
            plugins.load_plugin("plugin:velk_ui");
            plugins.load_plugin("plugin:velk_editor_core");
            own_core_ = instance().create<IObject>(editor::ClassId::EditorCore);
            own_server_ = create_remote_server();
            own_server_.publish("core", own_core_);
            client_.connect_local(own_server_);
        }
        auto roots = client_.roots();
        if (wait(roots, 5)) {
            for (auto& r : roots.get_result().get_value().items) {
                if (r.name == "core") {
                    core_ = r.object.handle;
                }
            }
        }
    }

    bool ready() const { return core_ != 0; }

    /** @brief Calls a core function and returns its result, or Form::None on failure. */
    RemoteCallResult call(string_view function, vector<RemoteValue> args, int seconds = 30)
    {
        const RemoteCall calls[] = {{core_, string(function), std::move(args)}};
        auto f = client_.call(calls);
        if (!wait(f, seconds)) {
            return {ReturnValue::Fail, {}};
        }
        auto results = f.get_result().get_value();
        return results.items.empty() ? RemoteCallResult{ReturnValue::Fail, {}} : results.items[0];
    }

    RemoteDescription describe(RemoteHandle object)
    {
        const RemoteHandle handles[] = {object};
        auto f = client_.describe(handles);
        if (!wait(f, 5)) {
            return {};
        }
        auto d = f.get_result().get_value();
        return d.items.empty() ? RemoteDescription{} : d.items[0];
    }

    const RemoteValue* property(const RemoteDescription& d, string_view name)
    {
        for (auto& p : d.properties) {
            if (p.name == name) {
                return &p.value;
            }
        }
        return nullptr;
    }

    /** @brief The core's tree as indented lines: "label  #handle". */
    string tree_text()
    {
        // Read the tree again first and wait for it, so the answer is current.
        auto before = describe(core_);
        const RemoteValue* revision_value = property(before, "revision");
        uint32_t revision = 0;
        if (revision_value) {
            read_remote_value(*revision_value, revision);
        }
        call("refresh", {});
        for (int i = 0; i < 2000; ++i) {
            auto now = describe(core_);
            uint32_t r = 0;
            bool attached = false;
            if (auto* v = property(now, "revision")) read_remote_value(*v, r);
            if (auto* v = property(now, "attached")) read_remote_value(*v, attached);
            if (r != revision || !attached) {
                before = now;
                break;
            }
            instance().update();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const RemoteValue* tree = property(before, "tree");
        if (!tree || tree->form != RemoteValue::Form::Object || !tree->object) {
            return "no tree";
        }
        const RemoteHandle hierarchy = tree->object;
        const RemoteHandle top = describe(hierarchy).hierarchy_root.handle;
        string out;
        add_lines(hierarchy, top, 0, out);
        return out;
    }

private:
    void add_lines(RemoteHandle hierarchy, RemoteHandle node, uint32_t depth, string& out)
    {
        const RemotePropertyName names[] = {{node, "label"}, {node, "object"}};
        auto got = client_.get(names);
        if (wait(got, 5)) {
            auto values = got.get_result().get_value().items;
            string label;
            uint64_t object = 0;
            if (values.size() == 2) {
                read_remote_value(values[0].value, label);
                read_remote_value(values[1].value, object);
            }
            for (uint32_t i = 0; i < depth; ++i) {
                out += "  ";
            }
            out += label;
            if (object) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "  #%llu", static_cast<unsigned long long>(object));
                out += string_view(buf, std::strlen(buf));
            }
            out += "\n";
        }
        const RemoteNode nodes[] = {{hierarchy, node}};
        auto children = client_.children(nodes);
        if (!wait(children, 5)) {
            return;
        }
        auto list = children.get_result().get_value().items;
        if (list.empty()) {
            return;
        }
        for (auto& c : list[0].children) {
            add_lines(hierarchy, c.handle, depth + 1, out);
        }
    }

    RemoteClient client_;
    RemoteServer own_server_;
    IObject::Ptr own_core_;
    RemoteHandle core_ = 0;
};

// ---- MCP ------------------------------------------------------------------------------------

const char* kTools = R"([
{"name":"launch","description":"Starts the application in a folder with an app.velk, for editing, and returns its object tree. describe then shows each object's source file, entry and authored values.","inputSchema":{"type":"object","properties":{"folder":{"type":"string"}},"required":["folder"]}},
{"name":"attach","description":"Attach the editor core to an application started with its editor endpoint (VELK_EDITOR_PORT=port), for inspecting: changes are not kept.","inputSchema":{"type":"object","properties":{"port":{"type":"integer"},"host":{"type":"string","description":"127.0.0.1 by default"}},"required":["port"]}},
{"name":"tree","description":"The attached application's object tree, one object per line, indented, with its handle after #.","inputSchema":{"type":"object","properties":{}}},
{"name":"describe","description":"An object's class, its properties as store-file JSON values, which are read-only, and its attachments.","inputSchema":{"type":"object","properties":{"object":{"type":"integer","description":"Handle from tree or describe"}},"required":["object"]}},
{"name":"apply","description":"Applies commands as one undo step. Each is {\"op\":\"set\",\"object\":handle,\"name\":property,\"value\":store-file JSON value, for example \"240px\", \"center\", 1.5 or {\"r\":1,\"g\":0,\"b\":0,\"a\":1}} or {\"op\":\"reset\",\"object\":handle,\"name\":property}, which removes the value from the file. After launch, changes go to the document and the application; after attach, to the application only.","inputSchema":{"type":"object","properties":{"commands":{"type":"array","items":{"type":"object"}}},"required":["commands"]}},
{"name":"undo","description":"Takes back the last apply.","inputSchema":{"type":"object","properties":{}}},
{"name":"redo","description":"Applies the last undone step again.","inputSchema":{"type":"object","properties":{}}},
{"name":"save","description":"Writes the files with unsaved changes. Needs a launched application.","inputSchema":{"type":"object","properties":{}}},
{"name":"screenshot","description":"The attached application's window as a PNG image.","inputSchema":{"type":"object","properties":{"window":{"type":"integer","description":"0 by default"}}}}
])";

string text_result(string_view text, bool error = false)
{
    string out = "{\"content\":[{\"type\":\"text\",\"text\":";
    json_quote(out, text);
    out += "}]";
    if (error) {
        out += ",\"isError\":true";
    }
    out += "}";
    return out;
}

/** @brief Runs a tools/call; returns the result object. */
string call_tool(Session& session, const string& name, const JsonValue& args)
{
    if (!session.ready()) {
        return text_result("no editor core available", true);
    }
    if (name == "attach") {
        string host = args["host"].type() == JsonType::String ? args["host"].as_string() : string("127.0.0.1");
        const auto port = static_cast<uint32_t>(args["port"].as_number());
        auto r = session.call("attach", {make_remote_value(string_view(host)), make_remote_value(port)});
        ReturnValue rv = ReturnValue::Fail;
        read_remote_value(r.value, rv);
        return succeeded(rv) ? text_result("attached") : text_result("cannot attach", true);
    }
    if (name == "launch") {
        auto r = session.call("launch", {make_remote_value(string_view(args["folder"].as_string()))}, 60);
        ReturnValue rv = ReturnValue::Fail;
        read_remote_value(r.value, rv);
        if (failed(rv)) {
            return text_result("cannot launch; see the server's log", true);
        }
        return text_result(session.tree_text());
    }
    if (name == "tree") {
        return text_result(session.tree_text());
    }
    if (name == "describe") {
        const auto object = static_cast<uint64_t>(args["object"].as_number());
        auto r = session.call("describe", {make_remote_value(object)});
        string text;
        read_remote_value(r.value, text);
        return text_result(text.empty() ? string_view("null") : string_view(text));
    }
    if (name == "apply") {
        auto r = session.call("apply", {make_remote_value(string_view(json_write_inline(args["commands"])))});
        string text;
        read_remote_value(r.value, text);
        if (text.empty()) {
            return text_result("no reply from the core", true);
        }
        return text_result(text, text.find("\"error\"") != string::npos);
    }
    if (name == "undo" || name == "redo") {
        auto r = session.call(name, {});
        ReturnValue rv = ReturnValue::Fail;
        read_remote_value(r.value, rv);
        return succeeded(rv) ? text_result(name == "undo" ? string_view("undone") : string_view("redone"))
                             : text_result(name == "undo" ? string_view("nothing to undo") : string_view("nothing to redo"), true);
    }
    if (name == "save") {
        auto r = session.call("save", {});
        string text;
        read_remote_value(r.value, text);
        if (text.empty()) {
            return text_result("no reply from the core", true);
        }
        return text_result(text, text.find("\"error\"") != string::npos);
    }
    if (name == "screenshot") {
        const auto window = static_cast<uint32_t>(args["window"].as_number());
        auto r = session.call("capture", {make_remote_value(window)}, 60);
        if (r.value.form != RemoteValue::Form::Array || r.value.data.empty()) {
            return text_result("no frame to capture", true);
        }
        string out = "{\"content\":[{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"";
        out += base64(r.value.data);
        out += "\"}]}";
        return out;
    }
    return text_result("unknown tool", true);
}

void respond(const JsonValue& id, string_view result_json, bool error = false)
{
    string out = "{\"jsonrpc\":\"2.0\",\"id\":";
    out += json_write_inline(id);
    out += error ? string_view(",\"error\":") : string_view(",\"result\":");
    out += result_json;
    out += "}\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
}

void handle(Session& session, const string& line)
{
    JsonValue message;
    string parse_error;
    if (!json_parse(line.data(), line.size(), message, parse_error) || message.type() != JsonType::Object) {
        return;
    }
    const JsonValue* id = message.find("id");
    const string& method = message["method"].as_string();
    if (!id) {
        return; // a notification
    }
    if (method == "initialize") {
        string result = "{\"protocolVersion\":";
        const auto& requested = message["params"]["protocolVersion"];
        if (requested.type() == JsonType::String) {
            json_quote(result, requested.as_string());
        } else {
            result += "\"2024-11-05\"";
        }
        result += ",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"velk-editor\",\"version\":\"0.1.0\"}}";
        respond(*id, result);
    } else if (method == "tools/list") {
        // One message per line: drop kTools' line breaks.
        string result = "{\"tools\":";
        for (const char* c = kTools; *c; ++c) {
            if (*c != '\n') {
                result += string_view(c, 1);
            }
        }
        result += "}";
        respond(*id, result);
    } else if (method == "tools/call") {
        const auto& params = message["params"];
        respond(*id, call_tool(session, params["name"].as_string(), params["arguments"]));
    } else if (method == "ping") {
        respond(*id, "{}");
    } else {
        respond(*id, "{\"code\":-32601,\"message\":\"method not found\"}", true);
    }
}

} // namespace

int main(int argc, char* argv[])
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    uint16_t service_port = kDefaultServicePort;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "service=", 8) == 0) {
            service_port = static_cast<uint16_t>(std::atoi(argv[i] + 8));
        }
    }

    Session session(service_port);

    // stdin is read on a thread; messages are handled on this one, between updates.
    std::mutex mutex;
    vector<string> inbox;
    bool closed = false;
    std::thread reader([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            std::lock_guard lock(mutex);
            inbox.push_back(string(string_view(line.data(), line.size())));
        }
        std::lock_guard lock(mutex);
        closed = true;
    });

    for (;;) {
        vector<string> lines;
        bool done = false;
        {
            std::lock_guard lock(mutex);
            for (auto& l : inbox) {
                lines.push_back(std::move(l));
            }
            inbox.clear();
            done = closed;
        }
        for (auto& l : lines) {
            handle(session, l);
        }
        if (done && lines.empty()) {
            break;
        }
        instance().update();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    reader.join();
    return 0;
}
