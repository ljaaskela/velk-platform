#include <velk/api/state.h>
#include <velk/api/store.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_object_storage.h>
#include <velk/interface/intf_plugin_registry.h>
#include <velk/plugins/importer/api/importer.h>

#include <fstream>
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <sstream>
#include <string>
#include <velk-ui/interface/constraint/intf_fixed_size.h>
#include <velk-ui/interface/intf_font.h>
#include <velk-ui/interface/intf_renderer.h>
#include <velk-ui/interface/intf_scene.h>
#include <velk-ui/interface/intf_visual.h>
#include <velk-ui/plugin.h>
#include <velk-ui/plugins/gl/plugin.h>
#include <velk-ui/plugins/text/intf_text_visual.h>
#include <velk-ui/plugins/text/plugin.h>

static std::string read_file(const char* path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string find_scene_path(const char* exe_path, const char* filename)
{
    std::string base(exe_path);
    for (auto& c : base) {
        if (c == '\\') {
            c = '/';
        }
    }
    auto pos = base.rfind('/');
    if (pos != std::string::npos) {
        base = base.substr(0, pos);
    }

    const char* prefixes[] = {
        "/scenes/",
        "/../scenes/",
        "/../../scenes/",
        "/../../../scenes/",
        "/../../../../scenes/",
    };
    for (auto* prefix : prefixes) {
        std::string path = base + prefix + filename;
        std::ifstream test(path);
        if (test.good()) {
            return path;
        }
    }
    return std::string("scenes/") + filename;
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
    velk.plugin_registry().load_plugin_from_path("velk_ui.dll");
    velk.plugin_registry().load_plugin_from_path("velk_gl.dll");
    velk.plugin_registry().load_plugin_from_path("velk_text.dll");
    velk.plugin_registry().load_plugin_from_path("velk_importer.dll");

    // Create and init renderer
    auto renderer_obj = velk.create<velk::IObject>(velk_ui::ClassId::GlRenderer);
    auto* renderer = velk::interface_cast<velk_ui::IRenderer>(renderer_obj);
    if (!renderer || !renderer->init(kWidth, kHeight)) {
        VELK_LOG(E, "Failed to initialize renderer");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // Import scene
    std::string scene_path = find_scene_path(argv[0], "stack_test.json");
    std::string json = read_file(scene_path.c_str());
    if (json.empty()) {
        VELK_LOG(E, "Failed to read scene: %s", scene_path.c_str());
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    auto importer = velk::create_json_importer();
    auto import_result = importer.import_from(velk::string_view(json.c_str(), json.size()));
    auto store = velk::Store(import_result.store);

    VELK_LOG(I, "Imported %zu objects from %s", store.object_count(), scene_path.c_str());

    // Create scene, load from store, wire up renderer and viewport
    auto scene_obj = velk.create<velk::IObject>(velk_ui::ClassId::Scene);
    auto* scene = velk::interface_cast<velk_ui::IScene>(scene_obj);

    scene->set_renderer(renderer);
    scene->set_viewport({{}, {static_cast<float>(kWidth), static_cast<float>(kHeight)}});
    scene->load(*import_result.store);

    // Programmatically create a text element with "Hello, Velk!"
    velk::IObject::Ptr font_obj;
    {
        font_obj = velk.create<velk::IObject>(velk_ui::ClassId::Font);
        auto* font = velk::interface_cast<velk_ui::IFont>(font_obj);
        if (font && font->init_default() && font->set_size(32.f)) {
            // Create TextVisual, set color to white, shape text
            auto tv_obj = velk.create<velk::IObject>(velk_ui::ClassId::Visual::Text);
            velk::write_state<velk_ui::IVisual>(
                tv_obj, [](velk_ui::IVisual::State& s) { s.color = velk::color::white(); });

            auto* tv = velk::interface_cast<velk_ui::ITextVisual>(tv_obj);
            if (tv) {
                tv->set_text("Hello, Velk!", *font);
            }

            // Create element for the text
            auto text_elem = velk.create<velk::IObject>(velk_ui::ClassId::Element);

            // Attach TextVisual to element
            auto* storage = velk::interface_cast<velk::IObjectStorage>(text_elem);
            if (storage) {
                auto att = velk::interface_pointer_cast<velk::IInterface>(tv_obj);
                storage->add_attachment(att);

                // Give the element a fixed size
                auto fs_obj = velk.create<velk::IObject>(velk_ui::ClassId::Constraint::FixedSize);
                velk::write_state<velk_ui::IFixedSize>(fs_obj, [](velk_ui::IFixedSize::State& s) {
                    s.width = velk_ui::dim::px(400.f);
                    s.height = velk_ui::dim::px(50.f);
                });
                auto fs_att = velk::interface_pointer_cast<velk::IInterface>(fs_obj);
                storage->add_attachment(fs_att);
            }

            // Add text element to scene hierarchy (as child of root)
            auto root = scene->root();
            if (root) {
                scene->add(root, text_elem);
            }

            VELK_LOG(I, "Text element added: \"Hello, Velk!\"");
        }
    }

    // Main loop: velk.update() drives scene layout via plugin post_update
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        velk.update();
        renderer->render();
        glfwSwapBuffers(window);
    }

    scene_obj = nullptr;
    font_obj = nullptr;
    renderer->shutdown();
    renderer_obj = nullptr;

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
