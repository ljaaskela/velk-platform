#include <velk/api/object_ref.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_plugin_registry.h>

#include <GLFW/glfw3.h>
#include <velk-ui/api/constraint/fixed_size.h>
#include <velk-ui/api/element.h>
#include <velk-ui/api/material/shader.h>
#include <velk-ui/api/render_context.h>
#include <velk-ui/api/scene.h>
#include <velk-ui/api/visual/rect.h>
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/api/text_visual.h>

static void glfw_error_callback(int error, const char* description)
{
    VELK_LOG(E, "GLFW error %d: %s", error, description);
}

static velk_ui::Scene* g_scene = nullptr;
static velk_ui::ISurface::Ptr g_surface;

static void glfw_framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    if (g_scene) {
        g_scene->set_geometry(velk::aabb::from_size({
            static_cast<float>(width), static_cast<float>(height)}));
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
    glfwSwapInterval(1);

    auto& velk = velk::instance();

    // Load plugins
    velk.plugin_registry().load_plugin_from_path("velk_ui.dll");
    velk.plugin_registry().load_plugin_from_path("velk_render.dll");
    velk.plugin_registry().load_plugin_from_path("velk_gl.dll");
    velk.plugin_registry().load_plugin_from_path("velk_text.dll");
    velk.plugin_registry().load_plugin_from_path("velk_importer.dll");

    // Create render context, renderer, and surface
    auto ctx = velk_ui::create_render_context(
        {velk_ui::RenderBackendType::GL, reinterpret_cast<void*>(glfwGetProcAddress)});
    if (!ctx) {
        VELK_LOG(E, "Failed to create render context");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    auto renderer = ctx.create_renderer();
    auto surface = ctx.create_surface(kWidth, kHeight);

    // Load scene
    auto scene = velk_ui::create_scene("app://scenes/stack_test.json");
    scene.set_geometry(velk::aabb::from_size({
        static_cast<float>(kWidth), static_cast<float>(kHeight)}));

    renderer->attach(surface, scene);

    g_scene = &scene;
    g_surface = surface;
    glfwSetFramebufferSizeCallback(window, glfw_framebuffer_size_callback);

    auto root = scene.root();
    auto child3 = scene.child_at(root, 2);
    // Apply a gradient shader to the blue rect (child3)
    {
        auto visual = child3.find_trait<velk_ui::IVisual>();
        if (visual) {
            auto mat = velk_ui::material::create_shader();
            mat.set_fragment_source(R"(
                #version 330 core
                in vec4 v_color;
                out vec4 frag_color;
                uniform vec4 u_rect;
                void main()
                {
                    float t = (gl_FragCoord.x - u_rect.x) / max(u_rect.z, 1.0);
                    vec3 top = vec3(0.1, 0.4, 0.9);
                    vec3 bot = vec3(0.9, 0.2, 0.6);
                    frag_color = vec4(mix(top, bot, t), 1.0);
                }
            )");
            velk::write_state<velk_ui::IVisual>(
                visual, [&](velk_ui::IVisual::State& s) { s.paint = velk::create_object_ref(mat); });
            VELK_LOG(I, "Paint set on child2, mat=%p", static_cast<void*>(mat.get().get()));
        }
    }

    // Programmatically create a text element with "Hello, Velk!"
    {
        auto font = velk_ui::create_font();
        if (font.init_default() && font.set_size(32.f)) {
            auto tv = velk_ui::visual::create_text();
            tv.set_font(font);
            tv.set_text("Hello, Velk!");
            tv.set_color(velk::color::white());

            child3.add_trait(tv);
        }
    }

    // First frame
    velk.update();
    renderer->render();
    glfwSwapBuffers(window);

    // Print stats after first frame
    {
        auto stats = velk.get_stats();
        VELK_LOG(I, "Plugins (%zu):", stats.plugins.size());
        for (auto& p : stats.plugins) {
            VELK_LOG(I, "  %.*s v%d.%d.%d [update=%s]",
                     static_cast<int>(p.plugin_name.size()), p.plugin_name.data(),
                     velk::version_major(p.version), velk::version_minor(p.version),
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
        glfwSwapBuffers(window);
    }

    g_scene = nullptr;
    g_surface = nullptr;

    scene = velk_ui::Scene{};
    renderer->shutdown();
    renderer = nullptr;

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
