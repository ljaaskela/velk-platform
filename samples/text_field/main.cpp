// velk_ui_text_field [script=<capture.png>]
//
// Two text fields. Click one and type; Enter commits, Escape cancels, clicking elsewhere
// commits. The line below them shows the last committed value.
//
// script=<capture.png> feeds clicks and keys instead, saves a capture while the second field
// is being edited, prints the field values and exits.

#include <velk/api/event.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>

#include <velk-render/interface/intf_camera.h>
#include <velk-render/interface/intf_image.h>
#include <velk-runtime/api/application.h>
#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
#include <velk-ui/interface/intf_input_dispatcher.h>
#include <velk-ui/plugins/image/plugin.h>
#include <velk-ui/plugins/text/api/text_visual.h>
#include <velk-ui/plugins/text/intf_text_field.h>

#include <cstring>

namespace {

using velk::ui::Key;
using velk::ui::KeyAction;
using velk::ui::KeyEvent;
using velk::ui::PointerAction;
using velk::ui::PointerButton;
using velk::ui::PointerEvent;

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
    wc.width = 520;
    wc.height = 160;
    wc.title = "text field";
    auto window = app.create_window(wc);
    if (!window) {
        return 1;
    }

    auto scene = velk::create_scene("app://text_field.velk");
    auto camera = scene.find_first<velk::ICamera>();
    if (!camera) {
        VELK_LOG(E, "text_field: cannot load text_field.velk");
        return 1;
    }
    app.add_view(window, camera);

    auto fields = scene.find_elements<velk::ui::ITextField>();
    auto status = scene.find_first<velk::ui::ITextVisual>();
    for (auto& element : scene.find_elements<velk::ui::ITextVisual>()) {
        if (!element.find_attachment<velk::ui::ITextField>()) {
            status = element;
        }
    }
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
            // Field rectangles from text_field.velk: 24px padding, 260x32, 12px apart.
            if (frame == 5) {
                click(*input, 270.f, 40.f); // end of the first field
                type(*input, " settings");
                press(*input, Key::Left, 3);
                type(*input, "X");
            } else if (frame == 10) {
                click(*input, 30.f, 84.f); // the second field; commits the first
                type(*input, "C:/my/code/github/editor/velk-ui/samples/text_field/main.cpp");
                press(*input, Key::Left, 4);
            } else if (frame == 20) {
                if (!save_capture(app, window, script)) {
                    VELK_LOG(E, "text_field: capture failed");
                    result = 1;
                }
                VELK_LOG(I, "text_field: first \"%s\", second (not committed) \"%s\", status \"%s\"",
                         text_of(fields[0]).c_str(), text_of(fields[1]).c_str(), text_of(status).c_str());
                break;
            }
        }
        app.update();
        app.present();
        ++frame;
    }
    return result;
}
