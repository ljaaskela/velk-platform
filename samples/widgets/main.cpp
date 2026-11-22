// velk_ui_widgets [script=<capture.png>]
//
// Two text fields, a scroll view, a tree and a list of 10,000 items. Click a field and type;
// Enter commits, Escape cancels, clicking elsewhere commits. The line below the fields shows
// the last committed value. The views scroll with the wheel (Shift for sideways) and their
// bars can be dragged. In the tree, + and - expand and collapse, and a click selects; in the
// list, a click selects.
//
// script=<capture.png> feeds clicks, keys and wheel turns instead, saves a capture while the
// second field is being edited, prints the values and exits.

#include <velk/api/event.h>
#include <velk/api/hierarchy.h>
#include <velk/api/object.h>
#include <velk/api/property.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/plugins/data/api/data.h>

#include <velk-render/interface/intf_camera.h>
#include <velk-render/interface/intf_image.h>
#include <velk-runtime/api/application.h>
#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
#include <velk-ui/interface/intf_input_dispatcher.h>
#include <velk-ui/interface/trait/intf_list_view.h>
#include <velk-ui/interface/trait/intf_scroll_view.h>
#include <velk-ui/plugins/image/plugin.h>
#include <velk-ui/plugins/text/api/text_visual.h>
#include <velk-ui/plugins/text/intf_text_field.h>
#include <velk-ui/plugins/text/intf_tree_view.h>

#include <cstdio>
#include <cstring>

namespace {

using velk::ui::Key;
using velk::ui::KeyAction;
using velk::ui::KeyEvent;
using velk::ui::PointerAction;
using velk::ui::PointerButton;
using velk::ui::PointerEvent;
using velk::ui::ScrollEvent;

void click(velk::ui::IInputDispatcher& input, float x, float y)
{
    PointerEvent ev;
    ev.position = {x, y};
    ev.button = PointerButton::Left;
    ev.action = PointerAction::Down;
    input.pointer_event(ev);
    ev.action = PointerAction::Up;
    input.pointer_event(ev);
}

void type(velk::ui::IInputDispatcher& input, const char* text)
{
    for (; *text; ++text) {
        KeyEvent ev;
        ev.action = KeyAction::Text;
        ev.codepoint = static_cast<unsigned char>(*text);
        input.key_event(ev);
    }
}

void press(velk::ui::IInputDispatcher& input, Key key, int times = 1)
{
    for (int i = 0; i < times; ++i) {
        KeyEvent ev;
        ev.named = key;
        ev.action = KeyAction::Down;
        input.key_event(ev);
    }
}

void wheel(velk::ui::IInputDispatcher& input, float x, float y, float lines, bool shift = false)
{
    ScrollEvent ev;
    ev.position = {x, y};
    ev.delta = {0.f, lines};
    ev.unit = velk::ui::ScrollUnit::Lines;
    ev.modifiers = shift ? velk::ui::Modifier::Shift : velk::ui::Modifier::None;
    input.scroll_event(ev);
}

velk::IObject::Ptr node(velk::string_view label)
{
    auto object = velk::create_object().as_ptr<velk::IObject>();
    auto* meta = interface_cast<velk::IMetadata>(object);
    velk::Property<velk::string>(meta->add_property("label", velk::type_uid<velk::string>(), nullptr))
        .set_value(velk::string(label));
    return object;
}

/// A tree for the tree view: a scene with lights, panels and props.
velk::Hierarchy make_tree()
{
    auto tree = velk::create_hierarchy();
    auto root = node("Scene");
    tree.set_root(root);
    tree.add(root, node("Camera"));
    auto lights = node("Lights");
    tree.add(root, lights);
    for (auto* name : {"Sun", "Fill", "Rim"}) {
        tree.add(lights, node(velk::string_view(name, std::strlen(name))));
    }
    auto panels = node("Panels");
    tree.add(root, panels);
    tree.add(panels, node("Header"));
    auto body = node("Body");
    tree.add(panels, body);
    for (auto* name : {"Title", "Text", "Button"}) {
        tree.add(body, node(velk::string_view(name, std::strlen(name))));
    }
    tree.add(panels, node("Footer"));
    for (int i = 1; i <= 10; ++i) {
        char name[16];
        const int n = std::snprintf(name, sizeof(name), "Prop %d", i);
        tree.add(root, node(velk::string_view(name, static_cast<size_t>(n))));
    }
    return tree;
}

velk::string text_of(const velk::Element& element)
{
    auto state = velk::read_state<velk::ui::ITextVisual>(element.find_attachment<velk::ui::ITextVisual>());
    return state ? state->text : velk::string();
}

bool save_capture(velk::Application& app, const velk::Window& window, const char* path)
{
    velk::vector<uint8_t> pixels;
    velk::uvec2 size{};
    auto application = velk::IApplication::Ptr(app);
    if (!application || !application->read_window(window.as_ptr<velk::IObject>(), pixels, size)) {
        return false;
    }
    auto writer = velk::instance().create<velk::IImageWriter>(velk::ui::ClassId::ImageEncoder);
    if (!writer) {
        return false;
    }
    return writer->save_png(static_cast<int>(size.x), static_cast<int>(size.y), velk::PixelFormat::RGBA8,
                            pixels.data(), pixels.size(), velk::string_view(path, std::strlen(path)));
}

} // namespace

int main(int argc, char* argv[])
{
    const char* script = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "script=", 7) == 0) {
            script = argv[i] + 7;
        }
    }

    auto app = velk::create_app();
    velk::WindowConfig wc;
    wc.width = 1124;
    wc.height = 280;
    wc.title = "widgets";
    auto window = app.create_window(wc);
    if (!window) {
        return 1;
    }

    auto scene = velk::create_scene("app://widgets.velk");
    auto camera = scene.find_first<velk::ICamera>();
    if (!camera) {
        VELK_LOG(E, "widgets: cannot load widgets.velk");
        return 1;
    }
    app.add_view(window, camera);

    auto fields = scene.find_elements<velk::ui::ITextField>();
    // The first text that is not a field is the status line.
    velk::Element status;
    for (auto& element : scene.find_elements<velk::ui::ITextVisual>()) {
        if (!element.find_attachment<velk::ui::ITextField>()) {
            status = element;
            break;
        }
    }
    auto scroll = scene.find_first<velk::ui::IScrollView>();
    auto tree_element = scene.find_first<velk::ui::ITreeView>();
    auto tree_source = make_tree();
    auto tree = tree_element.find_attachment<velk::ui::ITreeView>();
    velk::write_state<velk::ui::ITreeView>(tree, [&](velk::ui::ITreeView::State& s) {
        velk::set_object_ref(s.source, tree_source.as_ptr<velk::IObject>());
    });
    // The list: 10,000 items, rows only for those that show.
    auto items = velk::create_object_list();
    {
        velk::vector<velk::IObject::Ptr> all;
        for (int i = 0; i < 10000; ++i) {
            char name[24];
            const int n = std::snprintf(name, sizeof(name), "Item %d", i);
            all.push_back(node(velk::string_view(name, static_cast<size_t>(n))));
        }
        items->insert(0, {all.data(), all.size()});
    }
    auto items_element = scene.find_first<velk::ui::IListView>();
    auto items_view = items_element.find_attachment<velk::ui::IListView>();
    velk::write_state<velk::ui::IListView>(items_view, [&](velk::ui::IListView::State& s) {
        velk::set_object_ref(s.source, items);
    });
    velk::vector<velk::ScopedHandler> subs;
    for (auto& element : fields) {
        auto field = element.find_attachment<velk::ui::ITextField>();
        subs.emplace_back(field->on_commit(), [status](const velk::string& text) {
            velk::ui::TextVisual(status.find_attachment<velk::ui::ITextVisual>())
                .set_text(velk::string("Committed: ") + text);
        });
    }

    auto* input = window.input();
    int frame = 0;
    int result = 0;
    while (app.poll()) {
        if (script && input && fields.size() == 2) {
            // From widgets.velk: 24px padding; fields 260x32, 12px apart; the scroll view
            // 300x220, 24px right of them; the tree's view 220x220 at x 632, rows 22px, 16px
            // indent per level; the list's view 220x220 at x 876, rows 22px.
            if (frame == 5) {
                click(*input, 270.f, 40.f); // end of the first field
                type(*input, " settings");
                press(*input, Key::Left, 3);
                type(*input, "X");
            } else if (frame == 10) {
                click(*input, 30.f, 84.f); // the second field; commits the first
                type(*input, "C:/my/code/github/editor/velk-ui/samples/widgets/main.cpp");
                press(*input, Key::Left, 4);
            } else if (frame == 12) {
                wheel(*input, 450.f, 120.f, -1.3f);       // down 52px: the top row is cut
                wheel(*input, 450.f, 120.f, -1.f, true);  // right 40px
                click(*input, 632.f + 16.f + 4.f, 24.f + 3 * 22.f + 11.f);  // + of Panels
            } else if (frame == 14) {
                click(*input, 632.f + 32.f + 4.f, 24.f + 5 * 22.f + 11.f);  // + of Body
            } else if (frame == 16) {
                click(*input, 632.f + 120.f, 24.f + 7 * 22.f + 11.f);       // select Text
                wheel(*input, 700.f, 120.f, -0.8f);                         // down 32px
                wheel(*input, 980.f, 120.f, -125.f);                        // list down 5000px
            } else if (frame == 18) {
                click(*input, 980.f, 24.f + 77.f); // 5077px into the list: Item 230
            } else if (frame == 20) {
                if (!save_capture(app, window, script)) {
                    VELK_LOG(E, "widgets: capture failed");
                    result = 1;
                }
                auto sv = velk::read_state<velk::ui::IScrollView>(scroll.find_attachment<velk::ui::IScrollView>());
                auto ts = velk::read_state<velk::ui::ITreeView>(tree);
                auto picked = ts ? ts->selected.get<velk::IObject>() : nullptr;
                velk::string picked_label;
                if (auto* meta = interface_cast<velk::IMetadata>(picked)) {
                    picked_label = velk::ConstProperty<velk::string>(meta->get_property("label")).get_value();
                }
                auto ls = velk::read_state<velk::ui::IListView>(items_view);
                auto item = ls ? ls->selected.get<velk::IObject>() : nullptr;
                velk::string item_label;
                if (auto* meta = interface_cast<velk::IMetadata>(item)) {
                    item_label = velk::ConstProperty<velk::string>(meta->get_property("label")).get_value();
                }
                auto ivs = velk::read_state<velk::ui::IScrollView>(
                    scene.find_elements<velk::ui::IScrollView>().back().find_attachment<velk::ui::IScrollView>());
                size_t rows = 0;
                if (auto* h = interface_cast<velk::IHierarchy>(scene.as_ptr<velk::IScene>().get())) {
                    rows = h->child_count(velk::as_object(items_element.as_ptr<velk::IElement>()));
                }
                VELK_LOG(I, "widgets: first \"%s\", second (not committed) \"%s\", status \"%s\", scroll %.0f,%.0f, tree selected \"%s\", list scroll %.0f, %zu rows, list selected \"%s\"",
                         text_of(fields[0]).c_str(), text_of(fields[1]).c_str(), text_of(status).c_str(),
                         sv ? sv->scroll_x : -1.f, sv ? sv->scroll_y : -1.f, picked_label.c_str(),
                         ivs ? ivs->scroll_y : -1.f, rows, item_label.c_str());
                break;
            }
        }
        app.update();
        app.present();
        ++frame;
    }
    return result;
}
