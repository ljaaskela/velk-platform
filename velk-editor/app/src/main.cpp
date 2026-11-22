// velk_editor [<folder>] [attach=PORT] [service=PORT] [script=<prefix>]
//
// <folder> launches <folder>/app.velk for editing: edits go to its files and to the running
// application, Ctrl+Z undoes, Ctrl+Y or Ctrl+Shift+Z redoes, Ctrl+S saves.
// attach=PORT attaches to an application started with its editor endpoint on that port
// (VELK_EDITOR_PORT=PORT) for inspecting; edits change the application only.
// service=PORT is where other front ends (the MCP server) reach this editor's core; 47900 by default.
// script=<prefix> runs a fixed edit (select the object with a FixedSize, set its width, undo,
// redo, save), saves captures as <prefix>_1.png to _4.png, prints the results and exits.
//
// The hierarchy shows the core's tree; selecting an object shows its properties and those of
// its attachments in the inspector. A value is its store-file JSON; Enter applies it. Text
// that is not JSON is applied as a string.

#include <velk/api/event.h>
#include <velk/api/hierarchy.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk/interface/resource/intf_resource.h>
#include <velk/interface/resource/intf_resource_store.h>
#include <velk/json.h>
#include <velk/plugins/data/api/data.h>
#include <velk/plugins/importer/api/importer.h>
#include <velk/plugins/remote/api/remote.h>
#include <velk-editor/core/intf_editor_core.h>
#include <velk-editor/core/plugin.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-render/interface/intf_image.h>
#include <velk-runtime/api/application.h>
#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
#include <velk-ui/interface/intf_input_dispatcher.h>
#include <velk-ui/interface/trait/intf_tree_view.h>
#include <velk-ui/plugins/image/plugin.h>
#include <velk-ui/plugins/text/api/text_visual.h>
#include <velk-ui/plugins/text/intf_text_field.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

using velk::editor::IEditorCore;
using velk::editor::IEditorNode;

constexpr uint16_t kDefaultServicePort = 47900;

// GLFW key codes.
constexpr int kKeyS = 83;
constexpr int kKeyY = 89;
constexpr int kKeyZ = 90;

velk::ui::TextVisual text_of(velk::IStore& store, velk::string_view id)
{
    velk::Element element(interface_pointer_cast<velk::IElement>(store.find(id)));
    return velk::ui::TextVisual(element.find_attachment<velk::ui::ITextVisual>());
}

velk::string to_string(const char* s)
{
    return velk::string(velk::string_view(s, std::strlen(s)));
}

velk::string number_text(uint64_t n)
{
    char buf[24];
    const int len = std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(n));
    return velk::string(velk::string_view(buf, static_cast<size_t>(len)));
}

/// A property shown in the inspector.
struct PropertyRow
{
    uint64_t object = 0;
    velk::string name;
    velk::string value; ///< Store-file JSON.
    bool editable = false;
};

/// One object in the inspector: the selected object or one of its attachments.
struct Section
{
    uint64_t object = 0;
    velk::string title;
    velk::vector<PropertyRow> rows;
};

/// Reads one describe() result into a section.
Section section_of(const velk::string& description, bool editing)
{
    Section s;
    velk::JsonValue d;
    velk::string error;
    if (!velk::json_parse(description, d, error) || d.type() != velk::JsonType::Object) {
        s.title = "(cannot describe)";
        return s;
    }
    if (auto* o = d.find("object")) {
        s.object = static_cast<uint64_t>(o->as_number());
    }
    if (auto* c = d.find("class")) {
        s.title = c->as_string();
    } else {
        s.title = "(gone)";
    }
    const bool runtime = d.find("runtime") != nullptr;
    if (editing && runtime) {
        s.title += "  (runtime)";
    }
    auto* authored = d.find("authored");
    auto* read_only = d.find("read_only");
    if (auto* props = d.find("properties")) {
        for (auto& [name, value] : props->as_object()) {
            PropertyRow row;
            row.object = s.object;
            row.name = name;
            row.value = velk::json_write_inline(value);
            row.editable = !(editing && runtime);
            if (read_only) {
                for (auto& n : read_only->as_array()) {
                    if (n.as_string() == name) {
                        row.editable = false;
                    }
                }
            }
            // A value the file binds is not edited directly.
            if (authored) {
                if (auto* a = authored->find(name); a && a->type() == velk::JsonType::Object && a->find("bind")) {
                    row.editable = false;
                    row.value = velk::json_write_inline(*a);
                }
            }
            s.rows.push_back(std::move(row));
        }
    }
    return s;
}

void append_text_visual(velk::string& out, velk::string_view target, velk::string_view cls, velk::string_view text,
                        bool mono, float r, float g, float b)
{
    char color[96];
    std::snprintf(color, sizeof(color), "{\"r\":%.2f,\"g\":%.2f,\"b\":%.2f,\"a\":1.0}", r, g, b);
    out += ",{\"targets\":[\"";
    out += target;
    out += "\"],\"class\":\"";
    out += cls;
    out += "\",\"properties\":{\"text\":";
    velk::json_quote(out, text);
    out += ",\"font_size\":13.0,\"v_align\":\"center\",\"color\":";
    out += to_string(color);
    if (mono) {
        out += ",\"font\":{\"ref\":\"resources.mono\"}";
    }
    out += "}}";
}

/// The inspector's content as a store: a root "rows" with a header per section and a row per property.
/// Value elements are "v<i>" with i the index into @p flat.
velk::string inspector_store(const velk::vector<Section>& sections, velk::vector<const PropertyRow*>& flat)
{
    velk::string objects = "{\"id\":\"rows\",\"class\":\"velk-scene.Element\"}";
    velk::string rows_children;
    velk::string hierarchy;
    velk::string attachments =
        "{\"targets\":[\"rows\"],\"class\":\"velk-ui.Stack\",\"properties\":{\"axis\":1,\"spacing\":0}}"
        ",{\"targets\":[\"rows\"],\"class\":\"velk-ui.FixedSize\",\"properties\":{\"height\":\"auto\"}}"
        ",{\"targets\":[\"rows\"],\"class\":\"velk-ui.Padding\",\"properties\":{\"bottom\":\"4px\"}}";
    auto add_child = [&](const velk::string& id) {
        if (!rows_children.empty()) {
            rows_children += ",";
        }
        rows_children += "\"" + id + "\"";
    };
    size_t h = 0;
    for (auto& section : sections) {
        const velk::string hid = "h" + number_text(h++);
        objects += ",{\"id\":\"" + hid + "\",\"class\":\"velk-scene.Element\"}";
        add_child(hid);
        attachments += ",{\"targets\":[\"" + hid +
                       "\"],\"class\":\"velk-ui.FixedSize\",\"properties\":{\"height\":\"24px\"}}"
                       ",{\"targets\":[\"" + hid +
                       "\"],\"class\":\"velk-ui.Padding\",\"properties\":{\"left\":\"8px\"}}"
                       ",{\"targets\":[\"" + hid +
                       "\"],\"class\":\"velk-ui.RectVisual\",\"properties\":{\"color\":{\"r\":0.15,\"g\":0.15,\"b\":0.18,\"a\":1.0}}}";
        append_text_visual(attachments, hid, "velk-text.TextVisual", section.title, false, 0.75f, 0.75f, 0.8f);
        for (auto& row : section.rows) {
            const velk::string i = number_text(flat.size());
            flat.push_back(&row);
            const velk::string rid = "r" + i, nid = "n" + i, vid = "v" + i;
            objects += ",{\"id\":\"" + rid + "\",\"class\":\"velk-scene.Element\"}";
            objects += ",{\"id\":\"" + nid + "\",\"class\":\"velk-scene.Element\"}";
            objects += ",{\"id\":\"" + vid + "\",\"class\":\"velk-scene.Element\"}";
            add_child(rid);
            if (!hierarchy.empty()) {
                hierarchy += ",";
            }
            hierarchy += "\"" + rid + "\":[\"" + nid + "\",\"" + vid + "\"]";
            attachments += ",{\"targets\":[\"" + rid +
                           "\"],\"class\":\"velk-ui.Stack\",\"properties\":{\"axis\":0,\"spacing\":0}}"
                           ",{\"targets\":[\"" + rid +
                           "\"],\"class\":\"velk-ui.FixedSize\",\"properties\":{\"height\":\"24px\"}}"
                           ",{\"targets\":[\"" + nid +
                           "\"],\"class\":\"velk-ui.FixedSize\",\"properties\":{\"width\":\"104px\"}}"
                           ",{\"targets\":[\"" + nid +
                           "\"],\"class\":\"velk-ui.Margin\",\"properties\":{\"left\":\"12px\"}}"
                           ",{\"targets\":[\"" + vid +
                           "\"],\"class\":\"velk-ui.FixedSize\",\"properties\":{\"width\":\"166px\",\"height\":\"20px\"}}"
                           ",{\"targets\":[\"" + vid +
                           "\"],\"class\":\"velk-ui.Margin\",\"properties\":{\"top\":\"2px\"}}";
            append_text_visual(attachments, nid, "velk-text.TextVisual", row.name, false, 0.6f, 0.6f, 0.65f);
            if (row.editable) {
                attachments += ",{\"targets\":[\"" + vid +
                               "\"],\"class\":\"velk-ui.RectVisual\",\"properties\":{\"color\":{\"r\":0.17,\"g\":0.17,\"b\":0.2,\"a\":1.0}}}";
                append_text_visual(attachments, vid, "velk-text.TextField", row.value, true, 0.88f, 0.88f, 0.9f);
            } else {
                append_text_visual(attachments, vid, "velk-text.TextVisual", row.value, true, 0.5f, 0.5f, 0.55f);
            }
        }
    }
    velk::string out = "{\"version\":1,\"objects\":[" + objects + "],\"hierarchies\":{\"scene\":{\"rows\":[" +
                       rows_children + "]";
    if (!hierarchy.empty()) {
        out += "," + hierarchy;
    }
    out += "}},\"resources\":[{\"id\":\"mono\",\"uri\":\"font:memory://velk-text/JetBrainsMono-Regular.ttf\"}],"
           "\"attachments\":[" + attachments + "]}";
    return out;
}

bool save_capture(velk::Application& app, const velk::Window& window, const velk::string& path)
{
    velk::vector<uint8_t> pixels;
    velk::uvec2 size{};
    auto application = velk::IApplication::Ptr(app);
    if (!application || !application->read_window(window.as_ptr<velk::IObject>(), pixels, size)) {
        return false;
    }
    auto writer = velk::instance().create<velk::IImageWriter>(velk::ui::ClassId::ImageEncoder);
    return writer && writer->save_png(static_cast<int>(size.x), static_cast<int>(size.y), velk::PixelFormat::RGBA8,
                                      pixels.data(), pixels.size(), path);
}

velk::string read_file(const velk::string& path)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    auto s = ss.str();
    return velk::string(velk::string_view(s.data(), s.size()));
}

} // namespace

int main(int argc, char* argv[])
{
    uint16_t attach_port = 0;
    uint16_t service_port = kDefaultServicePort;
    velk::string folder;
    velk::string script;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "attach=", 7) == 0) {
            attach_port = static_cast<uint16_t>(std::atoi(argv[i] + 7));
        } else if (std::strncmp(argv[i], "service=", 8) == 0) {
            service_port = static_cast<uint16_t>(std::atoi(argv[i] + 8));
        } else if (std::strncmp(argv[i], "script=", 7) == 0) {
            script = to_string(argv[i] + 7);
        } else {
            folder = to_string(argv[i]);
        }
    }

    auto app = velk::create_app();
    velk::WindowConfig wc;
    wc.width = 1600;
    wc.height = 900;
    wc.title = "velk editor";
    wc.update_rate = velk::UpdateRate::VSync;
    auto window = app.create_window(wc);
    if (!window) {
        VELK_LOG(E, "velk-editor: cannot create the window");
        return 1;
    }

    // The editor keeps its UI file's store, to reach its elements by id.
    velk::string json;
    auto file = velk::instance().resource_store().get_resource<velk::IFile>("app://ui/editor.json");
    if (!file || failed(file->read_text(json))) {
        VELK_LOG(E, "velk-editor: cannot read ui/editor.json");
        return 1;
    }
    auto imported = velk::create_json_importer().import_from(json, "app://ui/editor.json");
    if (!imported.store) {
        VELK_LOG(E, "velk-editor: cannot import ui/editor.json");
        return 1;
    }
    auto ui = velk::create_scene();
    ui.load(*imported.store);
    auto camera = ui.find_first<velk::ICamera>();
    if (!camera) {
        VELK_LOG(E, "velk-editor: ui/editor.json has no camera");
        return 1;
    }
    app.add_view(window, camera);
    auto& store = *imported.store;
    auto status_text = text_of(store, "status_label");
    auto viewport_text = text_of(store, "viewport_label");
    velk::Element tree_element(interface_pointer_cast<velk::IElement>(store.find("hierarchy_tree")));
    auto tree_view = tree_element.find_attachment<velk::ui::ITreeView>();
    velk::Element inspector_body(interface_pointer_cast<velk::IElement>(store.find("inspector_body")));
    auto* ui_tree = interface_cast<velk::IHierarchy>(ui.as_ptr<velk::IScene>().get());

    // One core; this window uses it directly, other front ends through the service.
    velk::load_remote_plugin();
    velk::instance().plugin_registry().load_plugin("plugin:velk_editor_core");
    auto core = velk::instance().create<IEditorCore>(velk::editor::ClassId::EditorCore);
    if (!core) {
        VELK_LOG(E, "velk-editor: the editor core is not available");
        return 1;
    }
    auto service = velk::create_remote_server();
    service.publish("core", interface_pointer_cast<velk::IObject>(core));
    if (const uint16_t port = service.listen(service_port)) {
        VELK_LOG(I, "velk-editor: core service on port %u", static_cast<unsigned>(port));
    } else {
        VELK_LOG(E, "velk-editor: cannot listen on port %u for the core service", static_cast<unsigned>(service_port));
    }

    velk::string message;
    if (!folder.empty()) {
        if (failed(core->fn_launch(folder))) {
            message = "cannot launch " + folder;
        }
    } else if (attach_port && failed(core->fn_attach("127.0.0.1", attach_port))) {
        message = "cannot attach to port " + number_text(attach_port);
    }

    // ---- Inspector state ----
    uint64_t selected_handle = ~0ull;      // The object the inspector shows or is reading.
    uint32_t generation = 0;               // Bumped by each new read; older results are dropped.
    velk::vector<velk::Future<velk::string>> reading; // describe() of the object, then of its attachments.
    velk::vector<velk::string> descriptions;
    bool reading_attachments = false;
    velk::vector<Section> sections;
    velk::vector<const PropertyRow*> flat;
    velk::vector<velk::Element> value_elements;
    velk::vector<velk::ScopedHandler> field_subs;
    uint32_t built_generation = ~0u;
    velk::Future<velk::string> applying(nullptr);
    velk::Future<velk::ReturnValue> stepping(nullptr); // undo or redo
    bool stepping_undo = true;

    auto read_selection = [&](uint64_t handle) {
        selected_handle = handle;
        ++generation;
        reading.clear();
        descriptions.clear();
        reading_attachments = false;
        if (handle) {
            reading.emplace_back(core->fn_describe(handle));
        }
    };

    auto commit = [&](size_t index, const velk::string& text) {
        if (index >= flat.size()) {
            return;
        }
        const PropertyRow& row = *flat[index];
        velk::JsonValue value;
        velk::string error;
        velk::string value_json = velk::json_parse(text, value, error) ? text : velk::string();
        if (value_json.empty()) {
            velk::json_quote(value_json, text); // Not JSON: apply it as a string.
        }
        velk::string cmd = "[{\"op\":\"set\",\"object\":" + number_text(row.object) + ",\"name\":";
        velk::json_quote(cmd, row.name);
        cmd += ",\"value\":" + value_json + "}]";
        applying = velk::Future<velk::string>(core->fn_apply(cmd, {}));
    };

    auto undo = [&](bool is_undo) {
        stepping_undo = is_undo;
        stepping = velk::Future<velk::ReturnValue>(is_undo ? core->fn_undo() : core->fn_redo());
    };

    auto save = [&]() {
        velk::JsonValue r;
        velk::string error;
        auto result = core->fn_save();
        if (velk::json_parse(result, r, error) && r.find("error")) {
            message = r.find("error")->as_string();
        } else if (r.find("saved") && r.find("saved")->as_array().empty()) {
            message = "nothing to save";
        } else {
            message = "saved";
        }
    };

    velk::ScopedHandler key_sub = window.add_on_key_event([&](const velk::ui::KeyEvent& e) {
        if (e.action != velk::ui::KeyAction::Down ||
            (e.modifiers & velk::ui::Modifier::Ctrl) == velk::ui::Modifier::None) {
            return;
        }
        const bool shift = (e.modifiers & velk::ui::Modifier::Shift) != velk::ui::Modifier::None;
        if (e.key == kKeyZ) {
            undo(!shift);
        } else if (e.key == kKeyY) {
            undo(false);
        } else if (e.key == kKeyS) {
            save();
        }
    });

    // ---- Script ----
    auto* input = window.input();
    int step = 0;
    int wait_frames = 0;
    int result = 0;
    velk::string script_log;
    auto capture = [&](int n) {
        const velk::string path = script + "_" + number_text(static_cast<uint64_t>(n)) + ".png";
        if (!save_capture(app, window, path)) {
            VELK_LOG(E, "velk-editor: capture %s failed", path.c_str());
            result = 1;
        }
    };
    auto find_row = [&](velk::string_view section_class, velk::string_view name) -> int {
        for (size_t i = 0; i < flat.size(); ++i) {
            if (flat[i]->name == name) {
                for (auto& s : sections) {
                    if (s.object == flat[i]->object && velk::string_view(s.title).find(section_class) != velk::string_view::npos) {
                        return static_cast<int>(i);
                    }
                }
            }
        }
        return -1;
    };
    auto ctrl_key = [&](int key, bool shift = false) {
        velk::ui::KeyEvent ev;
        ev.key = key;
        ev.action = velk::ui::KeyAction::Down;
        ev.modifiers = shift ? (velk::ui::Modifier::Ctrl | velk::ui::Modifier::Shift) : velk::ui::Modifier::Ctrl;
        input->key_event(ev);
    };
    auto value_of = [&](int index) { return index >= 0 ? flat[static_cast<size_t>(index)]->value : velk::string(); };

    velk::IObject::Ptr tree_object;
    uint32_t shown_revision = ~0u;
    velk::string shown_status;
    uint64_t frame = 0;
    while (app.poll()) {
        // Until the core follows the application's changes, read the tree again every two seconds.
        if (++frame % 120 == 0) {
            core->fn_refresh();
        }
        auto state = velk::read_state<IEditorCore>(core);
        if (state && state->revision != shown_revision) {
            shown_revision = state->revision;
            auto tree = state->tree.get<velk::IObject>();
            if (tree && tree != tree_object) {
                tree_object = tree;
                auto source = velk::create_hierarchy_source(interface_pointer_cast<velk::IHierarchy>(tree));
                velk::write_state<velk::ui::ITreeView>(tree_view, [&](velk::ui::ITreeView::State& s) {
                    velk::set_object_ref(s.source, source);
                });
            }
        }

        // The selected node's object.
        uint64_t handle = 0;
        if (auto ts = velk::read_state<velk::ui::ITreeView>(tree_view)) {
            if (auto node = ts->selected.get<velk::IObject>()) {
                if (auto ns = velk::read_state<IEditorNode>(node)) {
                    handle = ns->object;
                }
            }
        }
        if (handle != selected_handle) {
            read_selection(handle);
        }

        // Descriptions: the object first, then its attachments.
        bool all_ready = !reading.empty();
        for (auto& f : reading) {
            all_ready = all_ready && f.is_ready();
        }
        if (all_ready) {
            for (auto& f : reading) {
                descriptions.push_back(f.get_result().get_value());
            }
            reading.clear();
            if (!reading_attachments) {
                reading_attachments = true;
                velk::JsonValue d;
                velk::string error;
                if (velk::json_parse(descriptions[0], d, error)) {
                    if (auto* atts = d.find("attachments")) {
                        for (auto& a : atts->as_array()) {
                            if (auto* o = a.find("object")) {
                                reading.emplace_back(core->fn_describe(static_cast<uint64_t>(o->as_number())));
                            }
                        }
                    }
                }
            }
            if (reading.empty()) {
                // Everything read: build the inspector.
                const bool editing = state && state->editing;
                sections.clear();
                for (auto& d : descriptions) {
                    sections.push_back(section_of(d, editing));
                }
                descriptions.clear();
                built_generation = generation;
            }
        }
        if (built_generation == generation && (!sections.empty() || selected_handle == 0)) {
            built_generation = ~0u;
            field_subs.clear();
            value_elements.clear();
            flat.clear();
            if (selected_handle == 0) {
                sections.clear();
            }
            if (ui_tree) {
                for (auto& child : ui_tree->children_of(velk::as_object(inspector_body.as_ptr<velk::IElement>()))) {
                    ui_tree->remove(child);
                }
            }
            auto text = inspector_store(sections, flat);
            auto rows = velk::create_json_importer().import_from(text, "editor:inspector");
            if (rows.store) {
                ui.load(*rows.store, inspector_body.as_ptr<velk::IElement>().get());
                for (size_t i = 0; i < flat.size(); ++i) {
                    velk::Element v(interface_pointer_cast<velk::IElement>(rows.store->find("v" + number_text(i))));
                    value_elements.push_back(v);
                    if (auto field = v.find_attachment<velk::ui::ITextField>()) {
                        field_subs.emplace_back(field->on_commit(),
                                                [&commit, i](const velk::string& t) { commit(i, t); });
                    }
                }
            } else {
                VELK_LOG(E, "velk-editor: cannot build the inspector");
            }
        }

        if (applying.is_ready()) {
            auto r = applying.get_result().get_value();
            applying = velk::Future<velk::string>(nullptr);
            velk::JsonValue v;
            velk::string error;
            message = velk::json_parse(r, v, error) && v.find("error") ? v.find("error")->as_string() : velk::string("applied");
            read_selection(selected_handle);
        }
        if (stepping.is_ready()) {
            const bool ok = succeeded(stepping.get_result().get_value());
            stepping = velk::Future<velk::ReturnValue>(nullptr);
            message = ok ? velk::string(stepping_undo ? "undone" : "redone")
                         : velk::string(stepping_undo ? "nothing to undo" : "nothing to redo");
            read_selection(selected_handle);
        }

        // Status line.
        velk::string status;
        if (state && state->attached) {
            status = state->editing ? "edit" : "inspect";
            status += "  " + state->target;
            if (state->modified) {
                status += "  modified";
            }
        } else {
            status = "not attached";
        }
        if (!message.empty()) {
            status += "  |  " + message;
        }
        if (status != shown_status) {
            shown_status = status;
            status_text.set_text(status);
            viewport_text.set_text(state && state->attached ? velk::string_view("No view yet")
                                                           : velk::string_view("No application attached"));
        }

        // The script: one step at a time, each when the previous one has finished.
        const bool busy = !reading.empty() || built_generation != ~0u || static_cast<bool>(applying) ||
                          static_cast<bool>(stepping);
        if (!script.empty() && input && !busy && ++wait_frames > 10) {
            wait_frames = 0;
            if (step == 0) {
                // Select the object with a FixedSize.
                velk::IObject::Ptr target;
                if (auto h = interface_pointer_cast<velk::IHierarchy>(tree_object)) {
                    velk::vector<velk::IObject::Ptr> todo{h->root()};
                    while (!todo.empty() && !target) {
                        auto n = todo.back();
                        todo.pop_back();
                        if (auto ns = velk::read_state<IEditorNode>(n);
                            ns && velk::string_view(ns->label).find("FixedSize") != velk::string_view::npos) {
                            target = n;
                        }
                        for (auto& c : h->children_of(n)) {
                            todo.push_back(c);
                        }
                    }
                }
                if (target) {
                    velk::write_state<velk::ui::ITreeView>(tree_view, [&](velk::ui::ITreeView::State& s) {
                        velk::set_object_ref(s.selected, target);
                    });
                    ++step;
                } else if (frame > 1200) {
                    VELK_LOG(E, "velk-editor: script: no object with a FixedSize in the tree");
                    result = 1;
                    break;
                }
            } else if (step == 1 && selected_handle) {
                capture(1);
                const int w = find_row("FixedSize", "width");
                if (w < 0) {
                    VELK_LOG(E, "velk-editor: script: no width row");
                    result = 1;
                    break;
                }
                script_log += "before \"" + value_of(w) + "\"";
                auto& v = value_elements[static_cast<size_t>(w)];
                auto m = v.get_world_matrix();
                auto size = v.get_size();
                velk::ui::PointerEvent ev;
                ev.position = {m.m[12] + size.width * 0.5f, m.m[13] + size.height * 0.5f};
                ev.button = velk::ui::PointerButton::Left;
                ev.action = velk::ui::PointerAction::Down;
                input->pointer_event(ev);
                ev.action = velk::ui::PointerAction::Up;
                input->pointer_event(ev);
                velk::ui::KeyEvent k;
                k.action = velk::ui::KeyAction::Down;
                k.named = velk::ui::Key::End;
                input->key_event(k);
                k.named = velk::ui::Key::Backspace;
                for (int i = 0; i < 40; ++i) {
                    input->key_event(k);
                }
                for (const char* c = "\"120px\""; *c; ++c) {
                    velk::ui::KeyEvent t;
                    t.action = velk::ui::KeyAction::Text;
                    t.codepoint = static_cast<unsigned char>(*c);
                    input->key_event(t);
                }
                k.named = velk::ui::Key::Enter;
                input->key_event(k);
                ++step;
            } else if (step == 2) {
                capture(2);
                script_log += ", applied \"" + value_of(find_row("FixedSize", "width")) + "\" (" + message + ")";
                ctrl_key(kKeyZ);
                ++step;
            } else if (step == 3) {
                capture(3);
                script_log += ", undone \"" + value_of(find_row("FixedSize", "width")) + "\" (" + message + ")";
                ctrl_key(kKeyZ, true);
                ++step;
            } else if (step == 4) {
                script_log += ", redone \"" + value_of(find_row("FixedSize", "width")) + "\" (" + message + ")";
                ctrl_key(kKeyS);
                ++step;
            } else if (step == 5) {
                capture(4);
                const auto saved = read_file(folder + "/scene.velk");
                script_log += ", save (" + message + "), file has 120px: ";
                script_log += to_string(velk::string_view(saved).find("120px") != velk::string_view::npos ? "yes" : "no");
                VELK_LOG(I, "velk-editor: script: %s; status \"%s\"", script_log.c_str(), shown_status.c_str());
                break;
            }
        }

        app.update();
        app.present();
    }
    core->fn_detach();
    return result;
}
