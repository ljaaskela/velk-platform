// velk_editor [attach=PORT] [service=PORT]
//
// attach=PORT attaches to an application started with its editor endpoint on that port
// (VELK_EDITOR_PORT=PORT) and shows its object tree.
// service=PORT is where other front ends (the MCP server) reach this editor's core; 47900 by default.

#include <velk/api/hierarchy.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk/interface/resource/intf_resource.h>
#include <velk/interface/resource/intf_resource_store.h>
#include <velk/plugins/importer/api/importer.h>
#include <velk/plugins/remote/api/remote.h>

#include <velk-editor/core/intf_editor_core.h>
#include <velk-editor/core/plugin.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-runtime/api/application.h>
#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
#include <velk-ui/plugins/text/api/text_visual.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint16_t kDefaultServicePort = 47900;

/// The text visual of the element with @p id in the editor's UI file.
velk::ui::TextVisual text_of(velk::IStore& store, velk::string_view id)
{
    velk::Element element(interface_pointer_cast<velk::IElement>(store.find(id)));
    return velk::ui::TextVisual(element.find_attachment<velk::ui::ITextVisual>());
}

void append_subtree(velk::IHierarchy& tree, const velk::IObject::Ptr& node, uint32_t depth, velk::string& out)
{
    if (auto state = velk::read_state<velk::editor::IEditorNode>(node)) {
        for (uint32_t i = 0; i < depth; ++i) {
            out += "  ";
        }
        out += state->label;
        out += "\n";
    }
    for (auto& child : tree.children_of(node)) {
        append_subtree(tree, child, depth + 1, out);
    }
}

/// The core's tree as indented text, without its top node.
velk::string tree_text(const velk::editor::IEditorCore::Ptr& core)
{
    velk::string text;
    auto state = velk::read_state<velk::editor::IEditorCore>(core);
    auto tree = state ? state->tree.get<velk::IHierarchy>() : nullptr;
    if (tree && tree->root()) {
        for (auto& root : tree->children_of(tree->root())) {
            append_subtree(*tree, root, 0, text);
        }
    }
    return text;
}

} // namespace

int main(int argc, char* argv[])
{
    uint16_t attach_port = 0;
    uint16_t service_port = kDefaultServicePort;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "attach=", 7) == 0) {
            attach_port = static_cast<uint16_t>(std::atoi(argv[i] + 7));
        } else if (std::strncmp(argv[i], "service=", 8) == 0) {
            service_port = static_cast<uint16_t>(std::atoi(argv[i] + 8));
        }
    }

    velk::ApplicationConfig config;
    auto app = velk::create_app(config);

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
    auto hierarchy_text = text_of(store, "hierarchy_tree");
    auto status_text = text_of(store, "status_label");
    auto viewport_text = text_of(store, "viewport_label");

    // One core; this window uses it directly, other front ends through the service.
    velk::load_remote_plugin();
    velk::instance().plugin_registry().load_plugin("plugin:velk_editor_core");
    auto core = velk::instance().create<velk::editor::IEditorCore>(velk::editor::ClassId::EditorCore);
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

    if (attach_port && failed(core->fn_attach("127.0.0.1", attach_port))) {
        VELK_LOG(E, "velk-editor: cannot attach to port %u", static_cast<unsigned>(attach_port));
    }

    uint32_t shown_revision = ~0u;
    bool shown_attached = false;
    uint64_t frame = 0;
    while (app.poll()) {
        // Until the core follows changes, read the tree again about once a second.
        if (++frame % 60 == 0) {
            core->fn_refresh();
        }
        if (auto state = velk::read_state<velk::editor::IEditorCore>(core)) {
            if (state->revision != shown_revision) {
                shown_revision = state->revision;
                hierarchy_text.set_text(tree_text(core));
            }
            if (state->attached != shown_attached || frame == 1) {
                shown_attached = state->attached;
                status_text.set_text(shown_attached ? velk::string_view(state->target) : velk::string_view("Not attached"));
                viewport_text.set_text(shown_attached ? velk::string_view("Attached; no view yet")
                                                      : velk::string_view("No application attached"));
            }
        }
        app.update();
        app.present();
    }
    return 0;
}
