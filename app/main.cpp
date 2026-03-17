#include <velk/api/store.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_plugin_registry.h>
#include <velk/plugins/importer/api/importer.h>

#include <velk-ui/interface/intf_renderer.h>
#include <velk-ui/plugin.h>
#include <velk-ui/plugins/gl/plugin.h>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::string read_file(const char* path)
{
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string find_scene_path(const char* exe_path)
{
    std::string base(exe_path);
    for (auto& c : base) {
        if (c == '\\') c = '/';
    }
    auto pos = base.rfind('/');
    if (pos != std::string::npos) {
        base = base.substr(0, pos);
    }

    const char* candidates[] = {
        "/scenes/hello.json",
        "/../scenes/hello.json",
        "/../../scenes/hello.json",
        "/../../../scenes/hello.json",
        "/../../../../scenes/hello.json",
    };
    for (auto* suffix : candidates) {
        std::string path = base + suffix;
        std::ifstream test(path);
        if (test.good()) return path;
    }
    return "scenes/hello.json";
}

static void glfw_error_callback(int error, const char* description)
{
    VELK_LOG(E, "GLFW error %d: %s", error, description);
}

int main(int argc, char* argv[])
{
    glfwSetErrorCallback(glfw_error_callback);

    if (!glfwInit()) {
        VELK_LOG(E, "Failed to initialize GLFW");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;

    GLFWwindow* window = glfwCreateWindow(kWidth, kHeight, "velk-ui", nullptr, nullptr);
    if (!window) {
        VELK_LOG(E, "Failed to create GLFW window");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);

    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        VELK_LOG(E, "Failed to load OpenGL via GLAD2");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    VELK_LOG(I, "OpenGL %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    auto& velk = velk::instance();

    // Load plugins
    auto rv = velk.plugin_registry().load_plugin_from_path("velk_ui.dll");
    if (rv != velk::ReturnValue::Success) {
        VELK_LOG(E, "Failed to load velk_ui plugin");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    rv = velk.plugin_registry().load_plugin_from_path("velk_gl.dll");
    if (rv != velk::ReturnValue::Success) {
        VELK_LOG(E, "Failed to load velk_gl plugin");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // Create and init renderer
    auto renderer_obj = velk.create<velk::IObject>(velk_ui::ClassId::GlRenderer);
    if (!renderer_obj) {
        VELK_LOG(E, "Failed to create GlRenderer");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    auto* renderer = velk::interface_cast<velk_ui::IRenderer>(renderer_obj);
    if (!renderer || !renderer->init(kWidth, kHeight)) {
        VELK_LOG(E, "Failed to initialize renderer");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // Import scene (velk_ui plugin must be loaded first so Element type is known)
    std::string scene_path = find_scene_path(argv[0]);
    std::string json = read_file(scene_path.c_str());
    if (json.empty()) {
        VELK_LOG(E, "Failed to read scene: %s", scene_path.c_str());
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    velk.plugin_registry().load_plugin_from_path("velk_importer.dll");
    auto importer = velk::create_json_importer();
    if (!importer) {
        VELK_LOG(E, "Failed to create JSON importer");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    auto import_result = importer.import_from(velk::string_view(json.c_str(), json.size()));
    auto store = velk::Store(import_result.store);

    if (!import_result.errors.empty()) {
        for (auto& err : import_result.errors) {
            VELK_LOG(W, "Import: %s", err.c_str());
        }
    }

    VELK_LOG(I, "Imported %zu objects from %s", store.object_count(), scene_path.c_str());

    // Register elements with the renderer
    std::vector<velk_ui::IRenderer::VisualId> visual_ids;
    for (size_t i = 0; i < store.object_count(); ++i) {
        auto obj = store.object_at(i);
        if (obj) {
            auto vid = renderer->add_visual(obj);
            visual_ids.push_back(vid);
        }
    }

    VELK_LOG(I, "Registered %zu visuals", visual_ids.size());

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        velk.update();
        renderer->render();
        glfwSwapBuffers(window);
    }

    renderer->shutdown();
    renderer_obj = nullptr;

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
