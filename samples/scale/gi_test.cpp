// Plain scenes for judging global illumination, each paired with a reference.
//
// Usage: velk_ui_scale gitest <test> [frames]
//
// frames is when motion ends, 5 frames before it: default 240. Set VELK_AUTO_EXIT_FRAMES to
// the same value to exit and capture then. samples/scale/gi_test.ps1 runs all tests and
// scores the captures.
//
// A building on open ground, split in two by a thin wall, seen from inside:
//   leak_light: a light in the far chamber, none in this one. Should be black.
//   leak_lit:   the same light, seen from its own chamber. The brightness leak_light is
//               measured against.
//   leak_sky:   the building is closed, under a sky and sun. Should be black.
//   open_sky:   the roof removed, the brightness leak_sky is measured against.
//
// A court with a wall, a box and a sphere, under a sky and sun:
//   size_1:      the reference for the tests below.
//   size_10:     the ground is 10 times wider, out of view.
//   size_100:    the ground is 100 times wider, out of view.
//   outlier:     one small box very far away.
//   shapes_mesh: boxes and spheres are triangle meshes instead of analytic shapes.
//   origin_1e5, origin_1e6, origin_1e7: the whole court that far from the world origin.
//
// The court with something changing until just before the capture:
//   move, move_ref:     a box moving past the others, or standing at its final position.
//   deform, deform_ref: a rippling sphere, or held at its final shape.
//
// The court lit only from one place:
//   lights_1:  one point light.
//   lights_64: 64 lights packed around that point, with the same total power.
//
// Instances much wider than the spacing between GI probes, with triangles much smaller:
//   dense:     the instances, far form on, so wide rays may stop inside their meshes.
//   dense_ref: far form off, so rays test every triangle.

#include <velk/api/velk.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <velk-render/api/material/standard_material.h>
#include <velk-render/api/render_context.h>
#include <velk-render/api/shadow_technique.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-runtime/api/application.h>
#include <velk-scene/api/camera.h>
#include <velk-scene/api/element.h>
#include <velk-scene/api/mesh.h>
#include <velk-scene/api/post_process.h>
#include <velk-scene/api/render_path.h>
#include <velk-scene/api/scene.h>
#include <velk-scene/api/trait/compute_mesh_deformer.h>
#include <velk-scene/api/trait/light.h>
#include <velk-scene/api/trait/orbit.h>
#include <velk-scene/api/trait/trs.h>
#include <velk-scene/api/visual/cube.h>
#include <velk-scene/api/visual/instanced_mesh.h>
#include <velk-scene/api/visual/mesh.h>
#include <velk-scene/api/visual/sphere.h>
#include <velk-ui/api/trait/fixed_size.h>
#include <velk-ui/plugins/image/api/environment.h>
#include <velk/interface/resource/intf_resource_protocol.h>
#include <velk/interface/resource/intf_resource_store.h>

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr long kMotionEndsBeforeExit = 5;
constexpr float kCourtHalf = 1500.f;
constexpr float kClusterPower = 1.5f;

/// A Radiance HDR file of one colour.
velk::vector<uint8_t> build_flat_hdr(float r, float g, float b)
{
    constexpr int kSkyWidth = 64;
    constexpr int kSkyHeight = 32;
    velk::vector<uint8_t> bytes;
    char header[96];
    const int header_size = std::snprintf(header, sizeof(header), "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n", kSkyHeight, kSkyWidth);
    for (int i = 0; i < header_size; ++i) {
        bytes.push_back(static_cast<uint8_t>(header[i]));
    }
    const float largest = std::max({r, g, b});
    int exponent = 0;
    const float scale = largest > 0.f ? std::frexp(largest, &exponent) * 256.f / largest : 0.f;
    for (int i = 0; i < kSkyWidth * kSkyHeight; ++i) {
        bytes.push_back(static_cast<uint8_t>(r * scale));
        bytes.push_back(static_cast<uint8_t>(g * scale));
        bytes.push_back(static_cast<uint8_t>(b * scale));
        bytes.push_back(largest > 0.f ? static_cast<uint8_t>(exponent + 128) : uint8_t{0});
    }
    return bytes;
}

} // namespace

int run_gi_test(const char* test, long frames)
{
    const auto is = [&](const char* name) { return std::strcmp(test, name) == 0; };
    const bool leak_room = is("leak_light") || is("leak_lit") || is("leak_sky") || is("open_sky");
    const bool moving = is("move") || is("move_ref");
    const bool deforming = is("deform") || is("deform_ref");
    const bool light_cluster = is("lights_1") || is("lights_64");
    const bool dense_instances = is("dense") || is("dense_ref");
    const float world_shift = is("origin_1e5") ? 1e5f : is("origin_1e6") ? 1e6f : is("origin_1e7") ? 1e7f : 0.f;
    if (!leak_room && !moving && !deforming && !light_cluster && !dense_instances && world_shift == 0.f && !is("size_1") && !is("size_10") && !is("size_100") && !is("outlier")
        && !is("shapes_mesh")) {
        VELK_LOG(E, "gitest: unknown test %s", test);
        return 1;
    }

    velk::ApplicationConfig config;
    velk::WindowConfig wc;
    wc.width = kWidth;
    wc.height = kHeight;
    wc.title = "velk-ui gitest";
    wc.depth = velk::DepthFormat::Default;
    wc.color_format = velk::SurfaceColorFormat::RGBA16F;
    wc.update_rate = velk::UpdateRate::Unlimited;

    auto app = velk::create_app(config);
    auto window = app.create_window(wc);
    if (!window) {
        VELK_LOG(E, "gitest: failed to create window");
        return 1;
    }
    auto ctx = app.render_context();
    auto render_context = static_cast<velk::IRenderContext::Ptr>(ctx);
    if (!render_context) {
        VELK_LOG(E, "gitest: no render context");
        return 1;
    }

    auto scene = velk::create_scene();
    scene.set_geometry(velk::aabb::from_size({static_cast<float>(kWidth), static_cast<float>(kHeight)}));
    auto root = velk::create_element();
    auto camera = velk::create_element();
    scene.set_root(root);
    scene.add(root, camera);

    const bool as_mesh = is("shapes_mesh");

    const auto add_box = [&](velk::vec3 min, velk::vec3 size, velk::color c) {
        auto element = velk::create_element();
        auto trs = velk::trait::transform::create_trs();
        trs.set_translate({min.x + world_shift, min.y, min.z});
        element.add_trait(trs);
        velk::Mesh mesh(ctx.build_cube());
        mesh.set_material(0, velk::material::create_standard(c, 0.f, 0.9f));
        if (as_mesh) {
            trs.set_scale(size);
            auto visual = velk::trait::visual::create_mesh();
            visual.set_mesh(mesh);
            element.add_trait(visual);
        } else {
            auto visual = velk::trait::visual::create_cube();
            visual.set_mesh(mesh);
            element.add_trait(velk::ui::trait::layout::create_fixed_size(velk::dim::px(size.x), velk::dim::px(size.y), velk::dim::px(size.z)));
            element.add_trait(visual);
        }
        scene.add(root, element);
        return trs;
    };

    const auto add_sphere = [&](velk::vec3 center, float radius, velk::color c) {
        auto element = velk::create_element();
        auto trs = velk::trait::transform::create_trs();
        trs.set_translate({center.x - radius + world_shift, center.y - radius, center.z - radius});
        element.add_trait(trs);
        velk::Mesh mesh(ctx.build_sphere(32));
        mesh.set_material(0, velk::material::create_standard(c, 0.f, 0.9f));
        if (as_mesh) {
            trs.set_scale({radius * 2.f, radius * 2.f, radius * 2.f});
            auto visual = velk::trait::visual::create_mesh();
            visual.set_mesh(mesh);
            element.add_trait(visual);
        } else {
            auto visual = velk::trait::visual::create_sphere();
            visual.set_mesh(mesh);
            const auto d = velk::dim::px(radius * 2.f);
            element.add_trait(velk::ui::trait::layout::create_fixed_size(d, d, d));
            element.add_trait(visual);
        }
        scene.add(root, element);
    };

    const auto add_point_light = [&](velk::vec3 position, float intensity, float range) {
        auto light = velk::trait::render::create_point_light(velk::color{1.f, 0.96f, 0.88f, 1.f}, intensity, range);
        light.add_technique(velk::technique::create_rt_shadow());
        auto element = velk::create_element();
        auto trs = velk::trait::transform::create_trs();
        trs.set_translate(position);
        element.add_trait(trs);
        element.add_trait(light);
        scene.add(root, element);
    };

    const velk::color grey{0.5f, 0.5f, 0.5f, 1.f};
    const velk::color white{0.75f, 0.75f, 0.75f, 1.f};

    bool sky_and_sun = !light_cluster;
    velk::vec3 focus_at{0.f, -40.f, 0.f};
    float yaw = 30.f;
    // Steep enough that only ground shows behind the objects, however wide it is.
    float pitch = -50.f;
    float distance = 800.f;
    float far_clip = 20000.f;

    if (leak_room) {
        // One wall's thickness: much thinner than the spacing between GI probes.
        constexpr float kThin = 2.f;
        add_box({-kCourtHalf, 0.f, -kCourtHalf}, {kCourtHalf * 2.f, 4.f, kCourtHalf * 2.f}, grey);
        add_box({-400.f - kThin, -302.f, -202.f}, {kThin, 302.f, 404.f}, white);
        add_box({400.f, -302.f, -202.f}, {kThin, 302.f, 404.f}, white);
        add_box({-400.f, -302.f, -202.f}, {800.f, 302.f, kThin}, white);
        add_box({-400.f, -302.f, 200.f}, {800.f, 302.f, kThin}, white);
        if (!is("open_sky")) {
            add_box({-402.f, -302.f, -202.f}, {804.f, kThin, 404.f}, white);
        }
        add_box({-kThin * 0.5f, -300.f, -200.f}, {kThin, 300.f, 400.f}, white);

        sky_and_sun = is("leak_sky") || is("open_sky");
        if (!sky_and_sun) {
            add_point_light({-200.f, -150.f, 0.f}, 1.2f, 800.f);
        }
        const bool own_chamber = is("leak_lit");
        focus_at = {own_chamber ? -1.f : 1.f, -150.f, 0.f};
        yaw = own_chamber ? -90.f : 90.f;
        pitch = 0.f;
        distance = 349.f;
    } else {
        const float half = dense_instances ? 16000.f : kCourtHalf * (is("size_10") ? 10.f : is("size_100") ? 100.f : 1.f);
        far_clip = std::max(far_clip, half * 4.f);
        add_box({-half, 0.f, -half}, {half * 2.f, 4.f, half * 2.f}, grey);
        if (dense_instances) {
            constexpr float kSize = 2400.f;
            constexpr float kStep = 4000.f;
            velk::Mesh mesh(ctx.build_sphere(512));
            mesh.set_material(0, velk::material::create_standard(velk::color{0.7f, 0.55f, 0.4f, 1.f}, 0.f, 0.9f));
            velk::vector<velk::InstanceTransform> transforms;
            for (int x = -3; x <= 3; ++x) {
                for (int z = -3; z <= 3; ++z) {
                    const float px = static_cast<float>(x) * kStep - kSize * 0.5f;
                    const float pz = static_cast<float>(z) * kStep - kSize * 0.5f;
                    transforms.push_back(velk::InstanceTransform{{{kSize, 0.f, 0.f, px}, {0.f, kSize, 0.f, -kSize}, {0.f, 0.f, kSize, pz}}});
                }
            }
            auto visual = velk::trait::visual::create_instanced_mesh();
            visual.set_mesh(mesh);
            if (is("dense_ref")) {
                visual.set_far_form(false);
            }
            visual.add_instances({transforms.data(), transforms.size()});
            auto element = velk::create_element();
            element.add_trait(visual);
            scene.add(root, element);
            focus_at = {0.f, -800.f, 0.f};
            pitch = -35.f;
            distance = 11000.f;
        } else {
            add_box({-200.f, -300.f, -200.f}, {20.f, 300.f, 400.f}, velk::color{0.8f, 0.1f, 0.1f, 1.f});
            add_box({20.f, -140.f, -60.f}, {120.f, 140.f, 120.f}, white);
            add_sphere({-60.f, -70.f, 60.f}, 70.f, velk::color{0.2f, 0.7f, 0.25f, 1.f});
        }
        if (is("outlier")) {
            add_box({1000000.f, -20.f, 0.f}, {20.f, 20.f, 20.f}, white);
        }
        if (is("lights_1")) {
            add_point_light({0.f, -250.f, 0.f}, kClusterPower, 1500.f);
        }
        if (is("lights_64")) {
            for (int x = 0; x < 4; ++x) {
                for (int y = 0; y < 4; ++y) {
                    for (int z = 0; z < 4; ++z) {
                        const velk::vec3 offset{static_cast<float>(x) * 5.f - 7.5f, static_cast<float>(y) * 5.f - 7.5f,
                                                static_cast<float>(z) * 5.f - 7.5f};
                        add_point_light({offset.x, -250.f + offset.y, offset.z}, kClusterPower / 64.f, 1500.f);
                    }
                }
            }
        }
    }

    const long motion_frames = std::max(frames - kMotionEndsBeforeExit, 1L);
    const velk::vec3 mover_start{150.f, -100.f, -300.f};
    const velk::vec3 mover_end{150.f, -100.f, 100.f};
    velk::Trs mover;
    if (moving) {
        mover = add_box(mover_end, {100.f, 100.f, 100.f}, velk::color{0.95f, 0.5f, 0.1f, 1.f});
    }

    velk::ComputeMeshDeformer ripple;
    if (deforming) {
        velk::Mesh source(ctx.build_sphere(128));
        source.set_material(0, velk::material::create_standard(velk::color{0.95f, 0.5f, 0.1f, 1.f}, 0.f, 0.9f));
        auto element = velk::create_element();
        auto trs = velk::trait::transform::create_trs();
        trs.set_translate({150.f, -220.f, -120.f});
        trs.set_scale({200.f, 200.f, 200.f});
        element.add_trait(trs);
        auto visual = velk::trait::visual::create_mesh();
        visual.set_mesh(velk::Mesh(render_context->get_mesh_builder().build_deforming(source)));
        element.add_trait(visual);
        ripple = velk::trait::deform::create_compute_mesh();
        ripple.set_deform(R"(
VelkVertex3D velk_mesh_deform(VelkVertex3D rest, uint index, vec4 params)
{
    float wave = sin(rest.position.y * 30.0 + params.x * 4.0);
    VelkVertex3D v = rest;
    v.position = rest.position + rest.normal * (wave * 0.06);
    return v;
}
)", 0.061f);
        element.add_trait(ripple);
        scene.add(root, element);
    }

    if (sky_and_sun) {
        auto light = velk::trait::render::create_directional_light(velk::color{1.f, 0.96f, 0.88f, 1.f}, 0.8f);
        light.add_technique(velk::technique::create_rt_shadow());
        auto sun = velk::create_element();
        auto trs = velk::trait::transform::create_trs();
        trs.set_rotation({50.f, 30.f, 0.f});
        sun.add_trait(trs);
        sun.add_trait(light);
        scene.add(root, sun);
    }

    auto cam_trait = velk::trait::render::create_camera();
    camera.add_trait(cam_trait);
    velk::Camera cam(cam_trait);
    cam.set_projection(velk::Projection::Perspective);
    cam.set_fov(45.f);
    cam.add_render_path(velk::path::create_deferred());
    auto post = velk::pp::create_post_process();
    post.add(velk::pp::create_tonemap());
    cam.add_post_process(post);

    // Always an explicit environment, so a test never depends on what none means.
    const velk::vector<uint8_t> sky = sky_and_sun ? build_flat_hdr(0.2f, 0.25f, 0.33f) : build_flat_hdr(0.f, 0.f, 0.f);
    auto protocol = velk::instance().resource_store().find_protocol("memory");
    if (auto* memory = interface_cast<velk::IMemoryProtocol>(protocol.get())) {
        memory->add_file("gitest_sky.hdr", sky.data(), sky.size());
        cam.set_environment(velk::ui::load_environment("env:memory://gitest_sky.hdr"));
    }

    auto focus = velk::create_element();
    auto focus_trs = velk::trait::transform::create_trs();
    focus_trs.set_translate({focus_at.x + world_shift, focus_at.y, focus_at.z});
    focus.add_trait(focus_trs);
    scene.add(root, focus);
    auto orbit = velk::trait::transform::create_orbit();
    orbit.set_target(focus);
    orbit.set_yaw(yaw);
    orbit.set_pitch(pitch);
    orbit.set_distance(distance);
    camera.add_trait(orbit);
    camera.find_trait<velk::ICamera>()->far_clip().set_value(far_clip);

    app.add_view(window, camera);

    long frame = 0;
    while (app.poll()) {
        // Motion follows the frame number, so runs are comparable.
        const float progress = is("move") || is("deform") ? std::min(static_cast<float>(frame) / static_cast<float>(motion_frames), 1.f) : 1.f;
        if (moving) {
            mover.set_translate(mover_start + (mover_end - mover_start) * progress);
        }
        if (deforming) {
            ripple.set_params({progress * static_cast<float>(motion_frames) / 60.f, 0.f, 0.f, 0.f});
        }
        app.update();
        app.present();
        ++frame;
    }
    return 0;
}
