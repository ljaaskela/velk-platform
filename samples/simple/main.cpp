#include "velk-ui/plugins/text/api/text_visual.h"

#include <velk/api/any.h>
#include <velk/api/callback.h>
#include <velk/api/velk.h>

#include <velk-render/api/material/shader_material.h>
#include <velk-render/api/render_context.h>
#include <velk-runtime/api/application.h>
#include <velk-ui/api/camera.h>
#include <velk-ui/api/element.h>
#include <velk-ui/api/input/click.h>
#include <velk-ui/api/material/gradient.h>
#include <velk-ui/api/scene.h>
#include <velk-ui/api/trait/fixed_size.h>
#include <velk-ui/api/trait/orbit.h>
#include <velk-ui/api/trait/trs.h>
#include <velk-ui/api/visual/rect.h>
#include <velk-ui/api/visual/visual.h>
#include <velk-render/interface/intf_camera.h>

constexpr velk::string_view ipsum = R"(
Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Proin tortor purus platea sit eu id nisi litora libero. Neque vulputate consequat ac amet augue blandit maximus aliquet congue.
Pharetra vestibulum posuere ornare faucibus fusce dictumst orci aenean eu facilisis ut volutpat commodo senectus purus himenaeos fames primis convallis nisi.
Phasellus fermentum malesuada phasellus netus dictum aenean placerat egestas amet. Ornare taciti semper dolor tristique morbi. Sem leo tincidunt aliquet semper eu lectus scelerisque quis. Sagittis vivamus mollis nisi mollis enim fermentum laoreet.
Curabitur semper venenatis lectus viverra ex dictumst nulla maximus. Primis iaculis elementum conubia feugiat venenatis dolor augue ac blandit nullam ac phasellus turpis feugiat mollis.
Duis lectus porta mattis imperdiet vivamus augue litora lectus arcu. Justo torquent pharetra volutpat ad blandit bibendum accumsan nec elit cras luctus primis ipsum gravida class congue.
Vehicula etiam elementum finibus enim duis feugiat commodo adipiscing tortor tempor elit. Et mollis consectetur habitant turpis tortor consectetur adipiscing vulputate dolor lectus iaculis convallis adipiscing.
Nam hendrerit dignissim condimentum ullamcorper diam morbi eget consectetur odio in sagittis.
)";

int main(int argc, char* argv[])
{
    velk::ApplicationConfig config;

    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;

    velk::WindowConfig wc;
    wc.width = kWidth;
    wc.height = kHeight;
    wc.title = "velk-ui";
    // wc.update_rate = velk::UpdateRate::Unlimited;

    auto app = velk::create_app(config);
    auto window = app.create_window(wc);
    if (!window) {
        VELK_LOG(E, "Initialization failed");
        return 1;
    }

    velk::instance().plugin_registry().load_plugin_from_path("velk_tracy.dll");

    VELK_LOG(E, "ipsum: %zu", ipsum.size());

    // Load scene
    auto scene = velk::ui::create_scene("app://scenes/dashboard.json");
    scene.set_geometry(velk::aabb::from_size({static_cast<float>(kWidth), static_cast<float>(kHeight)}));

    // The dashboard scene has two cameras: an ortho camera (no orbit trait)
    // and a perspective camera with an orbit trait. Find them by trait.
    auto camera = scene.find_first<velk::ICamera>();   // first camera in pre-order = ortho
    auto camera_3d = scene.find_first<velk::ui::IOrbit>(); // only the perspective one has orbit

    if (camera) {
        app.add_view(window, camera, {0.5f, 0, 0.5f, 1.0f});
    }
    if (camera_3d) {
        velk::ui::Camera(camera_3d.find_trait<velk::ICamera>()).set_render_path(velk::RenderPath::RayTrace);
        app.add_view(window, camera_3d, {0, 0, .5f, 1.0f});
    }

    // Orbit camera control via input dispatcher events.
    velk::ui::OrbitTrait orbit;
    bool orbit_dragging = false;
    double orbit_last_x = 0.0, orbit_last_y = 0.0;

    if (camera_3d) {
        orbit = velk::ui::OrbitTrait(camera_3d.find_trait<velk::ui::IOrbit>());
    }

    auto* dispatcher = window.input();

    velk::ScopedHandler pointer_sub = window.add_on_pointer_event([&](const velk::ui::PointerEvent& e) {
        if (e.action == velk::ui::PointerAction::Down && e.button == velk::ui::PointerButton::Right &&
            orbit) {
            orbit_dragging = true;
            orbit_last_x = e.position.x;
            orbit_last_y = e.position.y;
        } else if (e.action == velk::ui::PointerAction::Up && e.button == velk::ui::PointerButton::Right) {
            orbit_dragging = false;
        } else if (e.action == velk::ui::PointerAction::Move && orbit_dragging && orbit) {
            float dx = e.position.x - static_cast<float>(orbit_last_x);
            float dy = e.position.y - static_cast<float>(orbit_last_y);
            auto state = velk::read_state<velk::ui::IOrbit>(static_cast<velk::ui::IOrbit::Ptr>(orbit));
            if (state) {
                orbit.set_yaw(state->yaw + dx * 0.3f);
                orbit.set_pitch(state->pitch + dy * 0.3f);
            }
            orbit_last_x = e.position.x;
            orbit_last_y = e.position.y;
        }
    });

    velk::ScopedHandler scroll_sub(dispatcher->on_scroll_event(),
        [&](const velk::ui::ScrollEvent& e) {
            if (!orbit) {
                return;
            }
            auto state = velk::read_state<velk::ui::IOrbit>(
                static_cast<velk::ui::IOrbit::Ptr>(orbit));
            if (state) {
                float factor = (e.delta.y > 0) ? 0.9f : 1.1f;
                orbit.set_distance(state->distance * factor);
            }
        });

    // Add gradient background to the root element
    {
        auto root = scene.root();
        auto bg = velk::ui::trait::visual::create_rect();
        bg.set_color(velk::color::red());
        bg.set_paint(velk::ui::material::create_gradient(
            velk::color{0.05f, 0.07f, 0.15f, 1.f}, velk::color{0.18f, 0.12f, 0.28f, 1.f}, 90.f));

        root.add_trait(bg);
        auto click = velk::ui::trait::input::create_click();
        click.on_click().add_handler([&](const velk::ui::PointerEvent& ev) {
            VELK_LOG(E, "Clicked %f %f", ev.local_position.x, ev.local_position.y);
        });
        root.add_trait(click);
    }

    {
        float offset = -100;
        float c = 0.f;
        /*for (auto i = 0; i < 10; i++) {
            auto root = velk::ui::create_element();
            auto sz =
                velk::ui::trait::layout::create_fixed_size(velk::ui::dim::px(640), velk::ui::dim::px(1080));
            auto tr = velk::ui::trait::transform::create_trs();
            tr.set_translate({offset, offset, offset});
            auto vis = velk::ui::trait::visual::create_text();
            vis.set_text(ipsum);
            vis.set_color({1.f - c, c, (c * 2.f) / 2.f, 1});
            vis.set_font_size(28 - (1.f - c) * 14);
            vis.set_layout(velk::ui::TextLayout::WordWrap);
            root.add_trait(sz);
            root.add_trait(tr);
            root.add_trait(vis);
            scene.add(scene.root(), root);
            offset += 10;
            c += .1f;
        }*/
    }

    // Custom shader material: checkerboard pattern on the first card
    {
        auto ctx = app.render_context();

        constexpr velk::string_view checker_vert = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer CheckerParams {
    vec4 color_a;
    vec4 color_b;
    float scale;
};

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(RectInstanceData)
    CheckerParams material;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec2 v_local_uv;

void main()
{
    vec2 q = velk_unit_quad(gl_VertexIndex);
    RectInstance inst = root.instance_data.data[gl_InstanceIndex];
    vec4 local_pos = vec4(inst.pos + q * inst.size, 0.0, 1.0);
    gl_Position = root.global_data.view_projection * inst.world_matrix * local_pos;
    v_local_uv = q;
}
)";

        constexpr velk::string_view checker_frag = R"(
#version 450
#include "velk.glsl"

layout(buffer_reference, std430) readonly buffer CheckerParams {
    vec4 color_a;
    vec4 color_b;
    float scale;
};

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(OpaquePtr)
    CheckerParams material;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) in vec2 v_local_uv;
layout(location = 0) out vec4 frag_color;

void main()
{
    float s = root.material.scale;
    vec2 cell = floor(v_local_uv * s);
    float checker = mod(cell.x + cell.y, 2.0);
    frag_color = mix(root.material.color_a, root.material.color_b, checker);
}
)";

        auto sm = velk::create_shader_material(
            static_cast<velk::IRenderContext&>(*ctx.as<velk::IRenderContext>()), checker_frag, checker_vert);
        if (sm) {
            sm.set_input<velk::color>("color_a", {0.15f, 0.15f, 0.25f, 0.6f});
            sm.set_input<velk::color>("color_b", {0.25f, 0.2f, 0.35f, 0.6f});
            sm.set_input<float>("scale", 8.0f);

            auto header = scene.root().child_at(0).child_at(0);
            auto v = velk::ui::Visual(header.find_attachment<velk::ui::IVisual>());
            v.set_paint(sm);
        }
    }

    // Performance overlay (FPS, CPU/frame timing).
    velk::PerformanceOverlayConfig poc;
    poc.enabled = true;
    app.set_performance_overlay(window, poc);

    // First frame
    app.update();
    app.present();
    // Print stats after first frame
    {
        auto stats = velk::instance().get_stats();
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
    while (app.poll()) {
        app.update();
        app.present();
    }

    return 0;
}
