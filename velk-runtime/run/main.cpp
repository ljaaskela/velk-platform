// velk_run [folder]
//
// Runs the application described by <folder>/app.velk (the current directory by default):
// loads its plugins, opens its window and shows its store file's first camera.

#include <velk/api/velk.h>

#include <velk-render/interface/intf_camera.h>
#include <velk-runtime/api/application.h>
#include <velk-scene/api/scene.h>

int main(int argc, char* argv[])
{
    velk::string path(argc > 1 ? argv[1] : ".");
    if (path.back() != '/' && path.back() != '\\') {
        path.append("/");
    }
    path.append("app.velk");

    auto file = velk::read_app_config(path);
    if (!file.error.empty()) {
        VELK_LOG(E, "%s", file.error.c_str());
        return 1;
    }
    auto app = velk::create_app(file.app);
    if (!app) {
        return 1;
    }

    auto window = app.create_window(file.window);
    if (!window) {
        VELK_LOG(E, "velk_run: cannot create the window");
        return 1;
    }

    auto scene = velk::create_scene();
    auto loaded = scene.load_from(velk::string("file://") + file.store).get_result().get_value();
    if (failed(loaded)) {
        VELK_LOG(E, "velk_run: cannot load %s", file.store.c_str());
        return 1;
    }
    auto camera = scene.find_first<velk::ICamera>();
    if (!camera) {
        VELK_LOG(E, "velk_run: %s has no camera", file.store.c_str());
        return 1;
    }
    app.add_view(window, camera);

    while (app.poll()) {
        app.update();
        app.present();
    }
    return 0;
}
