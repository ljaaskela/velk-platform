// Fluent RT demo — a 6x7 grid of mirror tiles where each tile has
// slightly different yaw + roughness so reflections vary across the
// "calendar". Tiles are built programmatically so the parameters are
// easy to tweak. See design-notes/fluent_demo.md.

#include <velk/api/any.h>
#include <velk/api/callback.h>
#include <velk/api/velk.h>

#include <velk-render/api/material/standard.h>
#include <velk-render/api/render_context.h>
#include <velk-render/api/shadow_technique.h>
#include <velk-runtime/api/application.h>
#include <velk-ui/api/camera.h>
#include <velk-ui/api/element.h>
#include <velk-ui/api/scene.h>
#include <velk-ui/api/trait/fixed_size.h>
#include <velk-ui/api/trait/light.h>
#include <velk-ui/api/trait/orbit.h>
#include <velk-ui/api/trait/trs.h>
#include <velk-ui/api/visual/cube.h>
#include <velk-ui/api/visual/rounded_rect.h>
#include <velk-ui/api/visual/sphere.h>
#include <velk-ui/plugins/text/api/text_visual.h>
#include <velk-render/interface/intf_camera.h>

#include <chrono>
#include <cmath>

namespace {

void build_mirror_grid(velk::ui::Scene& scene, velk::ui::Element grid_root)
{
    constexpr int kCols = 6;
    constexpr int kRows = 7;
    constexpr float kTileW = 200.f;
    constexpr float kTileH = 160.f;
    constexpr float kGapX = 24.f;
    constexpr float kGapY = 24.f;

    const float grid_w = kCols * kTileW + (kCols - 1) * kGapX;
    const float grid_h = kRows * kTileH + (kRows - 1) * kGapY;
    const float x0 = -grid_w * 0.5f;
    // Lift the grid so the whole thing floats well above the floor plane
    // (floor y = 700 in the scene file).
    const float y0 = -grid_h * 0.5f - 400.f;

    auto hash = [](uint32_t x) {
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    };

    uint32_t idx = 0;
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col, ++idx) {
            // Deterministic pseudo-random variance per tile.
            uint32_t h = hash(idx * 2654435761u);
            float r01_a = (h & 0xFFFF) / 65535.f;
            float r01_b = ((h >> 16) & 0xFFFF) / 65535.f;

            float yaw = (r01_a - 0.5f) * 12.f;    // +/- 6 degrees
            float pitch = (r01_b - 0.5f) * 8.f;   // +/- 4 degrees
            float roughness = 0.02f + r01_a * 0.25f;

            auto mat = velk::material::create_standard(
                velk::color{0.9f, 0.92f, 0.95f, 1.f}, /*metallic=*/1.f, roughness);

            auto tile = velk::ui::create_element();
            auto sz = velk::ui::trait::layout::create_fixed_size(
                velk::ui::dim::px(kTileW), velk::ui::dim::px(kTileH));
            auto tr = velk::ui::trait::transform::create_trs();
            float cx = x0 + col * (kTileW + kGapX);
            float cy = y0 + row * (kTileH + kGapY);
            tr.set_translate({cx, cy, 0.f});
            tr.set_rotation({pitch, yaw, 0.f});

            auto vis = velk::ui::trait::visual::create_rounded_rect();
            vis.set_color({0.9f, 0.92f, 0.95f, 1.f});
            vis.set_paint(mat);

            tile.add_trait(sz);
            tile.add_trait(tr);
            tile.add_trait(vis);
            scene.add(grid_root, tile);
        }
    }
}

} // namespace

int main(int /*argc*/, char* /*argv*/[])
{
    velk::ApplicationConfig config;

    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;

    velk::WindowConfig wc;
    wc.width = kWidth;
    wc.height = kHeight;
    wc.title = "velk-ui fluent";

    auto app = velk::create_app(config);
    auto window = app.create_window(wc);
    if (!window) {
        VELK_LOG(E, "Initialization failed");
        return 1;
    }

    velk::instance().plugin_registry().load_plugin_from_path("velk_tracy.dll");

    auto scene = velk::ui::create_scene("app://scenes/fluent.json");
    scene.set_geometry(velk::aabb::from_size({static_cast<float>(kWidth), static_cast<float>(kHeight)}));

    auto camera_3d = scene.find_first<velk::ui::IOrbit>();
    velk::ui::OrbitTrait orbit;
    if (camera_3d) {
        velk::ui::Camera(camera_3d.find_trait<velk::ICamera>()).set_render_path(velk::RenderPath::Deferred);
        app.add_view(window, camera_3d, {0, 0, 1.f, 1.f});
        orbit = velk::ui::OrbitTrait(camera_3d.find_trait<velk::ui::IOrbit>());
    } else {
        VELK_LOG(E, "Fluent scene has no orbit camera; window will be blank.");
    }

    // Scene hierarchy: scene_root -> [camera_3d, floor, grid_root].
    auto root = scene.root();
    if (root.child_count() >= 3) {
        velk::ui::Element floor(root.child_at(1));
        velk::ui::Element grid_root(root.child_at(2));

        // Rough matte floor. Gives the mirrors something to reflect below
        // the horizon and breaks the symmetry with the sky reflection.
        if (auto floor_vis = velk::ui::Visual(floor.find_trait<velk::ui::IVisual>())) {
            floor_vis.set_paint(velk::material::create_standard(
                velk::color{0.45f, 0.42f, 0.40f, 1.f}, /*metallic=*/0.f, /*roughness=*/0.7f));
        }

        build_mirror_grid(scene, grid_root);
    }

    // Sun: directional light above-and-ahead of the grid, casting
    // ray-traced shadows onto the floor + tiles. Direction is the
    // light element's forward axis (-Z), so the Trs rotation below
    // tilts the sun to a nice angle.
    {
        auto sun_light = velk::ui::trait::render::create_directional_light(
            velk::color{1.f, 0.96f, 0.88f, 1.f}, /*intensity=*/2.5f);
        sun_light.add_technique(velk::technique::create_rt_shadow());

        auto sun = velk::ui::create_element();
        auto sun_trs = velk::ui::trait::transform::create_trs();
        sun_trs.set_rotation({35.f, 25.f, 0.f}); // pitch down (+Y forward) + yaw right
        sun.add_trait(sun_trs);
        sun.add_trait(sun_light);
        scene.add(scene.root(), sun);
    }

    // Animated 3D accents in front of the mirror grid. Cube + sphere,
    // orbiting the grid center on opposite phases. Using StandardMaterial
    // so they reflect the environment just like the tiles.
    velk::ui::Trs cube_trs;
    velk::ui::Trs sphere_trs;
    {
        auto cube = velk::ui::create_element();
        auto cube_sz = velk::ui::trait::layout::create_fixed_size(
            velk::ui::dim::px(140.f), velk::ui::dim::px(140.f), velk::ui::dim::px(140.f));
        cube_trs = velk::ui::trait::transform::create_trs();
        auto cube_vis = velk::ui::trait::visual::create_cube();
        cube_vis.set_color({0.95f, 0.95f, 1.0f, 1.f});
        cube_vis.set_paint(velk::material::create_standard(
            velk::color{0.95f, 0.7f, 0.4f, 1.f}, /*metallic=*/0.9f, /*roughness=*/0.15f));
        cube.add_trait(cube_sz);
        cube.add_trait(cube_trs);
        cube.add_trait(cube_vis);
        scene.add(scene.root(), cube);

        // Text block standing on the floor in front of the mirror grid.
        // The default text visual lies in the XY plane facing -Z; we
        // rotate it to stand vertical (XZ plane, facing -Y toward camera)
        // and translate down onto the floor.
        {
            auto text = velk::ui::create_element();
            auto text_sz = velk::ui::trait::layout::create_fixed_size(
                velk::ui::dim::px(900.f), velk::ui::dim::px(180.f));
            auto text_trs = velk::ui::trait::transform::create_trs();
            text_trs.set_translate({-450.f, 450.f, 650.f});
            auto tv = velk::ui::trait::visual::create_text();
            tv.set_text("velk-ui");
            tv.set_font_size(256.f);
            tv.set_color({0.95f, 0.55f, 1.0f, 1.f});
            tv.set_layout(velk::ui::TextLayout::MultiLine);
            text.add_trait(text_sz);
            text.add_trait(text_trs);
            text.add_trait(tv);
            scene.add(scene.root(), text);
        }

        auto sphere = velk::ui::create_element();
        auto sphere_sz = velk::ui::trait::layout::create_fixed_size(
            velk::ui::dim::px(180.f), velk::ui::dim::px(180.f), velk::ui::dim::px(180.f));
        sphere_trs = velk::ui::trait::transform::create_trs();
        auto sphere_vis = velk::ui::trait::visual::create_sphere();
        sphere_vis.set_color({0.95f, 0.95f, 1.0f, 1.f});
        sphere_vis.set_paint(velk::material::create_standard(
            velk::color{0.4f, 0.6f, 0.95f, 1.f}, /*metallic=*/0.3f, /*roughness=*/0.08f));
        sphere.add_trait(sphere_sz);
        sphere.add_trait(sphere_trs);
        sphere.add_trait(sphere_vis);
        scene.add(scene.root(), sphere);
    }

    bool orbit_dragging = false;
    double orbit_last_x = 0.0, orbit_last_y = 0.0;

    velk::ScopedHandler pointer_sub = window.add_on_pointer_event([&](const velk::ui::PointerEvent& e) {
        if (e.action == velk::ui::PointerAction::Down && e.button == velk::ui::PointerButton::Right && orbit) {
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

    auto* dispatcher = window.input();
    velk::ScopedHandler scroll_sub(dispatcher->on_scroll_event(),
        [&](const velk::ui::ScrollEvent& e) {
            if (!orbit) return;
            auto state =
                velk::read_state<velk::ui::IOrbit>(static_cast<velk::ui::IOrbit::Ptr>(orbit));
            if (state) {
                float factor = (e.delta.y > 0) ? 0.9f : 1.1f;
                orbit.set_distance(state->distance * factor);
            }
        });

    velk::PerformanceOverlayConfig poc;
    poc.enabled = true;
    app.set_performance_overlay(window, poc);

    auto t0 = std::chrono::steady_clock::now();
    auto tick = [&]() {
        float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        // Cube orbits in the xz plane in front of the grid; sphere 180 deg
        // out of phase and slightly lower/faster.
        constexpr float kRadius = 700.f;
        constexpr float kCubeY = -150.f;
        constexpr float kSphereY = 50.f;
        if (cube_trs) {
            float a = t * 0.6f;
            cube_trs.set_translate({std::cos(a) * kRadius, kCubeY, std::sin(a) * kRadius + 100.f});
            cube_trs.set_rotation({t * 30.f, t * 45.f, 0.f});
        }
        if (sphere_trs) {
            float a = t * 0.9f + 3.14159f;
            sphere_trs.set_translate({std::cos(a) * kRadius * 0.8f, kSphereY,
                                      std::sin(a) * kRadius * 0.8f + 150.f});
        }
    };

    tick();
    app.update();
    app.present();

    // First frame has rendered: G-buffer is allocated and its bindless
    // ids are stable. Register a column of debug overlays on the right
    // edge, one per attachment (albedo / normal / worldpos / material).
    if (auto renderer = app.renderer()) {
        if (camera_3d) {
            auto window_surface = window.surface();
            constexpr float kThumbW = 220.f;
            const float col_x = static_cast<float>(kWidth) - kThumbW;
            const float thumb_h = static_cast<float>(kHeight) / 4.f;
            for (uint32_t i = 0; i < 4; ++i) {
                velk::TextureId tid =
                    renderer->get_gbuffer_attachment(camera_3d, window_surface, i);
                if (tid != 0) {
                    renderer->add_debug_overlay(
                        window_surface, tid,
                        {col_x, i * thumb_h, kThumbW, thumb_h});
                }
            }
        }
    }

    while (app.poll()) {
        tick();
        app.update();
        app.present();
    }
    return 0;
}
