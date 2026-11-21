#include <velk/api/velk.h>

#include <velk-render/interface/intf_camera.h>
#include <velk-runtime/api/application.h>
#include <velk-scene/api/scene.h>

int main(int, char*[])
{
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

    auto ui = velk::create_scene("app://ui/editor.json");
    auto camera = ui.find_first<velk::ICamera>();
    if (!camera) {
        VELK_LOG(E, "velk-editor: ui/editor.json has no camera");
        return 1;
    }
    app.add_view(window, camera);

    while (app.poll()) {
        app.update();
        app.present();
    }
    return 0;
}
