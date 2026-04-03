#include <velk/api/object_ref.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_plugin_registry.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <velk-ui/api/input/click.h>
#include <velk-ui/api/input_dispatcher.h>
#include <velk-ui/api/material/gradient.h>
#include <velk-ui/api/material/shader.h>
#include <velk-ui/api/render_context.h>
#include <velk-ui/api/scene.h>
#include <velk-ui/api/visual/rect.h>
#include <velk-ui/plugins/vk/plugin.h>

static void glfw_error_callback(int error, const char* description)
{
    VELK_LOG(E, "GLFW error %d: %s", error, description);
}

static velk_ui::Scene* g_scene = nullptr;
static velk_ui::ISurface::Ptr g_surface;
static velk_ui::InputDispatcher* g_input = nullptr;

static void glfw_framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    if (g_scene) {
        g_scene->set_geometry(velk::aabb::from_size({static_cast<float>(width), static_cast<float>(height)}));
    }
    if (g_surface) {
        velk::write_state<velk_ui::ISurface>(g_surface, [&](velk_ui::ISurface::State& s) {
            s.width = width;
            s.height = height;
        });
    }
}

int main(int argc, char* argv[])
{
    glfwSetErrorCallback(glfw_error_callback);

    if (!glfwInit()) {
        VELK_LOG(E, "Failed to initialize GLFW");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;

    GLFWwindow* window = glfwCreateWindow(kWidth, kHeight, "velk-ui", nullptr, nullptr);
    if (!window) {
        VELK_LOG(E, "Failed to create GLFW window");
        glfwTerminate();
        return 1;
    }

    auto& velk = velk::instance();

    // Load plugins
    velk.plugin_registry().load_plugin_from_path("velk_ui.dll");
    velk.plugin_registry().load_plugin_from_path("velk_render.dll");
    velk.plugin_registry().load_plugin_from_path("velk_vk.dll");
    velk.plugin_registry().load_plugin_from_path("velk_text.dll");
    velk.plugin_registry().load_plugin_from_path("velk_importer.dll");

    // Create render context, renderer, and surface
    static velk_ui::VulkanInitParams vk_params;
    vk_params.user_data = window;
    vk_params.create_surface = [](void* vk_instance, void* out_surface, void* user_data) -> bool {
        auto instance = static_cast<VkInstance>(vk_instance);
        auto* surface = static_cast<VkSurfaceKHR*>(out_surface);
        auto* win = static_cast<GLFWwindow*>(user_data);
        return glfwCreateWindowSurface(instance, win, nullptr, surface) == VK_SUCCESS;
    };

    velk_ui::RenderConfig render_config;
    render_config.backend_params = &vk_params;

    auto ctx = velk_ui::create_render_context(render_config);
    if (!ctx) {
        VELK_LOG(E, "Failed to create render context");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    auto renderer = ctx.create_renderer();
    auto surface = ctx.create_surface(kWidth, kHeight);

    // Load scene
    auto scene = velk_ui::create_scene("app://scenes/dashboard.json");
    scene.set_geometry(velk::aabb::from_size({static_cast<float>(kWidth), static_cast<float>(kHeight)}));

    renderer->attach(surface, scene);

    g_scene = &scene;
    g_surface = surface;
    glfwSetFramebufferSizeCallback(window, glfw_framebuffer_size_callback);

    // Input dispatcher
    auto input = velk_ui::create_input_dispatcher(scene);
    g_input = &input;

    glfwSetCursorPosCallback(window, [](GLFWwindow*, double x, double y) {
        if (g_input) {
            velk_ui::PointerEvent ev;
            ev.position = {static_cast<float>(x), static_cast<float>(y)};
            ev.action = velk_ui::PointerAction::Move;
            g_input->pointer_event(ev);
        }
    });

    glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int button, int action, int mods) {
        if (!g_input) {
            return;
        }
        velk_ui::PointerEvent ev;
        double mx, my;
        glfwGetCursorPos(w, &mx, &my);
        ev.position = {static_cast<float>(mx), static_cast<float>(my)};
        ev.action = (action == GLFW_PRESS) ? velk_ui::PointerAction::Down : velk_ui::PointerAction::Up;
        ev.button = (button == GLFW_MOUSE_BUTTON_LEFT)    ? velk_ui::PointerButton::Left
                    : (button == GLFW_MOUSE_BUTTON_RIGHT) ? velk_ui::PointerButton::Right
                                                          : velk_ui::PointerButton::Middle;
        if (mods & GLFW_MOD_SHIFT) ev.modifiers = ev.modifiers | velk_ui::Modifier::Shift;
        if (mods & GLFW_MOD_CONTROL) ev.modifiers = ev.modifiers | velk_ui::Modifier::Ctrl;
        if (mods & GLFW_MOD_ALT) ev.modifiers = ev.modifiers | velk_ui::Modifier::Alt;
        if (mods & GLFW_MOD_SUPER) ev.modifiers = ev.modifiers | velk_ui::Modifier::Super;
        g_input->pointer_event(ev);
    });

    glfwSetScrollCallback(window, [](GLFWwindow* w, double xoffset, double yoffset) {
        if (!g_input) {
            return;
        }
        velk_ui::ScrollEvent ev;
        double mx, my;
        glfwGetCursorPos(w, &mx, &my);
        ev.position = {static_cast<float>(mx), static_cast<float>(my)};
        ev.delta = {static_cast<float>(xoffset), static_cast<float>(yoffset)};
        ev.unit = velk_ui::ScrollUnit::Lines;
        g_input->scroll_event(ev);
    });

    // Add gradient background to the root element
    {
        auto root = scene.root();
        auto bg = velk_ui::visual::create_rect();
        bg.set_color(velk::color::red());
        bg.set_paint(velk_ui::material::create_gradient(
            velk::color{0.05f, 0.07f, 0.15f, 1.f}, velk::color{0.18f, 0.12f, 0.28f, 1.f}, 90.f));

        root.add_trait(bg);
        auto click = velk_ui::input::create_click();
        click.on_click().add_handler([]() { VELK_LOG(E, "Clicked!"); });
        root.add_trait(click);
    }

    // First frame
    velk.update();
    renderer->render();

    // Print stats after first frame
    {
        auto stats = velk.get_stats();
        VELK_LOG(I, "Plugins (%zu):", stats.plugins.size());
        for (auto& p : stats.plugins) {
            VELK_LOG(I,
                     "  %.*s v%d.%d.%d [update=%s]",
                     static_cast<int>(p.plugin_name.size()),
                     p.plugin_name.data(),
                     velk::version_major(p.version),
                     velk::version_minor(p.version),
                     velk::version_patch(p.version),
                     p.update_enabled ? "on" : "off");
        }
        VELK_LOG(I, "Types (total: %zu, showing ones with live instances):", stats.types.size());
        for (auto& t : stats.types) {
            if (t.factory && (t.instance_count || t.policy != velk::CreationPolicy::Hive)) {
                auto& info = t.factory->get_class_info();
                VELK_LOG(I,
                         "  %s %.*s: %zu (size: %zu)",
                         t.policy == velk::CreationPolicy::Hive    ? "[hive] "
                         : t.policy == velk::CreationPolicy::Alloc ? "[alloc]"
                                                                   : "[auto] ",
                         static_cast<int>(info.name.size()),
                         info.name.data(),
                         t.instance_count,
                         t.factory->get_instance_size());
            }
        }
    }

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        velk.update();
        renderer->render();
    }

    g_input = nullptr;
    g_scene = nullptr;
    g_surface = nullptr;

    scene = velk_ui::Scene{};
    renderer->shutdown();
    renderer = nullptr;

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
