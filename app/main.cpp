#include <velk/api/velk.h>
#include <velk/interface/intf_plugin_registry.h>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <velk-ui/api/constraint/fixed_size.h>
#include <velk-ui/api/element.h>
#include <velk-ui/api/scene.h>
#include <velk-ui/plugins/gl/plugin.h>
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/api/text_visual.h>

static void glfw_error_callback(int error, const char* description)
{
    VELK_LOG(E, "GLFW error %d: %s", error, description);
}

static void glfw_framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    auto* renderer = static_cast<velk_ui::IRenderer*>(glfwGetWindowUserPointer(window));
    if (renderer) {
        velk::write_state<velk_ui::IRenderer>(renderer, [&](velk_ui::IRenderer::State& s) {
            s.viewport_width = static_cast<uint32_t>(width);
            s.viewport_height = static_cast<uint32_t>(height);
        });
    }
    glViewport(0, 0, width, height);
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
    auto renderer = velk.create<velk_ui::IRenderer>(velk_ui::ClassId::GlRenderer);
    if (!renderer || !renderer->init(kWidth, kHeight)) {
        VELK_LOG(E, "Failed to initialize renderer");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // Load scene
    auto scene = velk_ui::create_scene("app://scenes/stack_test.json");
    scene.set_renderer(renderer);

    glfwSetWindowUserPointer(window, renderer.get());
    glfwSetFramebufferSizeCallback(window, glfw_framebuffer_size_callback);

    // Programmatically create a text element with "Hello, Velk!"
    {
        auto font = velk_ui::create_font();
        if (font.init_default() && font.set_size(32.f)) {
            auto tv = velk_ui::visual::create_text();
            tv.set_font(font);
            tv.set_text("Hello, Velk!");
            tv.set_color(velk::color::white());

            auto text_elem = velk_ui::create_element();

            auto fs = velk_ui::constraint::create_fixed_size();
            fs.set_size(velk_ui::dim::px(400.f), velk_ui::dim::px(50.f));

            text_elem.add_trait(fs);
            text_elem.add_trait(tv);

            scene.add(scene.root(), text_elem);

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

    scene = velk_ui::Scene{};
    renderer->shutdown();
    renderer = nullptr;

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
