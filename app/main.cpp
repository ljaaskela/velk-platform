#include <velk/api/object_ref.h>
#include <velk/api/velk.h>

#include <velk/interface/intf_plugin_registry.h>

// clang-format off
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
// clang-format on
#include <velk-render/api/material/shader.h>
#include <velk-render/api/render_context.h>
#include <velk-render/plugins/vk/plugin.h>
#include <velk-ui/api/element.h>
#include <velk-ui/api/input/click.h>
#include <velk-ui/api/input_dispatcher.h>
#include <velk-ui/api/material/gradient.h>
#include <velk-ui/api/renderer.h>
#include <velk-ui/api/scene.h>
#include <velk-ui/api/visual/rect.h>
#include <velk-ui/api/visual/visual.h>
#include <velk-ui/api/trait/orbit.h>
#include <velk-ui/interface/intf_camera.h>

static void glfw_error_callback(int error, const char* description)
{
    VELK_LOG(E, "GLFW error %d: %s", error, description);
}

static velk::ui::Scene* g_scene = nullptr;
static velk::ISurface::Ptr g_surface;
static velk::ui::InputDispatcher* g_input = nullptr;
static velk::ui::OrbitTrait g_orbit;
static bool g_orbit_dragging = false;
static double g_orbit_last_x = 0.0;
static double g_orbit_last_y = 0.0;

static void glfw_framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    if (g_scene) {
        g_scene->set_geometry(velk::aabb::from_size({static_cast<float>(width), static_cast<float>(height)}));
    }
    if (g_surface) {
        velk::write_state<velk::ISurface>(g_surface, [&](velk::ISurface::State& s) {
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
    velk.plugin_registry().load_plugin_from_path("velk_image.dll");
    velk.plugin_registry().load_plugin_from_path("velk_importer.dll");

    // Create render context, renderer, and surface
    static velk::vk::VulkanInitParams vk_params;
    vk_params.user_data = window;
    vk_params.create_surface = [](void* vk_instance, void* out_surface, void* user_data) -> bool {
        auto instance = static_cast<VkInstance>(vk_instance);
        auto* surface = static_cast<VkSurfaceKHR*>(out_surface);
        auto* win = static_cast<GLFWwindow*>(user_data);
        return glfwCreateWindowSurface(instance, win, nullptr, surface) == VK_SUCCESS;
    };

    velk::RenderConfig render_config;
    render_config.backend_params = &vk_params;

    auto ctx = velk::create_render_context(render_config);
    if (!ctx) {
        VELK_LOG(E, "Failed to create render context");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    auto surface = ctx.create_surface(kWidth, kHeight);
    velk::IRenderContext::Ptr render_ctx = ctx;
    auto renderer = velk::ui::create_renderer(*render_ctx);

    // Load scene
    auto scene = velk::ui::create_scene("app://scenes/dashboard.json");
    scene.set_geometry(velk::aabb::from_size({static_cast<float>(kWidth), static_cast<float>(kHeight)}));

    auto camera = scene.child_at(scene.root(), 1);      // Ortho camera
    auto camera_3d = scene.child_at(scene.root(), 2);  // Perspective camera

    if (camera) {
        renderer->add_view(camera, surface, {0, 0, 0.5f, 1.0f});
    }
    if (camera_3d) {
        renderer->add_view(camera_3d, surface, {0.f, 0, 1.f, 1.0f});
        // Grab the orbit trait for mouse-driven rotation.
        g_orbit = velk::ui::OrbitTrait(camera_3d.find_trait<velk::ui::IOrbit>());
    }

    g_scene = &scene;
    g_surface = surface;
    glfwSetFramebufferSizeCallback(window, glfw_framebuffer_size_callback);

    // Input dispatcher
    auto input = velk::ui::create_input_dispatcher(scene);
    g_input = &input;

    glfwSetCursorPosCallback(window, [](GLFWwindow*, double x, double y) {
        // Orbit: right-drag rotates the 3D camera.
        if (g_orbit_dragging && g_orbit) {
            float dx = static_cast<float>(x - g_orbit_last_x);
            float dy = static_cast<float>(y - g_orbit_last_y);
            auto state = velk::read_state<velk::ui::IOrbit>(static_cast<velk::ui::IOrbit::Ptr>(g_orbit));
            if (state) {
                g_orbit.set_yaw(state->yaw + dx * 0.3f);
                g_orbit.set_pitch(state->pitch + dy * 0.3f);
            }
            g_orbit_last_x = x;
            g_orbit_last_y = y;
        }

        if (g_input) {
            velk::ui::PointerEvent ev;
            ev.position = {static_cast<float>(x), static_cast<float>(y)};
            ev.action = velk::ui::PointerAction::Move;
            g_input->pointer_event(ev);
        }
    });

    glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int button, int action, int mods) {
        // Orbit: right mouse button starts/stops drag rotation.
        if (button == GLFW_MOUSE_BUTTON_RIGHT && g_orbit) {
            if (action == GLFW_PRESS) {
                g_orbit_dragging = true;
                glfwGetCursorPos(w, &g_orbit_last_x, &g_orbit_last_y);
            } else {
                g_orbit_dragging = false;
            }
        }

        if (!g_input) {
            return;
        }
        velk::ui::PointerEvent ev;
        double mx, my;
        glfwGetCursorPos(w, &mx, &my);
        ev.position = {static_cast<float>(mx), static_cast<float>(my)};
        ev.action = (action == GLFW_PRESS) ? velk::ui::PointerAction::Down : velk::ui::PointerAction::Up;
        ev.button = (button == GLFW_MOUSE_BUTTON_LEFT)    ? velk::ui::PointerButton::Left
                    : (button == GLFW_MOUSE_BUTTON_RIGHT) ? velk::ui::PointerButton::Right
                                                          : velk::ui::PointerButton::Middle;
        if (mods & GLFW_MOD_SHIFT) {
            ev.modifiers = ev.modifiers | velk::ui::Modifier::Shift;
        }
        if (mods & GLFW_MOD_CONTROL) {
            ev.modifiers = ev.modifiers | velk::ui::Modifier::Ctrl;
        }
        if (mods & GLFW_MOD_ALT) {
            ev.modifiers = ev.modifiers | velk::ui::Modifier::Alt;
        }
        if (mods & GLFW_MOD_SUPER) {
            ev.modifiers = ev.modifiers | velk::ui::Modifier::Super;
        }
        g_input->pointer_event(ev);
    });

    glfwSetScrollCallback(window, [](GLFWwindow* w, double xoffset, double yoffset) {
        // Orbit: scroll wheel adjusts camera distance.
        if (g_orbit) {
            auto state = velk::read_state<velk::ui::IOrbit>(static_cast<velk::ui::IOrbit::Ptr>(g_orbit));
            if (state) {
                float factor = (yoffset > 0) ? 0.9f : 1.1f;
                g_orbit.set_distance(state->distance * factor);
            }
        }

        if (!g_input) {
            return;
        }
        velk::ui::ScrollEvent ev;
        double mx, my;
        glfwGetCursorPos(w, &mx, &my);
        ev.position = {static_cast<float>(mx), static_cast<float>(my)};
        ev.delta = {static_cast<float>(xoffset), static_cast<float>(yoffset)};
        ev.unit = velk::ui::ScrollUnit::Lines;
        g_input->scroll_event(ev);
    });

    // Add gradient background to the root element
    {
        auto root = scene.root();
        auto bg = velk::ui::visual::create_rect();
        bg.set_color(velk::color::red());
        bg.set_paint(velk::ui::material::create_gradient(
            velk::color{0.05f, 0.07f, 0.15f, 1.f}, velk::color{0.18f, 0.12f, 0.28f, 1.f}, 90.f));

        root.add_trait(bg);
        auto click = velk::ui::input::create_click();
        click.on_click().add_handler([]() { VELK_LOG(E, "Clicked!"); });
        root.add_trait(click);
    }

    // Custom shader material: checkerboard pattern on the first card
    {
        constexpr velk::string_view checker_vert = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(RectInstanceData)
    vec4 color_a;
    vec4 color_b;
    float scale;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec2 v_local_uv;

void main()
{
    vec2 q = velk_unit_quad(gl_VertexIndex);
    RectInstance inst = root.instance_data.data[gl_InstanceIndex];
    vec2 world_pos = inst.pos + q * inst.size;
    gl_Position = root.global_data.view_projection * vec4(world_pos, 0.0, 1.0);
    v_local_uv = q;
}
)";

        constexpr velk::string_view checker_frag = R"(
#version 450
#include "velk.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(Ptr64)
    vec4 color_a;
    vec4 color_b;
    float scale;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) in vec2 v_local_uv;
layout(location = 0) out vec4 frag_color;

void main()
{
    float s = root.scale;
    vec2 cell = floor(v_local_uv * s);
    float checker = mod(cell.x + cell.y, 2.0);
    frag_color = mix(root.color_a, root.color_b, checker);
}
)";

        auto sm = velk::create_shader_material(*render_ctx, checker_frag, checker_vert);
        if (sm) {
            sm.set_input<velk::color>("color_a", {0.15f, 0.15f, 0.25f, 0.6f});
            sm.set_input<velk::color>("color_b", {0.25f, 0.2f, 0.35f, 0.6f});
            sm.set_input<float>("scale", 8.0f);

            // Apply to the header element as paint
            auto header = scene.root().child_at(0).child_at(0);
            // Find first visual from the header
            auto v = velk::ui::Visual(header.find_attachment<velk::ui::IVisual>());
            // Apply paint
            v.set_paint(sm);
        }
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

    scene = velk::ui::Scene{};
    renderer->shutdown();
    renderer = nullptr;

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
