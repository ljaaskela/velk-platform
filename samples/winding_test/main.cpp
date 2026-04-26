// Winding-convention regression test.
//
// Renders the built-in CubeVisual with a custom fragment shader that
// outputs color based on gl_FrontFacing:
//
//   GREEN = pipeline classified this fragment's source triangle as FRONT
//   RED   = pipeline classified it as BACK
//
// With framework defaults (FrontFace::Clockwise + CullMode::Back) and
// a CCW-from-outside mesh like our cube, the visible (outside) faces
// should appear GREEN. Back faces are culled; they should not show.
//
// Run this sample:
//   - All visible cube faces green  => convention intact (PASS)
//   - Any visible face red          => winding flipped somewhere (FAIL)
//   - Cube missing entirely (black) => culling inverted (FAIL)
//
// If this fails after a future change to the math, camera, projection,
// viewport, or rotation matrices, the visible regression tells you the
// framework's winding convention has drifted and needs re-checking.

#include <velk/api/velk.h>

#include <cmath>
#include <velk-render/api/material/shader_material.h>
#include <velk-render/api/material/standard_material.h>
#include <velk-render/api/render_context.h>
#include <velk-render/ext/element_vertex.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-runtime/api/application.h>
#include <velk-scene/api/camera.h>
#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
#include <velk-ui/api/trait/fixed_size.h>
#include <velk-scene/api/trait/orbit.h>
#include <velk-scene/api/trait/trs.h>
#include <velk-scene/api/visual/cube.h>
#include <velk-scene/api/visual/visual.h>

namespace {

// Outputs GREEN if the fragment's source triangle is classified as
// front-facing by the rasterizer, RED if back-facing. The vertex shader
// (ext::element_vertex_src) is the standard one used by every visual.
constexpr velk::string_view front_facing_frag = R"(
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local_uv;
layout(location = 2) flat in vec2 v_size;
layout(location = 3) in vec3 v_world_pos;
layout(location = 4) in vec3 v_world_normal;
layout(location = 5) flat in uint v_shape_param;

layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = gl_FrontFacing
        ? vec4(0.0, 1.0, 0.0, 1.0)   // front => green => pass
        : vec4(1.0, 0.0, 0.0, 1.0);  // back  => red   => fail
}
)";

} // namespace

int main(int /*argc*/, char* /*argv*/[])
{
    velk::ApplicationConfig config;

    velk::WindowConfig wc;
    wc.width = 512;
    wc.height = 512;
    wc.title = "velk-ui winding test (green=pass, red=fail)";
    wc.depth = velk::DepthFormat::Default;

    auto app = velk::create_app(config);
    auto window = app.create_window(wc);
    if (!window) {
        VELK_LOG(E, "winding_test: failed to create window");
        return 1;
    }

    auto ctx = app.render_context();
    if (!ctx) {
        VELK_LOG(E, "winding_test: no render context");
        return 1;
    }

    // Build the scene programmatically: one camera, one cube, no JSON.
    auto scene = velk::create_scene();
    scene.set_geometry(velk::aabb::from_size(
        {static_cast<float>(wc.width), static_cast<float>(wc.height)}));
    auto root = velk::create_element();
    auto camera = velk::create_element();
    auto cube = velk::create_element();

    scene.set_root(root);
    scene.add(root, camera);
    scene.add(root, cube);

    // Camera: perspective with an orbit parented at the cube, forward
    // render path so the custom fragment shader actually runs and its
    // output hits the swapchain directly (deferred would route through
    // the G-buffer + lighting compute, hiding the gl_FrontFacing output).
    auto cam_trait = velk::trait::render::create_camera();
    camera.add_trait(cam_trait);
    velk::Camera(cam_trait).set_projection(velk::Projection::Perspective);
    velk::Camera(cam_trait).set_render_path(velk::RenderPath::Forward);

    auto orbit = velk::trait::transform::create_orbit();
    orbit.set_target(cube);
    orbit.set_distance(500.f);
    orbit.set_pitch(25.f);
    orbit.set_yaw(30.f);
    camera.add_trait(orbit);

    // Cube: a 200px cube with a custom shader material that colors by
    // gl_FrontFacing. Uses the standard element vertex shader so the
    // varyings line up.
    auto cube_sz = velk::ui::trait::layout::create_fixed_size(
        velk::dim::px(200.f), velk::dim::px(200.f), velk::dim::px(200.f));
    auto cube_trs = velk::trait::transform::create_trs();
    auto cube_vis = velk::trait::visual::create_cube();

    auto sm = velk::ShaderMaterial(
        ctx.create_shader_material(front_facing_frag, velk::ext::element_vertex_src));
    if (sm) {
        // Configure pipeline via the options attachment — framework
        // compiles the pipeline lazily on first draw with these. Without
        // depth, both near and far sides of the cube would render and
        // the last-drawn per pixel wins — a meaningless mix of front/back
        // classifications. Depth test ensures only the nearest triangle
        // (= the outside face) survives per pixel. cull=None is kept so
        // the winding test visually distinguishes front vs back rather
        // than just culling whichever side is "wrong".
        sm.set_options([](velk::IMaterialOptions::State& mo) {
            mo.cull_mode = velk::CullMode::None;
            mo.depth_test = velk::CompareOp::LessEqual;
            mo.depth_write = true;
        });
        velk::Mesh cube_mesh(ctx.build_cube());
        cube_mesh.set_material(0, sm);
        cube_vis.set_mesh(cube_mesh);
    } else {
        VELK_LOG(E, "winding_test: create_shader_material returned null; test cannot run");
        return 1;
    }
    cube.add_trait(cube_sz);
    cube.add_trait(cube_trs);
    cube.add_trait(cube_vis);

    app.add_view(window, camera);

    // Nudge the orbit yaw over time so the user sees multiple faces of
    // the cube; all visible faces should remain green across rotation.
    float t = 0.f;
    while (app.poll()) {
        t += 0.01f;
        velk::OrbitTrait o(camera.find_trait<velk::IOrbit>());
        if (o) {
            o.set_yaw(t * 20.f);
            o.set_pitch(25.f + 10.f * std::sin(t * 0.7f));
        }
        app.update();
        app.present();
    }
    return 0;
}
