// A small fixed scene for looking at the state of the renderer's structures rather than at a
// lit picture: levels of detail clusters, their size on screen, and, in a second view from
// above, everything the main camera chooses to draw.
//
// velk_ui_inspect [view=NAME] [level=N] [at=X,Y,Z,YAW,PITCH] [path[=FRAME]]
//
// The camera starts at a fixed view under free control: right drag turns, W, A, S and D move,
// the wheel moves along the view. Keys:
//   V  next debug view (lit, cluster, level, cluster size, instance, triangle size, partition,
//      ray tree, ray top tree)
//   K, L  the ray tree views' level down and up
//   P  scripted path on or off, the same motion every time
//   C  logs the camera's place, as at= takes it
//
// Arguments, so that a run can be repeated without the controls:
//   view=NAME   starts in that debug view: lit, cluster, level, size, instance, triangles,
//               partition, tree or top
//   level=N     the ray tree views' level
//   at=...      starts at that place instead of the fixed view
//   path        starts on the scripted path; path=FRAME starts it at that frame of it
// With VELK_AUTO_EXIT_FRAMES and VELK_AUTO_CAPTURE the window is saved and the app exits.
//
// Up is -y, as in the other samples.

#include <velk/api/velk.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <velk-render/api/material/standard_material.h>
#include <velk-render/api/render_context.h>
#include <velk-render/api/shadow_technique.h>
#include <velk-render/blas.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-runtime/api/application.h>
#include <velk-scene/api/camera.h>
#include <velk-scene/api/element.h>
#include <velk-scene/api/mesh.h>
#include <velk-scene/api/post_process.h>
#include <velk-scene/api/render_path.h>
#include <velk-scene/api/scene.h>
#include <velk-scene/api/trait/light.h>
#include <velk-scene/api/trait/orbit.h>
#include <velk-scene/api/trait/trs.h>
#include <velk-scene/api/visual/instanced_mesh.h>
#include <velk-scene/api/visual/mesh.h>
#include <velk-ui/api/trait/fixed_size.h>
#include <velk-ui/api/visual/rect.h>
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/api/text_visual.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Vertex
{
    float position[3];
    float normal[3];
    float uv[2];
    float tangent[4];
};

constexpr velk::VertexAttribute kAttributes[] = {
    {velk::VertexAttributeSemantic::Position, velk::VertexAttributeFormat::Float3, 0},
    {velk::VertexAttributeSemantic::Normal, velk::VertexAttributeFormat::Float3, 12},
    {velk::VertexAttributeSemantic::TexCoord0, velk::VertexAttributeFormat::Float2, 24},
    {velk::VertexAttributeSemantic::Tangent, velk::VertexAttributeFormat::Float4, 32},
};

velk::IMesh::Ptr build(velk::IRenderContext& ctx, const velk::vector<Vertex>& vertices, const velk::vector<uint32_t>& indices,
                       const velk::IMaterial::Ptr& material)
{
    velk::vec3 lo{1e30f, 1e30f, 1e30f};
    velk::vec3 hi{-1e30f, -1e30f, -1e30f};
    for (const Vertex& v : vertices) {
        lo = {std::min(lo.x, v.position[0]), std::min(lo.y, v.position[1]), std::min(lo.z, v.position[2])};
        hi = {std::max(hi.x, v.position[0]), std::max(hi.y, v.position[1]), std::max(hi.z, v.position[2])};
    }
    velk::aabb bounds{};
    bounds.position = lo;
    bounds.extent = {hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
    const auto vertex_count = static_cast<uint32_t>(vertices.size());
    const auto index_count = static_cast<uint32_t>(indices.size());
    auto& builder = ctx.get_mesh_builder();
    auto primitive = builder.build_primitive({kAttributes, 4}, sizeof(Vertex), vertices.data(), vertex_count, indices.data(),
                                             index_count, velk::MeshTopology::TriangleList, bounds);
    primitive->set_rt_blas(velk::build_mesh_blas(reinterpret_cast<const float*>(vertices.data()), sizeof(Vertex), vertex_count,
                                                 indices.data(), index_count / 3));
    velk::MeshPrimitive(primitive).set_material(material);
    velk::IMeshPrimitive::Ptr primitives[] = {primitive};
    return builder.build({primitives, 1});
}

/// Adds a flat grid of cells by cells quads, from @p origin along @p u and @p v, facing @p n.
void add_grid(velk::vector<Vertex>& vertices, velk::vector<uint32_t>& indices, const velk::vec3& origin,
              const velk::vec3& u, const velk::vec3& v, const velk::vec3& n, uint32_t cells)
{
    const auto first = static_cast<uint32_t>(vertices.size());
    for (uint32_t j = 0; j <= cells; ++j) {
        for (uint32_t i = 0; i <= cells; ++i) {
            const float a = static_cast<float>(i) / static_cast<float>(cells);
            const float b = static_cast<float>(j) / static_cast<float>(cells);
            const velk::vec3 p = origin + u * a + v * b;
            vertices.push_back({{p.x, p.y, p.z}, {n.x, n.y, n.z}, {a, b}, {u.x, u.y, u.z, 1.f}});
        }
    }
    // Wound so that the face is seen from where n points.
    const velk::vec3 c{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    const bool along = c.x * n.x + c.y * n.y + c.z * n.z > 0.f;
    const uint32_t stride = cells + 1;
    for (uint32_t j = 0; j < cells; ++j) {
        for (uint32_t i = 0; i < cells; ++i) {
            const uint32_t i00 = first + j * stride + i;
            const uint32_t i10 = i00 + 1;
            const uint32_t i01 = i00 + stride;
            const uint32_t i11 = i01 + 1;
            for (uint32_t k : along ? std::initializer_list<uint32_t>{i00, i10, i11, i00, i11, i01}
                                    : std::initializer_list<uint32_t>{i00, i11, i10, i00, i01, i11}) {
                indices.push_back(k);
            }
        }
    }
}

/// A flat square on y = 0, @p size across, centered on the origin, facing up (-y).
velk::IMesh::Ptr build_ground(velk::IRenderContext& ctx, float size, uint32_t cells, const velk::IMaterial::Ptr& material)
{
    velk::vector<Vertex> vertices;
    velk::vector<uint32_t> indices;
    add_grid(vertices, indices, {-size * 0.5f, 0.f, -size * 0.5f}, {size, 0.f, 0.f}, {0.f, 0.f, size}, {0.f, -1.f, 0.f}, cells);
    return build(ctx, vertices, indices, material);
}

/// A box standing on y = 0, centered on x and z, each face a grid of cells by cells.
velk::IMesh::Ptr build_box(velk::IRenderContext& ctx, const velk::vec3& size, uint32_t cells, const velk::IMaterial::Ptr& material)
{
    velk::vector<Vertex> vertices;
    velk::vector<uint32_t> indices;
    const float x0 = -size.x * 0.5f;
    const float z0 = -size.z * 0.5f;
    const float y0 = -size.y;
    const velk::vec3 ex{size.x, 0.f, 0.f};
    const velk::vec3 ey{0.f, size.y, 0.f};
    const velk::vec3 ez{0.f, 0.f, size.z};
    add_grid(vertices, indices, {x0, y0, z0}, ex, ez, {0.f, -1.f, 0.f}, cells);
    add_grid(vertices, indices, {x0, 0.f, z0}, ex, ez, {0.f, 1.f, 0.f}, cells);
    add_grid(vertices, indices, {x0, y0, z0}, ex, ey, {0.f, 0.f, -1.f}, cells);
    add_grid(vertices, indices, {x0, y0, -z0}, ex, ey, {0.f, 0.f, 1.f}, cells);
    add_grid(vertices, indices, {x0, y0, z0}, ez, ey, {-1.f, 0.f, 0.f}, cells);
    add_grid(vertices, indices, {-x0, y0, z0}, ez, ey, {1.f, 0.f, 0.f}, cells);
    return build(ctx, vertices, indices, material);
}

/// A lumpy rock @p size across standing on y = 0, from @p segments around and @p rings down.
velk::IMesh::Ptr build_rock(velk::IRenderContext& ctx, float size, uint32_t segments, uint32_t rings,
                            const velk::IMaterial::Ptr& material)
{
    const float radius = size * 0.5f;
    const auto surface = [&](float theta, float phi) {
        const velk::vec3 d{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
        const float lumps = 0.22f * std::sin(d.x * 3.1f + 0.7f) * std::cos(d.z * 2.7f) + 0.11f * std::sin(d.y * 7.3f + d.x * 5.1f)
                            + 0.05f * std::sin(d.z * 17.f + 1.3f) * std::sin(d.x * 19.f + d.y * 13.f);
        const float r = radius * (0.8f + lumps);
        return velk::vec3{d.x * r, -radius + d.y * r, d.z * r};
    };
    velk::vector<Vertex> vertices;
    velk::vector<uint32_t> indices;
    for (uint32_t r = 0; r <= rings; ++r) {
        const float phi = static_cast<float>(r) / static_cast<float>(rings) * kPi;
        for (uint32_t s = 0; s <= segments; ++s) {
            const float theta = static_cast<float>(s) / static_cast<float>(segments) * kPi * 2.f;
            const velk::vec3 p = surface(theta, phi);
            constexpr float kStep = 0.002f;
            const velk::vec3 a = surface(theta + kStep, phi);
            const velk::vec3 b = surface(theta - kStep, phi);
            const velk::vec3 c = surface(theta, std::min(phi + kStep, kPi - kStep));
            const velk::vec3 d = surface(theta, std::max(phi - kStep, kStep));
            const velk::vec3 du = a - b;
            const velk::vec3 dv = c - d;
            velk::vec3 n{du.y * dv.z - du.z * dv.y, du.z * dv.x - du.x * dv.z, du.x * dv.y - du.y * dv.x};
            const velk::vec3 out{p.x, p.y + radius, p.z};
            const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            const float flip = (n.x * out.x + n.y * out.y + n.z * out.z < 0.f ? -1.f : 1.f) / std::max(length, 1e-9f);
            n = n * flip;
            vertices.push_back({{p.x, p.y, p.z}, {n.x, n.y, n.z}, {0.f, 0.f}, {-std::sin(theta), 0.f, std::cos(theta), 1.f}});
        }
    }
    const uint32_t stride = segments + 1;
    const auto add_triangle = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
        const velk::vec3 a{vertices[i0].position[0], vertices[i0].position[1], vertices[i0].position[2]};
        const velk::vec3 b{vertices[i1].position[0], vertices[i1].position[1], vertices[i1].position[2]};
        const velk::vec3 c{vertices[i2].position[0], vertices[i2].position[1], vertices[i2].position[2]};
        const velk::vec3 e1 = b - a;
        const velk::vec3 e2 = c - a;
        const velk::vec3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
        if (n.x == 0.f && n.y == 0.f && n.z == 0.f) {
            return;
        }
        const bool outward = n.x * a.x + n.y * (a.y + radius) + n.z * a.z > 0.f;
        indices.push_back(i0);
        indices.push_back(outward ? i1 : i2);
        indices.push_back(outward ? i2 : i1);
    };
    for (uint32_t r = 0; r < rings; ++r) {
        for (uint32_t s = 0; s < segments; ++s) {
            const uint32_t i00 = r * stride + s;
            add_triangle(i00, i00 + stride, i00 + stride + 1);
            add_triangle(i00, i00 + stride + 1, i00 + 1);
        }
    }
    return build(ctx, vertices, indices, material);
}

/// A value from -0.5 to 0.5 that depends only on @p i and @p salt.
float jitter(long i, float salt)
{
    const float x = std::sin(static_cast<float>(i) * 12.9898f + salt * 78.233f) * 43758.5453f;
    return x - std::floor(x) - 0.5f;
}

/// Adds a mesh to the scene at @p at.
void add_mesh(velk::Scene& scene, velk::Element& root, const velk::IMesh::Ptr& mesh, const velk::vec3& at)
{
    auto element = velk::create_element();
    auto trs = velk::trait::transform::create_trs();
    trs.set_translate(at);
    auto visual = velk::trait::visual::create_mesh();
    visual.set_mesh(velk::Mesh(mesh));
    element.add_trait(trs);
    element.add_trait(visual);
    scene.add(root, element);
}

/// A camera that flies: an orbit at distance 1 around a focus that moves.
struct FlyCamera
{
    velk::Element element;
    velk::Camera camera;
    velk::OrbitTrait orbit;
    velk::Trs focus;
};

FlyCamera add_camera(velk::Scene& scene, velk::Element& root)
{
    FlyCamera fly;
    fly.element = velk::create_element();
    auto focus = velk::create_element();
    fly.focus = velk::trait::transform::create_trs();
    focus.add_trait(fly.focus);
    scene.add(root, focus);
    scene.add(root, fly.element);

    auto cam_trait = velk::trait::render::create_camera();
    fly.camera = velk::Camera(cam_trait);
    fly.camera.set_projection(velk::Projection::Perspective);
    fly.camera.set_fov(50.f);
    fly.camera.add_render_path(velk::path::create_deferred());
    auto post = velk::pp::create_post_process();
    post.add(velk::pp::create_tonemap());
    fly.camera.add_post_process(post);
    fly.element.add_trait(cam_trait);

    fly.orbit = velk::trait::transform::create_orbit();
    fly.orbit.set_target(focus);
    fly.orbit.set_distance(1.f);
    fly.element.add_trait(fly.orbit);
    fly.element.find_trait<velk::ICamera>()->far_clip().set_value(40000.f);
    return fly;
}

/// Where a fly camera looks, from its orbit's yaw and pitch in degrees.
velk::vec3 forward_of(float yaw_degrees, float pitch_degrees)
{
    const float yaw = yaw_degrees * kPi / 180.f;
    const float pitch = pitch_degrees * kPi / 180.f;
    return {-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

struct NamedView
{
    velk::DebugView view;
    const char* key;     ///< As view= takes it.
    const char* name;
    const char* legend;  ///< What the colours mean.
};

constexpr NamedView kViews[] = {
    {velk::DebugView::None, "lit", "lit", "the lit image"},
    {velk::DebugView::Cluster, "cluster", "cluster",
     "a colour per level of detail cluster of each instance; grey: not drawn by cluster"},
    {velk::DebugView::Level, "level", "level",
     "level of detail: red full detail, then orange, yellow, green, cyan, blue, purple, pink"},
    {velk::DebugView::ClusterSize, "size", "cluster size",
     "cluster width on the main camera's screen: blue <8 px, cyan <16, green <32, yellow <64, orange <128, red more"},
    {velk::DebugView::Instance, "instance", "instance", "a colour per instance"},
    {velk::DebugView::TriangleSize, "triangles", "triangle size",
     "triangle size on screen: magenta drawn in compute, red <0.5 px, yellow <1, green <2, cyan <8, blue more"},
    {velk::DebugView::Partition, "partition", "partition",
     "partition bounds: red while a partition chooses its levels of detail again, cyan while it keeps them"},
    {velk::DebugView::RayTree, "tree", "ray tree",
     "boxes of one level of the trees rays walk over each partition (K and L for the level): 0 four clusters each"},
    {velk::DebugView::RayTop, "top", "ray top tree",
     "boxes of one level of the tree rays walk over the partitions (K and L for the level): 0 one partition each"},
    {velk::DebugView::Quads, "quads", "quads",
     "the partitions' quads alone, one per chunk of 64 members, in their colour"},
};
constexpr size_t kViewCount = sizeof(kViews) / sizeof(kViews[0]);

} // namespace

int main(int argc, char* argv[])
{
    size_t start_view = 0;
    bool has_place = false;
    float place[5] = {};
    bool start_scripted = false;
    long start_frame = 0;
    uint32_t tree_level = 0;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strncmp(arg, "level=", 6) == 0) {
            tree_level = static_cast<uint32_t>(std::atol(arg + 6));
        } else if (std::strncmp(arg, "view=", 5) == 0) {
            bool known = false;
            for (size_t v = 0; v < kViewCount; ++v) {
                if (std::strcmp(arg + 5, kViews[v].key) == 0) {
                    start_view = v;
                    known = true;
                }
            }
            if (!known) {
                VELK_LOG(E, "inspect: unknown view '%s'", arg + 5);
            }
        } else if (std::strncmp(arg, "at=", 3) == 0) {
            has_place = std::sscanf(arg + 3, "%f,%f,%f,%f,%f", &place[0], &place[1], &place[2], &place[3], &place[4]) == 5;
            if (!has_place) {
                VELK_LOG(E, "inspect: at= takes X,Y,Z,YAW,PITCH");
            }
        } else if (std::strcmp(arg, "path") == 0) {
            start_scripted = true;
        } else if (std::strncmp(arg, "path=", 5) == 0) {
            start_scripted = true;
            start_frame = std::atol(arg + 5);
        } else {
            VELK_LOG(E, "inspect: unknown argument '%s'", arg);
        }
    }

    velk::ApplicationConfig config;
    velk::WindowConfig wc;
    wc.width = 1280;
    wc.height = 720;
    wc.title = "velk-ui inspect";
    wc.depth = velk::DepthFormat::Default;
    wc.color_format = velk::SurfaceColorFormat::RGBA16F;

    auto app = velk::create_app(config);
    auto window = app.create_window(wc);
    if (!window) {
        VELK_LOG(E, "inspect: failed to create window");
        return 1;
    }
    auto ctx = app.render_context();
    auto render_context = static_cast<velk::IRenderContext::Ptr>(ctx);
    if (!render_context) {
        VELK_LOG(E, "inspect: no render context");
        return 1;
    }

    auto scene = velk::create_scene();
    scene.set_geometry(velk::aabb::from_size({static_cast<float>(wc.width), static_cast<float>(wc.height)}));
    auto root = velk::create_element();
    scene.set_root(root);

    // The main camera starts at a fixed view; the overview hangs above it, looking down, and
    // draws what the main camera draws.
    FlyCamera main_camera = add_camera(scene, root);
    FlyCamera overview = add_camera(scene, root);
    overview.camera.set_choose_as(main_camera.element);
    constexpr float kOverviewHeight = 2500.f;

    const velk::vec3 start_at{0.f, -60.f, 300.f};
    constexpr float kStartYaw = 0.f;
    constexpr float kStartPitch = -8.f;
    main_camera.focus.set_translate(has_place ? velk::vec3{place[0], place[1], place[2]} : start_at);
    main_camera.orbit.set_yaw(has_place ? place[3] : kStartYaw);
    main_camera.orbit.set_pitch(has_place ? place[4] : kStartPitch);
    overview.orbit.set_pitch(-89.f);

    // Sun, fixed.
    {
        auto light = velk::trait::render::create_directional_light(velk::color{1.f, 0.96f, 0.88f, 1.f}, 2.5f);
        light.add_technique(velk::technique::create_rt_shadow());
        auto sun = velk::create_element();
        auto trs = velk::trait::transform::create_trs();
        trs.set_rotation({35.f, 25.f, 0.f});
        sun.add_trait(trs);
        sun.add_trait(light);
        scene.add(root, sun);
    }

    auto ground_material = velk::material::create_standard(velk::color{0.45f, 0.42f, 0.38f, 1.f}, 0.f, 0.9f);
    auto rock_material = velk::material::create_standard(velk::color{0.55f, 0.5f, 0.44f, 1.f}, 0.f, 0.8f);
    auto pebble_material = velk::material::create_standard(velk::color{0.35f, 0.42f, 0.25f, 1.f}, 0.f, 0.8f);
    auto wall_material = velk::material::create_standard(velk::color{0.6f, 0.58f, 0.55f, 1.f}, 0.f, 0.7f);

    // A flat ground: flat areas simplify at no error, and its full detail runs out up close.
    add_mesh(scene, root, build_ground(*render_context, 8000.f, 256, ground_material), {0.f, 0.f, 0.f});

    // Dense rocks at rising distances along the view.
    const velk::IMesh::Ptr rock = build_rock(*render_context, 60.f, 160, 80, rock_material);
    for (float z : {150.f, -100.f, -600.f, -1600.f, -3500.f}) {
        add_mesh(scene, root, rock, {-60.f, 0.f, z});
    }

    // A field of many small things, the grass case: small pebbles on a jittered grid.
    {
        constexpr long kColumns = 200;
        constexpr float kSpacing = 9.f;
        const velk::vec3 corner{40.f, 0.f, -1500.f};
        velk::vector<velk::InstanceTransform> transforms;
        transforms.reserve(static_cast<size_t>(kColumns * kColumns));
        for (long i = 0; i < kColumns * kColumns; ++i) {
            const float x = corner.x + (static_cast<float>(i % kColumns) + jitter(i, 1.3f)) * kSpacing;
            const float z = corner.z + (static_cast<float>(i / kColumns) + jitter(i, 2.9f)) * kSpacing;
            const float s = 0.6f + jitter(i, 4.1f) * 0.6f;
            const float turn = (jitter(i, 7.7f) + 0.5f) * 2.f * kPi;
            const float c = std::cos(turn) * s;
            const float n = std::sin(turn) * s;
            transforms.push_back(velk::InstanceTransform{{{c, 0.f, n, x}, {0.f, s, 0.f, 0.f}, {-n, 0.f, c, z}}});
        }
        auto field = velk::trait::visual::create_instanced_mesh();
        field.set_mesh(velk::Mesh(build_rock(*render_context, 5.f, 16, 8, pebble_material)));
        field.add_instances({transforms.data(), transforms.size()});
        auto element = velk::create_element();
        element.add_trait(field);
        scene.add(root, element);
    }

    // One large wall throwing a shadow across the field and the ground.
    add_mesh(scene, root, build_box(*render_context, {900.f, 260.f, 30.f}, 24, wall_material), {500.f, 0.f, -700.f});

    app.add_view(window, main_camera.element);
    app.add_view(window, overview.element, {0.66f, 0.f, 0.34f, 0.4f});

    // What is shown and what its colours mean, in a 2D scene of its own drawn last, over the
    // bottom left of the window.
    auto overlay_scene = velk::create_scene();
    overlay_scene.set_geometry(velk::aabb::from_size({static_cast<float>(wc.width), static_cast<float>(wc.height)}));
    auto overlay_root = velk::create_element();
    overlay_scene.set_root(overlay_root);
    auto overlay_camera = velk::create_element();
    overlay_scene.add(overlay_root, overlay_camera);
    auto overlay_camera_trait = velk::trait::render::create_camera();
    overlay_camera_trait.set_projection(velk::Projection::Ortho);
    overlay_camera.add_trait(overlay_camera_trait);
    constexpr float kTextWidth = 760.f;
    constexpr float kTextHeight = 112.f;
    auto text_element = velk::create_element();
    auto text_trs = velk::trait::transform::create_trs();
    text_trs.set_translate({8.f, static_cast<float>(wc.height) - kTextHeight - 8.f, 0.f});
    text_element.add_trait(text_trs);
    text_element.add_trait(velk::ui::trait::layout::create_fixed_size(velk::dim::px(kTextWidth), velk::dim::px(kTextHeight)));
    auto text_background = velk::ui::trait::visual::create_rect();
    text_background.set_color({0.f, 0.f, 0.f, 0.55f});
    text_element.add_trait(text_background);
    auto text = velk::ui::trait::visual::create_text();
    text.set_font(velk::ui::get_default_font());
    text.set_font_size(14.f);
    text.set_color({1.f, 1.f, 0.f, 1.f});
    text.set_layout(velk::TextLayout::MultiLine);
    text_element.add_trait(text);
    overlay_scene.add(overlay_root, text_element);
    app.add_view(window, overlay_camera);

    size_t view_index = 0;
    bool scripted = start_scripted;
    bool text_stale = true;
    velk::vec3 shown_at{};
    float shown_yaw = 0.f;
    float shown_pitch = 0.f;
    const auto show_text = [&](const velk::vec3& at, float yaw, float pitch) {
        char level[32] = "";
        if (kViews[view_index].view == velk::DebugView::RayTree || kViews[view_index].view == velk::DebugView::RayTop) {
            std::snprintf(level, sizeof(level), ", level %u", tree_level);
        }
        char buf[768];
        std::snprintf(buf, sizeof(buf),
                      "view: %s%s (V for next)\n%s\n"
                      "top right, from above: all the main camera's kept cut holds, dimmed outside its view (yellow lines)\n"
                      "camera: %s (P), at %.0f, %.0f, %.0f, yaw %.0f, pitch %.0f\n"
                      "right drag turns, W A S D move, wheel moves along the view, C logs the place",
                      kViews[view_index].name, level, kViews[view_index].legend, scripted ? "scripted path" : "free", at.x,
                      at.y, at.z, yaw, pitch);
        text.set_text(velk::string(buf));
        shown_at = at;
        shown_yaw = yaw;
        shown_pitch = pitch;
    };
    const auto show_view = [&](size_t index) {
        view_index = index % kViewCount;
        text_stale = true;
        main_camera.camera.set_debug_view(kViews[view_index].view);
        overview.camera.set_debug_view(kViews[view_index].view);
        VELK_LOG(I, "inspect: view %s", kViews[view_index].name);
    };
    const auto show_level = [&](uint32_t level) {
        tree_level = level;
        text_stale = true;
        main_camera.camera.set_debug_level(level);
        overview.camera.set_debug_level(level);
    };

    long scripted_frame = start_frame;
    bool turning = false;
    float last_x = 0.f;
    float last_y = 0.f;
    bool held[4] = {false, false, false, false};

    const auto camera_state = [&]() {
        auto orbit_state = velk::read_state<velk::IOrbit>(static_cast<velk::IOrbit::Ptr>(main_camera.orbit));
        auto focus_state = velk::read_state<velk::ITrs>(static_cast<velk::ITrs::Ptr>(main_camera.focus));
        return std::make_pair(orbit_state, focus_state);
    };
    const auto move = [&](float forward, float right) {
        auto [orbit_state, focus_state] = camera_state();
        if (!orbit_state || !focus_state) {
            return;
        }
        // Faster the higher above the ground.
        const float speed = std::max(60.f, -focus_state->translate.y);
        const velk::vec3 ahead = forward_of(orbit_state->yaw, orbit_state->pitch);
        const float yaw = orbit_state->yaw * kPi / 180.f;
        const velk::vec3 side{std::cos(yaw), 0.f, -std::sin(yaw)};
        main_camera.focus.set_translate(focus_state->translate + ahead * (forward * speed) + side * (right * speed));
    };

    velk::ScopedHandler pointer_sub = window.add_on_pointer_event([&](const velk::ui::PointerEvent& e) {
        if (e.action == velk::ui::PointerAction::Down && e.button == velk::ui::PointerButton::Right) {
            turning = true;
            last_x = e.position.x;
            last_y = e.position.y;
        } else if (e.action == velk::ui::PointerAction::Up && e.button == velk::ui::PointerButton::Right) {
            turning = false;
        } else if (e.action == velk::ui::PointerAction::Move && turning && !scripted) {
            auto [orbit_state, focus_state] = camera_state();
            if (orbit_state) {
                main_camera.orbit.set_yaw(orbit_state->yaw - (e.position.x - last_x) * 0.2f);
                main_camera.orbit.set_pitch(std::clamp(orbit_state->pitch - (e.position.y - last_y) * 0.2f, -89.f, 89.f));
            }
            last_x = e.position.x;
            last_y = e.position.y;
        }
    });
    velk::ScopedHandler scroll_sub(window.input()->on_scroll_event(), [&](const velk::ui::ScrollEvent& e) {
        if (!scripted) {
            move(e.delta.y > 0 ? 0.5f : -0.5f, 0.f);
        }
    });
    velk::ScopedHandler key_sub = window.add_on_key_event([&](const velk::ui::KeyEvent& e) {
        const bool down = e.action != velk::ui::KeyAction::Up;
        // W, S, A and D.
        if (e.key == 87) held[0] = down;
        if (e.key == 83) held[1] = down;
        if (e.key == 65) held[2] = down;
        if (e.key == 68) held[3] = down;
        if (e.action != velk::ui::KeyAction::Down) {
            return;
        }
        if (e.key == 86) {  // V
            show_view(view_index + 1);
        } else if (e.key == 76) {  // L
            show_level(tree_level + 1);
        } else if (e.key == 75) {  // K
            show_level(tree_level > 0 ? tree_level - 1 : 0);
        } else if (e.key == 80) {  // P
            scripted = !scripted;
            scripted_frame = 0;
            text_stale = true;
            VELK_LOG(I, "inspect: scripted path %s", scripted ? "on" : "off");
        } else if (e.key == 67) {  // C
            auto [orbit_state, focus_state] = camera_state();
            if (orbit_state && focus_state) {
                VELK_LOG(I, "inspect: at=%.1f,%.1f,%.1f,%.1f,%.1f", focus_state->translate.x, focus_state->translate.y,
                         focus_state->translate.z, orbit_state->yaw, orbit_state->pitch);
            }
        }
    });

    show_view(start_view);
    show_level(tree_level);
    while (app.poll()) {
        if (scripted) {
            // Out along the view and back, rising and falling, the same every time.
            const float t = static_cast<float>(scripted_frame++) / 60.f;
            const float along = 0.5f - 0.5f * std::cos(t * 0.15f);
            main_camera.focus.set_translate({std::sin(t * 0.21f) * 250.f, -60.f - 400.f * along * along, 300.f - 3800.f * along});
            main_camera.orbit.set_yaw(kStartYaw + std::sin(t * 0.33f) * 40.f);
            main_camera.orbit.set_pitch(kStartPitch - 12.f * along);
        } else {
            constexpr float kStepSeconds = 1.f / 60.f;
            move(((held[0] ? 1.f : 0.f) - (held[1] ? 1.f : 0.f)) * kStepSeconds,
                 ((held[3] ? 1.f : 0.f) - (held[2] ? 1.f : 0.f)) * kStepSeconds);
        }
        // The overview follows the main camera from above, turned the same way.
        auto [orbit_state, focus_state] = camera_state();
        if (orbit_state && focus_state) {
            overview.focus.set_translate(focus_state->translate + velk::vec3{0.f, -kOverviewHeight, 0.f});
            overview.orbit.set_yaw(orbit_state->yaw);
            // The text again when the view or mode changed, or the camera moved or turned a little.
            const velk::vec3 moved = focus_state->translate - shown_at;
            if (text_stale || std::abs(moved.x) + std::abs(moved.y) + std::abs(moved.z) > 5.f
                || std::abs(orbit_state->yaw - shown_yaw) > 1.f || std::abs(orbit_state->pitch - shown_pitch) > 1.f) {
                show_text(focus_state->translate, orbit_state->yaw, orbit_state->pitch);
                text_stale = false;
            }
        }
        app.update();
        app.present();
    }
    return 0;
}
