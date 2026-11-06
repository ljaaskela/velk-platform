// Scale test for moving and deforming content. Builds its scene in code, so
// it loads no assets.
//
// Usage: velk_ui_scale [elements] [movers] [frames]
//                      [translate|rotate|group|escape|burst|churn|recolor|deform|scatter
//                       |forest|forest_elements|forest_shift|forest_churn]
//                      [unlimited] [rt] [text]
//
// elements: visuals in the grid, an equal mix of triangle meshes, rects, cubes
//           and spheres. Default 1000.
// movers:   how many of them move every frame. Default 10.
// frames:   exits after this many frames. Default 0, which runs until the
//           window closes.
// rotate:   movers rotate in place, with no change in translation. The default
//           is translate.
// group:    movers translate a parent that has no visual, and the visual sits on
//           its child.
// escape:   movers rise far above the scene and come back, which grows the GI
//           probe volume and then shrinks it. The first 190 frames show both.
// burst:    movers translate for 200 frames, then stay still for 200, and repeat.
// churn:    movers translate, and every 20 frames one extra cube is removed and
//           another added.
// recolor:  movers translate, and at frame 10 the rects turn from grey to red with
//           no change in geometry.
// deform:   movers are spheres of about 65,000 triangles with a ripple running over
//           them. The perf log has the cost under renderer.deform.
// scatter:  like deform, and the vertices also fly apart, until the trees degrade
//           enough to be built again, shown under mesh.blas_rebuild.
// forest:   elements is the number of trees, all on one instanced visual, on a
//           jittered grid instead of the mixed visuals. Movers are trees moved every
//           frame. Trees have two primitives and 24 triangles.
// forest_elements: the same trees as one element each, to compare against forest.
// forest_shift: forest, with the whole element translated every frame.
// forest_churn: forest, with one tree removed and another added every frame.
// unlimited: turns vsync off, so frame times show the real cost.
// rt:       uses the ray-traced render path instead of deferred.
// text:     adds a text visual above the grid. It moves like the movers when
//           there are any.
//
// Prints each GI probe volume change as it happens, and GPU pass timings on
// exit. The engine's perf log, also printed on exit,
// has the BVH timings under renderer.bvh_*. The first frames are excluded from
// the GPU timings because they include pipeline creation.

#include <velk/api/velk.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <velk-render/api/material/standard_material.h>
#include <velk-render/api/render_context.h>
#include <velk-render/api/shadow_technique.h>
#include <velk-render/blas.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-render/interface/intf_render_backend.h>
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
#include <velk-scene/api/visual/cube.h>
#include <velk-scene/api/visual/instanced_mesh.h>
#include <velk-scene/api/visual/mesh.h>
#include <velk-scene/api/visual/sphere.h>
#include <velk-scene/ext/trait.h>
#include <velk-ui/api/trait/fixed_size.h>
#include <velk-ui/api/visual/rect.h>
#include <velk-ui/plugins/text/api/text_visual.h>

namespace {

constexpr float kElementSize = 50.f;
constexpr float kElementSpacing = 80.f;
constexpr float kFloorY = 200.f;
constexpr long kWarmupFrames = 10;

struct Vertex
{
    float position[3];
    float normal[3];
    float uv[2];
    float tangent[4];
};

class IRipple : public velk::Interface<IRipple>
{
public:
    VELK_INTERFACE(
        (PROP, float, time, 0.f),
        (PROP, float, scatter, 0.f)
    )
};

inline constexpr velk::Uid kRippleClass{"d864fbf7-9279-47f3-8dda-319b1afc8f6f"};

/// Runs a ripple along the normals, and moves each vertex away by its own offset.
class Ripple : public velk::ext::MeshDeformer<Ripple, IRipple>
{
public:
    VELK_CLASS_UID(kRippleClass, "ScaleRipple");

    void deform(velk::array_view<const velk::MeshVertex> rest, velk::MeshVertex* vertices) override
    {
        constexpr float kAmount = 4.f;
        constexpr float kWavesPerUnit = 0.25f;
        auto state = velk::read_state<IRipple>(this);
        for (size_t i = 0; i < rest.size(); ++i) {
            const velk::vec3 p = rest[i].position;
            const float wave = std::sin(p.y * kWavesPerUnit + state->time * 4.f);
            // Three unrelated values per vertex, from -1 to 1.
            const auto f = static_cast<float>(i);
            const velk::vec3 away{std::sin(f * 12.9898f), std::sin(f * 78.233f), std::sin(f * 37.719f)};
            vertices[i].position = p + rest[i].normal * (wave * kAmount) + away * state->scatter;
        }
    }
};

/// Builds a triangle mesh sphere with its own BLAS. The element's size does not
/// scale a mesh visual, so its vertices are in world units.
velk::IMesh::Ptr build_triangle_mesh(velk::IRenderContext& ctx, float size, uint32_t segments = 16,
                                     uint32_t rings = 8)
{
    constexpr float kPi = 3.14159265358979323846f;
    const float radius = size * 0.5f;

    velk::vector<Vertex> vertices;
    velk::vector<uint32_t> indices;

    for (uint32_t r = 0; r <= rings; ++r) {
        const float vy = static_cast<float>(r) / rings;
        const float phi = vy * kPi;
        for (uint32_t s = 0; s <= segments; ++s) {
            const float vx = static_cast<float>(s) / segments;
            const float theta = vx * kPi * 2.f;
            const float nx = std::sin(phi) * std::cos(theta);
            const float ny = std::cos(phi);
            const float nz = std::sin(phi) * std::sin(theta);
            vertices.push_back({{radius + radius * nx, radius + radius * ny, radius + radius * nz},
                                {nx, ny, nz},
                                {vx, vy},
                                {-std::sin(theta), 0.f, std::cos(theta), 1.f}});
        }
    }

    const uint32_t stride = segments + 1;
    for (uint32_t r = 0; r < rings; ++r) {
        for (uint32_t s = 0; s < segments; ++s) {
            const uint32_t i00 = r * stride + s;
            const uint32_t i10 = i00 + 1;
            const uint32_t i01 = i00 + stride;
            const uint32_t i11 = i01 + 1;
            for (uint32_t i : {i00, i01, i11, i00, i11, i10}) {
                indices.push_back(i);
            }
        }
    }

    static constexpr velk::VertexAttribute kAttributes[] = {
        {velk::VertexAttributeSemantic::Position, velk::VertexAttributeFormat::Float3, 0},
        {velk::VertexAttributeSemantic::Normal, velk::VertexAttributeFormat::Float3, 12},
        {velk::VertexAttributeSemantic::TexCoord0, velk::VertexAttributeFormat::Float2, 24},
        {velk::VertexAttributeSemantic::Tangent, velk::VertexAttributeFormat::Float4, 32},
    };

    velk::aabb bounds{};
    bounds.position = {0.f, 0.f, 0.f};
    bounds.extent = {size, size, size};

    const auto vertex_count = static_cast<uint32_t>(vertices.size());
    const auto index_count = static_cast<uint32_t>(indices.size());

    auto& builder = ctx.get_mesh_builder();
    auto primitive = builder.build_primitive({kAttributes, 4}, sizeof(Vertex), vertices.data(), vertex_count,
                                             indices.data(), index_count, velk::MeshTopology::TriangleList, bounds);
    if (!primitive) {
        return nullptr;
    }
    primitive->set_rt_blas(velk::build_mesh_blas(reinterpret_cast<const float*>(vertices.data()), sizeof(Vertex),
                                                 vertex_count, indices.data(), index_count / 3));
    velk::IMeshPrimitive::Ptr primitives[] = {primitive};
    return builder.build({primitives, 1});
}

/// Builds a low-poly tree standing on y = 0 and growing towards -y: a trunk and a
/// crown, as a primitive each.
velk::IMesh::Ptr build_tree_mesh(velk::IRenderContext& ctx, float height, const velk::IMaterial::Ptr& trunk,
                                 const velk::IMaterial::Ptr& crown)
{
    constexpr uint32_t kSides = 6;
    constexpr float kPi = 3.14159265358979323846f;

    static constexpr velk::VertexAttribute kAttributes[] = {
        {velk::VertexAttributeSemantic::Position, velk::VertexAttributeFormat::Float3, 0},
        {velk::VertexAttributeSemantic::Normal, velk::VertexAttributeFormat::Float3, 12},
        {velk::VertexAttributeSemantic::TexCoord0, velk::VertexAttributeFormat::Float2, 24},
        {velk::VertexAttributeSemantic::Tangent, velk::VertexAttributeFormat::Float4, 32},
    };

    auto& builder = ctx.get_mesh_builder();
    const auto build_cone = [&](float bottom_y, float top_y, float bottom_radius, float top_radius,
                                const velk::IMaterial::Ptr& material) -> velk::IMeshPrimitive::Ptr {
        velk::vector<Vertex> vertices;
        velk::vector<uint32_t> indices;
        for (uint32_t s = 0; s < kSides; ++s) {
            const float theta = static_cast<float>(s) / kSides * kPi * 2.f;
            const float c = std::cos(theta);
            const float z = std::sin(theta);
            vertices.push_back({{c * bottom_radius, bottom_y, z * bottom_radius}, {c, 0.f, z}, {0.f, 0.f}, {-z, 0.f, c, 1.f}});
            vertices.push_back({{c * top_radius, top_y, z * top_radius}, {c, -0.5f, z}, {0.f, 1.f}, {-z, 0.f, c, 1.f}});
        }
        for (uint32_t s = 0; s < kSides; ++s) {
            const uint32_t a = s * 2;
            const uint32_t b = (s + 1) % kSides * 2;
            for (uint32_t i : {a, a + 1, b + 1, a, b + 1, b}) {
                indices.push_back(i);
            }
        }
        velk::vec3 lo{-bottom_radius, std::min(bottom_y, top_y), -bottom_radius};
        velk::aabb bounds{};
        bounds.position = lo;
        bounds.extent = {bottom_radius * 2.f, std::abs(top_y - bottom_y), bottom_radius * 2.f};
        const auto vertex_count = static_cast<uint32_t>(vertices.size());
        const auto index_count = static_cast<uint32_t>(indices.size());
        auto primitive = builder.build_primitive({kAttributes, 4}, sizeof(Vertex), vertices.data(), vertex_count,
                                                 indices.data(), index_count, velk::MeshTopology::TriangleList, bounds);
        if (primitive) {
            primitive->set_rt_blas(velk::build_mesh_blas(reinterpret_cast<const float*>(vertices.data()), sizeof(Vertex),
                                                         vertex_count, indices.data(), index_count / 3));
            velk::MeshPrimitive(primitive).set_material(material);
        }
        return primitive;
    };

    velk::IMeshPrimitive::Ptr primitives[] = {build_cone(0.f, -height * 0.4f, height * 0.06f, height * 0.04f, trunk),
                                              build_cone(-height * 0.25f, -height, height * 0.25f, height * 0.01f, crown)};
    return builder.build({primitives, 2});
}

/// A position that looks random but follows from @p i, so runs are comparable.
float forest_jitter(long i, float seed)
{
    const float v = std::sin(static_cast<float>(i) * seed) * 43758.5453f;
    return v - std::floor(v) - 0.5f;
}

/// Median and worst GPU time for one pass across the measured frames.
struct PassSamples
{
    const char* label;
    velk::vector<float> ms;
};

void record_gpu_timings(velk::vector<PassSamples>& passes, velk::array_view<const velk::GpuPassTiming> timings)
{
    for (const auto& timing : timings) {
        PassSamples* found = nullptr;
        for (auto& pass : passes) {
            if (std::strcmp(pass.label, timing.label) == 0) {
                found = &pass;
                break;
            }
        }
        if (!found) {
            passes.push_back({timing.label, {}});
            found = &passes.back();
        }
        found->ms.push_back(timing.ms);
    }
}

void print_gpu_timings(velk::vector<PassSamples>& passes)
{
    for (auto& pass : passes) {
        if (pass.ms.empty()) {
            continue;
        }
        std::sort(pass.ms.begin(), pass.ms.end());
        VELK_LOG(I, "scale: gpu %-24s med=%7.3f ms  max=%7.3f ms", pass.label,
                 static_cast<double>(pass.ms[pass.ms.size() / 2]), static_cast<double>(pass.ms.back()));
    }
}

} // namespace

int main(int argc, char* argv[])
{
    const long element_count = argc > 1 ? std::max(0L, std::strtol(argv[1], nullptr, 10)) : 1000;
    const long mover_count = std::min(element_count, argc > 2 ? std::max(0L, std::strtol(argv[2], nullptr, 10)) : 10L);
    const long frame_limit = argc > 3 ? std::strtol(argv[3], nullptr, 10) : 0;
    const bool rotate_only = argc > 4 && std::strcmp(argv[4], "rotate") == 0;
    const bool group_movers = argc > 4 && std::strcmp(argv[4], "group") == 0;
    const bool escape_movers = argc > 4 && std::strcmp(argv[4], "escape") == 0;
    const bool burst_movers = argc > 4 && std::strcmp(argv[4], "burst") == 0;
    const bool churn_elements = argc > 4 && std::strcmp(argv[4], "churn") == 0;
    const bool recolor_rects = argc > 4 && std::strcmp(argv[4], "recolor") == 0;
    const bool scatter_movers = argc > 4 && std::strcmp(argv[4], "scatter") == 0;
    const bool deform_movers = scatter_movers || (argc > 4 && std::strcmp(argv[4], "deform") == 0);
    const bool forest_shift = argc > 4 && std::strcmp(argv[4], "forest_shift") == 0;
    const bool forest_churn = argc > 4 && std::strcmp(argv[4], "forest_churn") == 0;
    const bool forest_instances = forest_shift || forest_churn || (argc > 4 && std::strcmp(argv[4], "forest") == 0);
    const bool forest_elements = argc > 4 && std::strcmp(argv[4], "forest_elements") == 0;
    const bool forest = forest_instances || forest_elements;
    bool unlimited = false;
    bool ray_traced = false;
    bool with_text = false;
    for (int i = 5; i < argc; ++i) {
        unlimited = unlimited || std::strcmp(argv[i], "unlimited") == 0;
        ray_traced = ray_traced || std::strcmp(argv[i], "rt") == 0;
        with_text = with_text || std::strcmp(argv[i], "text") == 0;
    }

    VELK_LOG(I, "scale: %ld elements, %ld movers (%s)", element_count, mover_count, argc > 4 ? argv[4] : "translate");

    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;

    velk::ApplicationConfig config;

    velk::WindowConfig wc;
    wc.width = kWidth;
    wc.height = kHeight;
    wc.title = "velk-ui scale";
    wc.depth = velk::DepthFormat::Default;
    wc.color_format = velk::SurfaceColorFormat::RGBA16F;
    if (unlimited) {
        wc.update_rate = velk::UpdateRate::Unlimited;
    }

    auto app = velk::create_app(config);
    auto window = app.create_window(wc);
    if (!window) {
        VELK_LOG(E, "scale: failed to create window");
        return 1;
    }

    auto ctx = app.render_context();
    auto render_context = static_cast<velk::IRenderContext::Ptr>(ctx);
    if (!render_context) {
        VELK_LOG(E, "scale: no render context");
        return 1;
    }
    auto backend = render_context->backend();
    if (backend) {
        backend->set_gpu_timing_enabled(true);
    }

    auto scene = velk::create_scene();
    scene.set_geometry(velk::aabb::from_size({static_cast<float>(kWidth), static_cast<float>(kHeight)}));

    auto root = velk::create_element();
    auto camera = velk::create_element();
    scene.set_root(root);
    scene.add(root, camera);

    const long columns = std::max(1L, static_cast<long>(std::ceil(std::sqrt(static_cast<double>(element_count)))));
    const float extent = static_cast<float>(columns) * kElementSpacing;

    auto cam_trait = velk::trait::render::create_camera();
    camera.add_trait(cam_trait);
    velk::Camera cam(cam_trait);
    cam.set_projection(velk::Projection::Perspective);
    cam.set_fov(45.f);
    cam.add_render_path(ray_traced ? velk::path::create_rt() : velk::path::create_deferred());

    auto post = velk::pp::create_post_process();
    post.add(velk::pp::create_tonemap());
    cam.add_post_process(post);

    const float distance = std::max(extent * 1.3f, 800.f);
    auto orbit = velk::trait::transform::create_orbit();
    orbit.set_target(root);
    orbit.set_distance(distance);
    orbit.set_pitch(-30.f);
    camera.add_trait(orbit);
    camera.find_trait<velk::ICamera>()->far_clip().set_value(distance * 10.f);

    // Floor.
    {
        const float half = extent * 0.5f + 400.f;
        auto floor = velk::create_element();
        auto trs = velk::trait::transform::create_trs();
        trs.set_translate({-half, kFloorY, -half});
        trs.set_rotation({90.f, 0.f, 0.f});
        auto visual = velk::ui::trait::visual::create_rect();
        visual.set_paint(velk::material::create_standard(velk::color{0.45f, 0.42f, 0.40f, 1.f}, 0.05f, 0.65f));
        floor.add_trait(velk::ui::trait::layout::create_fixed_size(velk::dim::px(half * 2.f), velk::dim::px(half * 2.f)));
        floor.add_trait(trs);
        floor.add_trait(visual);
        scene.add(root, floor);
    }

    // Sun.
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

    // Grid of mixed visuals.
    auto mesh_material = velk::material::create_standard(velk::color{0.75f, 0.35f, 0.25f, 1.f}, 0.f, 0.5f);
    auto rect_material = velk::material::create_standard(velk::color{0.3f, 0.6f, 0.85f, 1.f}, 0.f, 0.4f);
    auto cube_material = velk::material::create_standard(velk::color{0.9f, 0.7f, 0.35f, 1.f}, 0.f, 0.5f);
    auto sphere_material = velk::material::create_standard(velk::color{0.4f, 0.75f, 0.45f, 1.f}, 0.f, 0.3f);

    velk::Mesh triangle_mesh(build_triangle_mesh(*render_context, kElementSize));
    triangle_mesh.set_material(0, mesh_material);
    velk::Mesh cube_mesh(ctx.build_cube());
    cube_mesh.set_material(0, cube_material);
    velk::Mesh sphere_mesh(ctx.build_sphere());
    sphere_mesh.set_material(0, sphere_material);

    struct Mover
    {
        velk::Trs trs;
        velk::vec3 base;
        float phase;
    };
    velk::vector<Mover> movers;
    movers.reserve(static_cast<size_t>(mover_count));
    velk::vector<velk::ui::RectVisual> rects;
    velk::vector<IRipple::Ptr> ripples;

    velk::Mesh deform_source;
    if (deform_movers) {
        velk::instance().type_registry().register_type<Ripple>();
        deform_source = velk::Mesh(build_triangle_mesh(*render_context, kElementSize, 256, 128));
        deform_source.set_material(0, mesh_material);
    }

    const auto size_px = velk::dim::px(kElementSize);
    const float grid_start = -extent * 0.5f;
    long next_mover = 0;

    // Trees stand on the floor on a jittered grid.
    constexpr float kTreeHeight = 60.f;
    const auto tree_position = [&](long i) {
        return velk::vec3{grid_start + (static_cast<float>(i % columns) + 0.8f * forest_jitter(i, 12.9898f)) * kElementSpacing,
                          kFloorY,
                          grid_start + (static_cast<float>(i / columns) + 0.8f * forest_jitter(i, 78.233f)) * kElementSpacing};
    };
    velk::InstancedMeshVisual forest_visual;
    velk::Trs forest_trs;
    uint32_t forest_extra_slot = 0xFFFFFFFFu;
    if (forest) {
        auto trunk_material = velk::material::create_standard(velk::color{0.3f, 0.2f, 0.12f, 1.f}, 0.f, 0.8f);
        auto crown_material = velk::material::create_standard(velk::color{0.15f, 0.42f, 0.14f, 1.f}, 0.f, 0.7f);
        velk::Mesh tree_mesh(build_tree_mesh(*render_context, kTreeHeight, trunk_material, crown_material));
        if (forest_instances) {
            velk::vector<velk::InstanceTransform> transforms;
            transforms.resize(static_cast<size_t>(element_count));
            for (long i = 0; i < element_count; ++i) {
                transforms[static_cast<size_t>(i)] = velk::InstanceTransform::from(velk::mat4::translate(tree_position(i)));
            }
            forest_visual = velk::trait::visual::create_instanced_mesh();
            forest_visual.set_mesh(tree_mesh);
            forest_visual.add_instances({transforms.data(), transforms.size()});
            auto forest_element = velk::create_element();
            forest_trs = velk::trait::transform::create_trs();
            forest_element.add_trait(forest_trs);
            forest_element.add_trait(forest_visual);
            scene.add(root, forest_element);
        } else {
            for (long i = 0; i < element_count; ++i) {
                auto element = velk::create_element();
                auto trs = velk::trait::transform::create_trs();
                trs.set_translate(tree_position(i));
                auto visual = velk::trait::visual::create_mesh();
                visual.set_mesh(tree_mesh);
                element.add_trait(trs);
                element.add_trait(visual);
                scene.add(root, element);
            }
        }
    }

    for (long i = 0; i < element_count && !forest; ++i) {
        auto element = velk::create_element();
        auto trs = velk::trait::transform::create_trs();
        const velk::vec3 position{grid_start + static_cast<float>(i % columns) * kElementSpacing, -kElementSize,
                                  grid_start + static_cast<float>(i / columns) * kElementSpacing};
        trs.set_translate(position);

        // Movers are spread evenly through the grid.
        const bool is_mover = next_mover < mover_count && i == next_mover * element_count / mover_count;

        velk::Element parent = root;
        if (group_movers && is_mover) {
            parent = velk::create_element();
            parent.add_trait(trs);
            scene.add(root, parent);
        } else {
            element.add_trait(trs);
        }

        switch (deform_movers && is_mover ? 4 : i % 4) {
        case 4: {
            auto visual = velk::trait::visual::create_mesh();
            visual.set_mesh(velk::Mesh(render_context->get_mesh_builder().build_deforming(deform_source)));
            element.add_trait(visual);
            auto ripple = velk::instance().create<IRipple>(Ripple::static_class_id());
            element.add_trait(velk::Trait(interface_pointer_cast<velk::ITrait>(ripple)));
            ripples.push_back(ripple);
            break;
        }
        case 0: {
            auto visual = velk::trait::visual::create_mesh();
            visual.set_mesh(triangle_mesh);
            element.add_trait(visual);
            break;
        }
        case 1: {
            auto visual = velk::ui::trait::visual::create_rect();
            if (recolor_rects) {
                visual.set_color({0.5f, 0.5f, 0.5f, 1.f});
                rects.push_back(visual);
            } else {
                visual.set_paint(rect_material);
            }
            element.add_trait(velk::ui::trait::layout::create_fixed_size(size_px, size_px));
            element.add_trait(visual);
            break;
        }
        case 2: {
            auto visual = velk::trait::visual::create_cube();
            visual.set_mesh(cube_mesh);
            element.add_trait(velk::ui::trait::layout::create_fixed_size(size_px, size_px, size_px));
            element.add_trait(visual);
            break;
        }
        default: {
            auto visual = velk::trait::visual::create_sphere();
            visual.set_mesh(sphere_mesh);
            element.add_trait(velk::ui::trait::layout::create_fixed_size(size_px, size_px, size_px));
            element.add_trait(visual);
            break;
        }
        }
        scene.add(parent, element);

        if (is_mover) {
            if (!deform_movers) {
                movers.push_back({trs, position, static_cast<float>(next_mover)});
            }
            ++next_mover;
        }
    }

    if (with_text) {
        const velk::vec3 position{-400.f, -300.f, 0.f};
        auto text = velk::create_element();
        auto trs = velk::trait::transform::create_trs();
        trs.set_translate(position);
        auto visual = velk::ui::trait::visual::create_text();
        visual.set_text("glyph records");
        visual.set_font_size(110.f);
        visual.set_color({0.95f, 0.55f, 1.f, 1.f});
        text.add_trait(velk::ui::trait::layout::create_fixed_size(velk::dim::px(800.f), velk::dim::px(180.f)));
        text.add_trait(trs);
        text.add_trait(visual);
        scene.add(root, text);
        if (mover_count > 0) {
            movers.push_back({trs, position, 0.f});
        }
    }

    app.add_view(window, camera);

    using Clock = std::chrono::steady_clock;
    auto report_start = Clock::now();
    long frame = 0;
    long report_frame = 0;
    velk::vector<PassSamples> gpu_passes;
    auto scene_ptr = static_cast<velk::IScene::Ptr>(scene);
    uint32_t last_probe_reset = 0;
    bool probe_volume_seen = false;
    velk::vector<velk::Element> extra_cubes;
    long next_extra = 0;

    while (app.poll()) {
        // Motion follows the frame number, so runs are comparable.
        const float t = static_cast<float>(frame) / 60.f;
        const bool movers_still = burst_movers && (frame / 200) % 2 == 1;
        for (auto& mover : movers) {
            if (movers_still) {
                break;
            }
            const float a = t + mover.phase;
            if (rotate_only) {
                mover.trs.set_rotation({a * 30.f, a * 45.f, 0.f});
            } else if (escape_movers) {
                mover.trs.set_translate({mover.base.x, mover.base.y - 3000.f * std::max(0.f, std::sin(t)),
                                         mover.base.z});
            } else {
                mover.trs.set_translate({mover.base.x + std::sin(a) * 30.f, mover.base.y + std::sin(a * 1.7f) * 30.f,
                                         mover.base.z + std::cos(a) * 30.f});
            }
        }

        if (forest_shift) {
            forest_trs.set_translate({std::sin(t) * 100.f, 0.f, 0.f});
        }
        if (forest_churn && element_count > 0) {
            // The slot just freed is reused, so this keeps the same slot in use.
            const auto removed = static_cast<uint32_t>(frame % element_count);
            forest_visual.remove_instance(removed);
            forest_visual.add_instance(velk::mat4::translate(tree_position(removed) + velk::vec3{0.f, 0.f, 20.f}));
            if (forest_extra_slot == 0xFFFFFFFFu) {
                forest_extra_slot = forest_visual.add_instance(velk::mat4::translate({grid_start, kFloorY, grid_start - kElementSpacing}));
            }
        }
        if (forest_instances) {
            for (long m = 0; m < mover_count; ++m) {
                const long i = m * element_count / mover_count;
                const float a = t + static_cast<float>(m);
                forest_visual.set_transform(static_cast<uint32_t>(i),
                                            velk::mat4::translate(tree_position(i) + velk::vec3{std::sin(a) * 20.f, 0.f, std::cos(a) * 20.f}));
            }
        }

        for (auto& ripple : ripples) {
            ripple->time().set_value(t);
            if (scatter_movers) {
                ripple->scatter().set_value(std::min(t, 1.f) * kElementSize * 0.25f);
            }
        }

        if (recolor_rects && frame == 10) {
            for (auto& rect : rects) {
                rect.set_color({1.f, 0.05f, 0.05f, 1.f});
            }
        }

        if (churn_elements && frame % 20 == 0) {
            constexpr size_t kExtraCubes = 8;
            if (extra_cubes.size() == kExtraCubes) {
                scene.remove(extra_cubes.front());
                extra_cubes.erase(extra_cubes.begin());
            }
            auto cube = velk::create_element();
            auto trs = velk::trait::transform::create_trs();
            trs.set_translate({grid_start + static_cast<float>(next_extra % columns) * kElementSpacing,
                               -kElementSize * 2.5f, grid_start + kElementSpacing * 0.5f});
            auto visual = velk::trait::visual::create_cube();
            visual.set_mesh(cube_mesh);
            cube.add_trait(velk::ui::trait::layout::create_fixed_size(size_px, size_px, size_px));
            cube.add_trait(trs);
            cube.add_trait(visual);
            scene.add(root, cube);
            extra_cubes.push_back(cube);
            ++next_extra;
        }

        app.update();
        app.present();
        ++frame;

        if (auto bvh = scene_ptr ? scene_ptr->get_default_bvh() : nullptr) {
            const velk::ProbeVolume probes = bvh->get_probe_volume();
            if (!probe_volume_seen || probes.reset_frame != last_probe_reset) {
                VELK_LOG(I, "scale: frame %ld, probe volume y %.0f to %.0f, reset frame %u", frame,
                         static_cast<double>(probes.min.y), static_cast<double>(probes.max.y), probes.reset_frame);
                probe_volume_seen = true;
                last_probe_reset = probes.reset_frame;
            }
        }

        if (backend && frame > kWarmupFrames) {
            record_gpu_timings(gpu_passes, backend->last_gpu_timings());
        }

        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - report_start).count();
        if (elapsed >= 1.0) {
            const long frames = frame - report_frame;
            VELK_LOG(I, "scale: frame %ld, %.2f ms/frame", frame, elapsed * 1000.0 / frames);
            report_start = now;
            report_frame = frame;
        }

        if (frame_limit > 0 && frame >= frame_limit) {
            break;
        }
    }

    print_gpu_timings(gpu_passes);
    return 0;
}
