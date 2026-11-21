// velk_editor [attach=PORT]
//
// attach=PORT attaches to an application started with its editor endpoint on that port
// (VELK_EDITOR_PORT=PORT) and shows its object tree.

#include <velk/api/event.h>
#include <velk/api/velk.h>
#include <velk/interface/resource/intf_resource.h>
#include <velk/interface/resource/intf_resource_store.h>
#include <velk/plugins/importer/api/importer.h>

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

/// The text visual of the element with @p id in the editor's UI file.
velk::ui::TextVisual text_of(velk::IStore& store, velk::string_view id)
{
    velk::Element element(interface_pointer_cast<velk::IElement>(store.find(id)));
    return velk::ui::TextVisual(element.find_attachment<velk::ui::ITextVisual>());
}

velk::string tree_text(const velk::vector<velk::editor::EditorNode>& tree)
{
    velk::string text;
    for (auto& node : tree) {
        for (uint32_t i = 0; i < node.depth; ++i) {
            text += "  ";
        }
        text += node.label;
        text += "\n";
    }
    return text;
}

} // namespace

int main(int argc, char* argv[])
{
    uint16_t attach_port = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "attach=", 7) == 0) {
            attach_port = static_cast<uint16_t>(std::atoi(argv[i] + 7));
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
    auto imported = velk::create_json_importer().import_from(json);
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

    auto& plugins = velk::instance().plugin_registry();
    plugins.load_plugin("plugin:velk_remote");
    plugins.load_plugin("plugin:velk_editor_core");
    auto core = velk::instance().create<velk::editor::IEditorCore>(velk::editor::ClassId::EditorCore);
    if (!core) {
        VELK_LOG(E, "velk-editor: the editor core is not available");
        return 1;
    }

    char attached_to[64];
    std::snprintf(attached_to, sizeof(attached_to), "Attached to 127.0.0.1:%u", static_cast<unsigned>(attach_port));
    velk::ScopedHandler on_attached(core->on_attached(), [&]() {
        status_text.set_text(velk::string_view(attached_to, std::strlen(attached_to)));
        viewport_text.set_text("Attached; no view yet");
    });
    velk::ScopedHandler on_detached(core->on_detached(), [&]() {
        status_text.set_text("Detached");
        viewport_text.set_text("No application attached");
    });
    velk::ScopedHandler on_tree(core->on_tree_changed(), [&]() { hierarchy_text.set_text(tree_text(core->tree())); });

    if (attach_port) {
        if (failed(core->attach("127.0.0.1", attach_port))) {
            status_text.set_text("Cannot attach");
            VELK_LOG(E, "velk-editor: cannot attach to port %u", static_cast<unsigned>(attach_port));
        }
    } else {
        hierarchy_text.set_text("");
    }

    uint64_t frame = 0;
    while (app.poll()) {
        // Until the core follows changes, read the tree again about once a second.
        if (++frame % 60 == 0) {
            core->refresh();
        }
        app.update();
        app.present();
    }
    return 0;
}
