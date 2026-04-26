// Fluent RT demo — a 6x7 grid of mirror tiles where each tile has
// slightly different yaw + roughness so reflections vary across the
// "calendar". Tiles are built programmatically so the parameters are
// easy to tweak. See design-notes/fluent_demo.md.

#include "velk-ui/plugins/gltf/interface/intf_gltf_asset.h"
#include "velk-ui/plugins/image/intf_image_material.h"

#include <velk/api/any.h>
#include <velk/api/callback.h>
#include <velk/api/velk.h>

#include <chrono>
#include <cmath>
#include <velk-render/api/material/standard_material.h>
#include <velk-render/api/render_context.h>
#include <velk-render/api/shadow_technique.h>
#include <velk-render/debug/render_target_dump.h>
#include <velk-render/gbuffer.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-render/interface/intf_image.h>
#include <velk-runtime/api/application.h>
#include <velk-scene/api/camera.h>
#include <velk-scene/api/element.h>
#include <velk-scene/api/mesh.h>
#include <velk-scene/api/scene.h>
#include <velk-ui/api/trait/fixed_size.h>
#include <velk-scene/api/trait/light.h>
#include <velk-scene/api/trait/orbit.h>
#include <velk-scene/api/trait/trs.h>
#include <velk-scene/api/visual/cube.h>
#include <velk-ui/api/visual/rounded_rect.h>
#include <velk-scene/api/visual/sphere.h>
#include <velk-ui/interface/intf_texture_visual.h>
#include <velk-ui/plugins/image/api/image.h>
#include <velk-ui/plugins/image/plugin.h>
#include <velk-ui/plugins/text/api/text_visual.h>

#include <filesystem>

namespace {

void build_mirror_grid(velk::Scene& scene, velk::Element grid_root)
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

    velk::StandardMaterial m;

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
            m = mat;

            auto tile = velk::create_element();
            auto sz = velk::ui::trait::layout::create_fixed_size(
                velk::dim::px(kTileW), velk::dim::px(kTileH));
            auto tr = velk::trait::transform::create_trs();
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

    auto img = velk::ui::image::load_image("image:app://assets/avatar.png");
    m.base_color().set_texture(img.as_surface());
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
    wc.depth = velk::DepthFormat::Default;
    // wc.update_rate = velk::UpdateRate::Unlimited;

    auto app = velk::create_app(config);
    auto window = app.create_window(wc);
    if (!window) {
        VELK_LOG(E, "Initialization failed");
        return 1;
    }

    auto ctx = app.render_context();

    velk::instance().plugin_registry().load_plugin_from_path("velk_tracy.dll");

    auto scene = velk::create_scene("app://scenes/fluent.json");
    scene.set_geometry(velk::aabb::from_size({static_cast<float>(kWidth), static_cast<float>(kHeight)}));

    auto camera_3d = scene.find_first<velk::IOrbit>();
    velk::OrbitTrait orbit;
    if (camera_3d) {
        velk::Camera(camera_3d.find_trait<velk::ICamera>()).set_render_path(velk::RenderPath::Deferred);
        app.add_view(window, camera_3d, {0, 0, 1.f, 1.f});
        orbit = velk::OrbitTrait(camera_3d.find_trait<velk::IOrbit>());
    } else {
        VELK_LOG(E, "Fluent scene has no orbit camera; window will be blank.");
    }

    // Scene hierarchy: scene_root -> [camera_3d, floor, grid_root].
    auto root = scene.root();
    if (root.child_count() >= 3) {
        velk::Element floor(root.child_at(1));
        velk::Element grid_root(root.child_at(2));

        // Rough matte floor. Gives the mirrors something to reflect below
        // the horizon and breaks the symmetry with the sky reflection.
        if (auto floor_vis = velk::Visual2D(floor.find_trait<velk::IVisual>())) {
            floor_vis.set_paint(velk::material::create_standard(
                velk::color{0.45f, 0.42f, 0.40f, 1.f}, /*metallic=*/0.8f, /*roughness=*/0.2f));
        }

        build_mirror_grid(scene, grid_root);
    }

    // Sun: directional light above-and-ahead of the grid, casting
    // ray-traced shadows onto the floor + tiles. Direction is the
    // light element's forward axis (-Z), so the Trs rotation below
    // tilts the sun to a nice angle.
    {
        auto sun_light = velk::trait::render::create_directional_light(
            velk::color{1.f, 0.96f, 0.88f, 1.f}, /*intensity=*/2.5f);
        sun_light.add_technique(velk::technique::create_rt_shadow());

        auto sun = velk::create_element();
        auto sun_trs = velk::trait::transform::create_trs();
        sun_trs.set_rotation({35.f, 25.f, 0.f}); // pitch down (+Y forward) + yaw right
        sun.add_trait(sun_trs);
        sun.add_trait(sun_light);
        scene.add(scene.root(), sun);
    }

    // Animated 3D accents in front of the mirror grid. Cube + sphere,
    // orbiting the grid center on opposite phases. Using StandardMaterial
    // so they reflect the environment just like the tiles.
    velk::Trs cube_trs;
    velk::Trs sphere_trs;
    {
        auto cube = velk::create_element();
        auto cube_sz = velk::ui::trait::layout::create_fixed_size(
            velk::dim::px(140.f), velk::dim::px(140.f), velk::dim::px(140.f));
        cube_trs = velk::trait::transform::create_trs();
        auto cube_vis = velk::trait::visual::create_cube();
        // Build the cube mesh up-front so its primitive's material can
        // be set at authoring time. Procedural meshes share their
        // IMeshBuffer across calls; the primitive object is per-caller.
        velk::Mesh cube_mesh(ctx.build_cube());
        cube_mesh.set_material(0, velk::material::create_standard(
            velk::color{0.95f, 0.7f, 0.4f, 1.f}, /*metallic=*/0.9f, /*roughness=*/0.15f));
        cube_vis.set_mesh(cube_mesh);
        cube.add_trait(cube_sz);
        cube.add_trait(cube_trs);
        cube.add_trait(cube_vis);
        scene.add(scene.root(), cube);

        // Text block standing on the floor in front of the mirror grid.
        // The default text visual lies in the XY plane facing -Z; we
        // rotate it to stand vertical (XZ plane, facing -Y toward camera)
        // and translate down onto the floor.
        {
            auto text = velk::create_element();
            auto text_sz = velk::ui::trait::layout::create_fixed_size(
                velk::dim::px(900.f), velk::dim::px(180.f));
            auto text_trs = velk::trait::transform::create_trs();
            text_trs.set_translate({-450.f, 450.f, 650.f});
            auto tv = velk::ui::trait::visual::create_text();
            tv.set_text("velk-ui");
            tv.set_font_size(256.f);
            tv.set_color({0.95f, 0.55f, 1.0f, 1.f});
            tv.set_layout(velk::TextLayout::MultiLine);

            auto m = velk::instance().create<velk::ui::IImageMaterial>(::velk::ui::ClassId::Material::Image);
            auto img = velk::ui::image::load_image("image:app://assets/avatar.png");
            auto ref = ::velk::create_object_ref();
            ref.set(interface_pointer_cast<velk::IObject>(img.as_surface()));
            m->texture().set_value(ref);
            tv.set_paint(interface_pointer_cast<velk::IMaterial>(m));

            text.add_trait(text_sz);
            text.add_trait(text_trs);
            text.add_trait(tv);
            scene.add(scene.root(), text);
        }

        auto sphere = velk::create_element();
        auto sphere_sz = velk::ui::trait::layout::create_fixed_size(
            velk::dim::px(180.f), velk::dim::px(180.f), velk::dim::px(180.f));
        sphere_trs = velk::trait::transform::create_trs();
        auto sphere_vis = velk::trait::visual::create_sphere();
        velk::Mesh sphere_mesh(ctx.build_sphere());
        sphere_mesh.set_material(0, velk::material::create_standard(
            velk::color{0.4f, 0.6f, 0.95f, 1.f}, /*metallic=*/0.3f, /*roughness=*/0.08f));
        sphere_vis.set_mesh(sphere_mesh);
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
            auto state = velk::read_state<velk::IOrbit>(static_cast<velk::IOrbit::Ptr>(orbit));
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
                velk::read_state<velk::IOrbit>(static_cast<velk::IOrbit::Ptr>(orbit));
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

    {
        auto& rs = velk::instance().resource_store();
        auto asset = rs.get_resource<velk::ui::IGltfAsset>("gltf:app://assets/BoxTextured.glb");
        if (asset) {
            auto add = [&](const velk::vec3& pos) {
                auto store = asset->instantiate();
                if (store) {
                    auto host = velk::create_element();
                    auto host_trs = velk::trait::transform::create_trs();
                    host_trs.set_translate(pos);
                    host_trs.set_scale({200.f, 200.f, 200.f});
                    host.add_trait(host_trs);
                    scene.add(scene.root(), host);
                    scene.load(*store, host.as_ptr<velk::IElement>().get());
                    VELK_LOG(I, "gltf: loaded BoxTextured.glb (%zu objects)", store->object_count());
                } else {
                    VELK_LOG(W, "gltf: BoxTextured.glb instantiate() returned null");
                }
            };
            add({550.f, 400.f, 650.f});
            add({450.f, 500.f, 50.f});
        } else {
            VELK_LOG(W, "gltf: BoxTextured.glb resource not available");
        }
    }

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
