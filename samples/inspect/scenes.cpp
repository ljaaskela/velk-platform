#include "scenes.h"

#include <velk/api/velk.h>
#include <velk/interface/resource/intf_resource_protocol.h>
#include <velk/interface/resource/intf_resource_store.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <velk-render/api/material/standard_material.h>
#include <velk-render/api/shadow_technique.h>
#include <velk-render/blas.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-scene/api/mesh.h>
#include <velk-scene/api/trait/light.h>
#include <velk-scene/api/visual/cube.h>
#include <velk-scene/api/visual/instanced_mesh.h>
#include <velk-scene/api/visual/mesh.h>
#include <velk-scene/api/visual/sphere.h>
#include <velk-ui/api/trait/fixed_size.h>
#include <velk-ui/plugins/image/api/environment.h>

namespace inspect {

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

/// A value from -0.5 to 0.5 that depends only on @p i and @p salt.
float jitter(long i, float salt)
{
    const float x = std::sin(static_cast<float>(i) * 12.9898f + salt * 78.233f) * 43758.5453f;
    return x - std::floor(x) - 0.5f;
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

/// A tuft @p size across standing on y = 0: @p blades separate thin triangles, about 0.5
/// wide at the base and 3 to 5 tall, leaning a little each its own way.
velk::IMesh::Ptr build_tuft(velk::IRenderContext& ctx, float size, uint32_t blades, const velk::IMaterial::Ptr& material)
{
    velk::vector<Vertex> vertices;
    velk::vector<uint32_t> indices;
    for (uint32_t b = 0; b < blades; ++b) {
        const velk::vec3 base{jitter(b, 0.31f) * size, 0.f, jitter(b, 0.57f) * size};
        const float height = 4.f + jitter(b, 0.83f) * 2.f;
        const float turn = (jitter(b, 1.19f) + 0.5f) * 2.f * kPi;
        const velk::vec3 side{std::cos(turn) * 0.25f, 0.f, std::sin(turn) * 0.25f};
        const velk::vec3 lean{-std::sin(turn) * height * 0.25f, -height, std::cos(turn) * height * 0.25f};
        const velk::vec3 n = velk::vec3::normalize(velk::vec3{-std::sin(turn), -0.5f, std::cos(turn)});
        const auto first = static_cast<uint32_t>(vertices.size());
        for (const velk::vec3& p : {base - side, base + side, base + lean}) {
            vertices.push_back({{p.x, p.y, p.z}, {n.x, n.y, n.z}, {0.f, 0.f}, {side.x, side.y, side.z, 1.f}});
        }
        for (uint32_t i : {first, first + 1u, first + 2u}) {
            indices.push_back(i);
        }
    }
    return build(ctx, vertices, indices, material);
}

/// Adds @p element to the scene under its root, as one of the scene's.
void place(SceneBuilder& b, velk::Element& element)
{
    b.scene.add(b.root, element);
    b.out.elements.push_back(element);
}

/// The inside of a room, seen from inside: a face for each of floor, ceiling and back wall in
/// @p white, the left wall in @p left and the right one in @p right; open at z = 0.
void add_room(SceneBuilder& b, const velk::vec3& size, const velk::IMaterial::Ptr& white, const velk::IMaterial::Ptr& left,
              const velk::IMaterial::Ptr& right);

/// Adds @p mesh at @p at, returning its transform.
velk::Trs add_mesh(SceneBuilder& b, const velk::IMesh::Ptr& mesh, const velk::vec3& at)
{
    auto element = velk::create_element();
    auto trs = velk::trait::transform::create_trs();
    trs.set_translate(at);
    auto visual = velk::trait::visual::create_mesh();
    visual.set_mesh(velk::Mesh(mesh));
    element.add_trait(trs);
    element.add_trait(visual);
    place(b, element);
    return trs;
}

/// Adds an instanced visual of @p mesh at @p transforms.
void add_instances(SceneBuilder& b, const velk::IMesh::Ptr& mesh, const velk::vector<velk::InstanceTransform>& transforms)
{
    auto visual = velk::trait::visual::create_instanced_mesh();
    visual.set_mesh(velk::Mesh(mesh));
    visual.add_instances({transforms.data(), transforms.size()});
    auto element = velk::create_element();
    element.add_trait(visual);
    place(b, element);
}

/// A box from @p min, @p size across, drawn as a shape (the cube visual), or with @p as_mesh as
/// the same unit cube mesh scaled to that size.
void add_shape_box(SceneBuilder& b, const velk::vec3& min, const velk::vec3& size, const velk::color& c, bool as_mesh = false)
{
    auto element = velk::create_element();
    auto trs = velk::trait::transform::create_trs();
    trs.set_translate(min);
    element.add_trait(trs);
    velk::Mesh mesh(b.ctx.build_cube());
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
    place(b, element);
}

/// A sphere at @p center drawn as a shape (the sphere visual), or with @p as_mesh as the same
/// sphere mesh scaled to that size.
void add_shape_sphere(SceneBuilder& b, const velk::vec3& center, float radius, const velk::color& c, bool as_mesh = false)
{
    auto element = velk::create_element();
    auto trs = velk::trait::transform::create_trs();
    trs.set_translate({center.x - radius, center.y - radius, center.z - radius});
    element.add_trait(trs);
    velk::Mesh mesh(b.ctx.build_sphere(32));
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
    place(b, element);
}

/// A sun from @p rotation (degrees), with the shadow technique the reference path traces.
void add_sun(SceneBuilder& b, const velk::vec3& rotation, float intensity)
{
    auto light = velk::trait::render::create_directional_light(velk::color{1.f, 0.96f, 0.88f, 1.f}, intensity);
    light.add_technique(velk::technique::create_rt_shadow());
    auto sun = velk::create_element();
    auto trs = velk::trait::transform::create_trs();
    trs.set_rotation(rotation);
    sun.add_trait(trs);
    sun.add_trait(light);
    place(b, sun);
}

/// A point light at @p position, with the shadow technique the reference path traces.
void add_point(SceneBuilder& b, const velk::vec3& position, float intensity, float range)
{
    auto light = velk::trait::render::create_point_light(velk::color{1.f, 0.96f, 0.88f, 1.f}, intensity, range);
    light.add_technique(velk::technique::create_rt_shadow());
    auto element = velk::create_element();
    auto trs = velk::trait::transform::create_trs();
    trs.set_translate(position);
    element.add_trait(trs);
    element.add_trait(light);
    place(b, element);
}

/// The bytes of a Radiance HDR file of one colour, for an even sky.
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

/// An even sky of one colour around the scene, so that no test depends on what none means.
void set_sky(SceneBuilder& b, const char* name, float r, float g, float bl)
{
    const velk::vector<uint8_t> sky = build_flat_hdr(r, g, bl);
    auto protocol = velk::instance().resource_store().find_protocol("memory");
    auto* memory = interface_cast<velk::IMemoryProtocol>(protocol.get());
    if (!memory) {
        return;
    }
    char file[64];
    std::snprintf(file, sizeof(file), "inspect_sky_%s.hdr", name);
    memory->add_file(velk::string_view(file, std::strlen(file)), sky.data(), sky.size());
    char url[96];
    std::snprintf(url, sizeof(url), "env:memory://%s", file);
    auto env = velk::ui::load_environment(velk::string_view(url, std::strlen(url)));
    auto* surface = interface_cast<velk::ISurface>(env.get());
    if (!surface || surface->get_dimensions().x == 0) {
        VELK_LOG(E, "inspect: the sky '%s' did not load", url);
    }
    b.camera.set_environment(env);
}

velk::IMaterial::Ptr standard(float r, float g, float b, float roughness = 0.9f)
{
    return velk::material::create_standard(velk::color{r, g, b, 1.f}, 0.f, roughness);
}

void add_room(SceneBuilder& b, const velk::vec3& size, const velk::IMaterial::Ptr& white, const velk::IMaterial::Ptr& left,
              const velk::IMaterial::Ptr& right)
{
    velk::IRenderContext& ctx = *static_cast<velk::IRenderContext::Ptr>(b.ctx);
    const float x0 = -size.x * 0.5f;
    const float x1 = size.x * 0.5f;
    const float top = -size.y;
    const float back = -size.z;
    constexpr uint32_t kCells = 16;
    velk::vector<Vertex> vertices;
    velk::vector<uint32_t> indices;
    add_grid(vertices, indices, {x0, 0.f, back}, {size.x, 0.f, 0.f}, {0.f, 0.f, size.z}, {0.f, -1.f, 0.f}, kCells);
    add_grid(vertices, indices, {x0, top, back}, {size.x, 0.f, 0.f}, {0.f, 0.f, size.z}, {0.f, 1.f, 0.f}, kCells);
    add_grid(vertices, indices, {x0, top, back}, {size.x, 0.f, 0.f}, {0.f, size.y, 0.f}, {0.f, 0.f, 1.f}, kCells);
    add_mesh(b, build(ctx, vertices, indices, white), {});
    velk::vector<Vertex> lv;
    velk::vector<uint32_t> li;
    add_grid(lv, li, {x0, top, back}, {0.f, 0.f, size.z}, {0.f, size.y, 0.f}, {1.f, 0.f, 0.f}, kCells);
    add_mesh(b, build(ctx, lv, li, left), {});
    velk::vector<Vertex> rv;
    velk::vector<uint32_t> ri;
    add_grid(rv, ri, {x1, top, back}, {0.f, 0.f, size.z}, {0.f, size.y, 0.f}, {-1.f, 0.f, 0.f}, kCells);
    add_mesh(b, build(ctx, rv, ri, right), {});
}

velk::IRenderContext& raw(SceneBuilder& b)
{
    return *static_cast<velk::IRenderContext::Ptr>(b.ctx);
}

// The scenes, simplest first.

/// One box on a plane under a sun: one hard shadow edge.
void build_cube(SceneBuilder& b)
{
    set_sky(b, "cube", 0.2f, 0.25f, 0.33f);
    add_sun(b, {50.f, 30.f, 0.f}, 0.8f);
    add_mesh(b, build_ground(raw(b), 1200.f, 64, standard(0.5f, 0.5f, 0.5f)), {});
    add_mesh(b, build_box(raw(b), {80.f, 80.f, 80.f}, 8, standard(0.75f, 0.75f, 0.75f)), {});
}

/// Boxes of several sizes, some close together, under a sun and a point light: soft shadows,
/// shadows of two lights, contact shadows.
void build_cubes(SceneBuilder& b)
{
    set_sky(b, "cubes", 0.1f, 0.12f, 0.16f);
    add_sun(b, {50.f, 30.f, 0.f}, 0.5f);
    add_point(b, {-150.f, -150.f, 60.f}, 1.2f, 800.f);
    add_mesh(b, build_ground(raw(b), 1200.f, 64, standard(0.5f, 0.5f, 0.5f)), {});
    const auto box = [&](const velk::vec3& size, const velk::vec3& at) {
        add_mesh(b, build_box(raw(b), size, 8, standard(0.75f, 0.75f, 0.75f)), at);
    };
    box({60.f, 100.f, 60.f}, {0.f, 0.f, 0.f});
    box({40.f, 40.f, 40.f}, {-120.f, 0.f, 40.f});
    box({70.f, 30.f, 70.f}, {110.f, 0.f, -30.f});
    box({30.f, 60.f, 30.f}, {40.f, 0.f, 120.f});
    box({90.f, 20.f, 40.f}, {-60.f, 0.f, -120.f});
    box({12.f, 12.f, 12.f}, {48.f, 0.f, 0.f});
}

/// The shapes layout, its objects as shapes or with @p as_mesh as meshes.
void build_shapes_as(SceneBuilder& b, bool as_mesh)
{
    set_sky(b, "shapes", 0.1f, 0.12f, 0.16f);
    add_sun(b, {50.f, 30.f, 0.f}, 0.5f);
    add_point(b, {-150.f, -150.f, 60.f}, 1.2f, 800.f);
    const velk::color grey{0.5f, 0.5f, 0.5f, 1.f};
    const velk::color white{0.75f, 0.75f, 0.75f, 1.f};
    add_shape_box(b, {-600.f, 0.f, -600.f}, {1200.f, 4.f, 1200.f}, grey, as_mesh);
    const auto box = [&](const velk::vec3& size, const velk::vec3& at) {
        add_shape_box(b, {at.x - size.x * 0.5f, -size.y, at.z - size.z * 0.5f}, size, white, as_mesh);
    };
    box({60.f, 100.f, 60.f}, {0.f, 0.f, 0.f});
    box({40.f, 40.f, 40.f}, {-120.f, 0.f, 40.f});
    box({70.f, 30.f, 70.f}, {110.f, 0.f, -30.f});
    add_shape_sphere(b, {40.f, -30.f, 120.f}, 30.f, white, as_mesh);
    add_shape_sphere(b, {-60.f, -20.f, -120.f}, 20.f, white, as_mesh);
}

/// The same layout as cubes, every object a shape (cube and sphere visuals) instead of a mesh,
/// the ground too. Its reference is the same layout of meshes, which should look the same.
void build_shapes(SceneBuilder& b)
{
    build_shapes_as(b, false);
}

void build_shapes_meshes(SceneBuilder& b)
{
    build_shapes_as(b, true);
}

/// A closed room with a red and a green wall and two boxes, lit by a point light under its
/// ceiling, and no sky: bounce light and colour bleeding.
void build_cornell(SceneBuilder& b)
{
    set_sky(b, "cornell", 0.f, 0.f, 0.f);
    add_point(b, {0.f, -270.f, -150.f}, 1.f, 800.f);
    add_room(b, {300.f, 300.f, 300.f}, standard(0.73f, 0.73f, 0.73f), standard(0.63f, 0.06f, 0.05f),
             standard(0.12f, 0.45f, 0.15f));
    add_mesh(b, build_box(raw(b), {80.f, 180.f, 80.f}, 8, standard(0.73f, 0.73f, 0.73f)), {-55.f, 0.f, -190.f});
    add_mesh(b, build_box(raw(b), {80.f, 80.f, 80.f}, 8, standard(0.73f, 0.73f, 0.73f)), {60.f, 0.f, -100.f});
}

/// A fence of thin boards and a canopy of small leaves under a sun: detail near the size of
/// the light's spacing.
void build_thin(SceneBuilder& b)
{
    set_sky(b, "thin", 0.2f, 0.25f, 0.33f);
    add_sun(b, {50.f, 30.f, 0.f}, 0.8f);
    add_mesh(b, build_ground(raw(b), 1200.f, 64, standard(0.5f, 0.5f, 0.5f)), {});
    const velk::IMesh::Ptr board = build_box(raw(b), {3.f, 90.f, 3.f}, 2, standard(0.6f, 0.45f, 0.3f));
    for (int i = 0; i < 25; ++i) {
        add_mesh(b, board, {-150.f + static_cast<float>(i) * 12.f, 0.f, 40.f});
    }
    const velk::IMesh::Ptr leaf = build_box(raw(b), {8.f, 1.f, 8.f}, 1, standard(0.2f, 0.45f, 0.15f));
    for (int z = 0; z < 12; ++z) {
        for (int x = 0; x < 12; ++x) {
            add_mesh(b, leaf, {-96.f + static_cast<float>(x) * 16.f, -140.f, -200.f + static_cast<float>(z) * 16.f});
        }
    }
}

/// A box moving across the plane over the first frames, then standing still: how light
/// follows what moves, and settles once it stops.
void build_moving(SceneBuilder& b)
{
    set_sky(b, "moving", 0.2f, 0.25f, 0.33f);
    add_sun(b, {50.f, 30.f, 0.f}, 0.8f);
    add_mesh(b, build_ground(raw(b), 1200.f, 64, standard(0.5f, 0.5f, 0.5f)), {});
    add_mesh(b, build_box(raw(b), {50.f, 50.f, 50.f}, 8, standard(0.75f, 0.75f, 0.75f)), {-120.f, 0.f, -20.f});
    b.out.moves = true;
    b.out.from = {-200.f, 0.f, 60.f};
    b.out.to = {150.f, 0.f, 60.f};
    b.out.frames = 120;
    b.out.mover = add_mesh(b, build_box(raw(b), {60.f, 60.f, 60.f}, 8, standard(0.75f, 0.75f, 0.75f)), b.out.from);
}

/// A small forest of tree sized solids and a field of pebbles, instanced: the instanced cases.
void build_forest(SceneBuilder& b)
{
    set_sky(b, "forest", 0.2f, 0.25f, 0.33f);
    add_sun(b, {50.f, 30.f, 0.f}, 0.8f);
    add_mesh(b, build_ground(raw(b), 4000.f, 128, standard(0.45f, 0.42f, 0.38f)), {});
    {
        velk::vector<velk::InstanceTransform> transforms;
        for (long i = 0; i < 30 * 30; ++i) {
            const float x = -650.f + (static_cast<float>(i % 30) + 0.8f * jitter(i, 3.1f)) * 45.f;
            const float z = -1400.f + (static_cast<float>(i / 30) + 0.8f * jitter(i, 5.9f)) * 45.f;
            const float s = 1.f + jitter(i, 8.3f) * 0.5f;
            const float turn = (jitter(i, 9.7f) + 0.5f) * 2.f * kPi;
            const float c = std::cos(turn) * s;
            const float n = std::sin(turn) * s;
            transforms.push_back(velk::InstanceTransform{{{c, 0.f, n, x}, {0.f, s * 2.5f, 0.f, 0.f}, {-n, 0.f, c, z}}});
        }
        add_instances(b, build_rock(raw(b), 24.f, 48, 24, standard(0.12f, 0.3f, 0.1f, 0.8f)), transforms);
    }
    {
        velk::vector<velk::InstanceTransform> transforms;
        for (long i = 0; i < 60 * 60; ++i) {
            const float x = 40.f + (static_cast<float>(i % 60) + jitter(i, 1.3f)) * 9.f;
            const float z = -560.f + (static_cast<float>(i / 60) + jitter(i, 2.9f)) * 9.f;
            const float s = 0.6f + jitter(i, 4.1f) * 0.6f;
            const float turn = (jitter(i, 7.7f) + 0.5f) * 2.f * kPi;
            const float c = std::cos(turn) * s;
            const float n = std::sin(turn) * s;
            transforms.push_back(velk::InstanceTransform{{{c, 0.f, n, x}, {0.f, s, 0.f, 0.f}, {-n, 0.f, c, z}}});
        }
        add_instances(b, build_rock(raw(b), 5.f, 16, 8, standard(0.35f, 0.42f, 0.25f, 0.8f)), transforms);
    }
}

/// The large field: rocks at rising distances, pebbles, tree sized solids and tufts of blades out
/// to where they are a pixel or two, plain rocks and a wall, under a strong sun.
void build_field(SceneBuilder& b)
{
    set_sky(b, "field", 0.2f, 0.25f, 0.33f);
    add_sun(b, {35.f, 25.f, 0.f}, 2.5f);
    auto ground_material = standard(0.45f, 0.42f, 0.38f);
    auto rock_material = standard(0.55f, 0.5f, 0.44f, 0.8f);
    auto pebble_material = standard(0.35f, 0.42f, 0.25f, 0.8f);
    auto wall_material = standard(0.6f, 0.58f, 0.55f, 0.7f);
    auto tree_material = standard(0.12f, 0.3f, 0.1f, 0.8f);
    auto tuft_material = velk::material::create_standard(velk::color{0.5f, 0.6f, 0.15f, 1.f}, 0.f, 0.8f);
    // Blades are seen from both sides.
    tuft_material.options().set_double_sided(true);

    add_mesh(b, build_ground(raw(b), 32000.f, 512, ground_material), {});
    const velk::IMesh::Ptr rock = build_rock(raw(b), 60.f, 160, 80, rock_material);
    for (float z : {150.f, -100.f, -600.f, -1600.f, -3500.f}) {
        add_mesh(b, rock, {-60.f, 0.f, z});
    }
    {
        constexpr long kColumns = 200;
        velk::vector<velk::InstanceTransform> transforms;
        for (long i = 0; i < kColumns * kColumns; ++i) {
            const float x = 40.f + (static_cast<float>(i % kColumns) + jitter(i, 1.3f)) * 9.f;
            const float z = -1500.f + (static_cast<float>(i / kColumns) + jitter(i, 2.9f)) * 9.f;
            const float s = 0.6f + jitter(i, 4.1f) * 0.6f;
            const float turn = (jitter(i, 7.7f) + 0.5f) * 2.f * kPi;
            const float c = std::cos(turn) * s;
            const float n = std::sin(turn) * s;
            transforms.push_back(velk::InstanceTransform{{{c, 0.f, n, x}, {0.f, s, 0.f, 0.f}, {-n, 0.f, c, z}}});
        }
        add_instances(b, build_rock(raw(b), 5.f, 16, 8, pebble_material), transforms);
    }
    {
        constexpr long kColumns = 80;
        constexpr long kRows = 333;
        velk::vector<velk::InstanceTransform> transforms;
        for (long i = 0; i < kColumns * kRows; ++i) {
            const float x = -4000.f + (static_cast<float>(i % kColumns) + 0.8f * jitter(i, 3.1f)) * 45.f;
            const float z = -15000.f + (static_cast<float>(i / kColumns) + 0.8f * jitter(i, 5.9f)) * 45.f;
            const float s = 1.f + jitter(i, 8.3f) * 0.5f;
            const float turn = (jitter(i, 9.7f) + 0.5f) * 2.f * kPi;
            const float c = std::cos(turn) * s;
            const float n = std::sin(turn) * s;
            transforms.push_back(velk::InstanceTransform{{{c, 0.f, n, x}, {0.f, s * 2.5f, 0.f, 0.f}, {-n, 0.f, c, z}}});
        }
        add_instances(b, build_rock(raw(b), 24.f, 48, 24, tree_material), transforms);
    }
    {
        constexpr long kColumns = 80;
        constexpr long kRows = 409;
        velk::vector<velk::InstanceTransform> transforms;
        for (long i = 0; i < kColumns * kRows; ++i) {
            const float x = 2200.f + (static_cast<float>(i % kColumns) + 0.8f * jitter(i, 2.3f)) * 22.f;
            const float z = -9000.f + (static_cast<float>(i / kColumns) + 0.8f * jitter(i, 6.1f)) * 22.f;
            const float turn = (jitter(i, 4.4f) + 0.5f) * 2.f * kPi;
            const float c = std::cos(turn);
            const float n = std::sin(turn);
            transforms.push_back(velk::InstanceTransform{{{c, 0.f, n, x}, {0.f, 1.f, 0.f, 0.f}, {-n, 0.f, c, z}}});
        }
        add_instances(b, build_tuft(raw(b), 20.f, 100, tuft_material), transforms);
    }
    {
        const velk::IMesh::Ptr small_rock = build_rock(raw(b), 8.f, 16, 8, rock_material);
        for (long i = 0; i < 40 * 50; ++i) {
            const float x = 400.f + (static_cast<float>(i % 40) + 0.8f * jitter(i, 7.1f)) * 40.f;
            const float z = -1800.f - (static_cast<float>(i / 40) + 0.8f * jitter(i, 8.9f)) * 110.f;
            add_mesh(b, small_rock, {x, 0.f, z});
        }
    }
    add_mesh(b, build_box(raw(b), {900.f, 260.f, 30.f}, 24, wall_material), {500.f, 0.f, -700.f});
}

} // namespace

const velk::vector<SceneSpec>& scenes()
{
    static const velk::vector<SceneSpec> list = {
        {"cube", "one box on a plane, a sun: one hard shadow edge",
         {{"front", {{0.f, -110.f, 300.f}, 0.f, -15.f}}, {"above", {{180.f, -380.f, 220.f}, 35.f, -50.f}},
          {"close", {{-78.2f, -31.3f, 49.8f}, -10.4f, -35.4f}}},
         build_cube},
        {"cubes", "boxes of several sizes, a sun and a point light: soft, double and contact shadows",
         {{"front", {{0.f, -130.f, 360.f}, 0.f, -18.f}, {{30.f, -120.f, 300.f}, 8.f, -19.f}, 10},
          {"low", {{260.f, -60.f, 260.f}, 45.f, -8.f}}},
         build_cubes},
        {"shapes", "the cubes layout as cube and sphere shapes, the ground too",
         {{"front", {{0.f, -130.f, 360.f}, 0.f, -18.f}}, {"low", {{260.f, -60.f, 260.f}, 45.f, -8.f}}},
         build_shapes, build_shapes_meshes},
        {"cornell", "a closed room, a red and a green wall, a point light: bounce and colour bleeding",
         {{"front", {{0.f, -150.f, 420.f}, 0.f, 0.f}}},
         build_cornell},
        {"thin", "a fence of thin boards and a canopy of small leaves: detail near the light's spacing",
         {{"front", {{0.f, -100.f, 320.f}, 0.f, -15.f}}, {"above", {{150.f, -320.f, 250.f}, 30.f, -45.f}}},
         build_thin},
        {"moving", "a box moving across, then standing still: light follows and settles",
         {{"front", {{0.f, -120.f, 320.f}, 0.f, -15.f}}},
         build_moving},
        {"forest", "instanced tree sized solids and pebbles",
         {{"near", {{0.f, -60.f, 250.f}, 0.f, -8.f}}, {"far", {{0.f, -260.f, 500.f}, 0.f, -20.f}}},
         build_forest},
        {"field", "the large field: instances out to a pixel or two, tufts of blades, a wall",
         {{"start", {{0.f, -60.f, 300.f}, 0.f, -8.f}}, {"far", {{0.f, -400.f, 300.f}, 0.f, -15.f}}},
         build_field},
    };
    return list;
}

const SceneSpec* find_scene(const char* name)
{
    for (const SceneSpec& spec : scenes()) {
        if (std::strcmp(spec.name, name) == 0) {
            return &spec;
        }
    }
    return nullptr;
}

BuiltScene build_scene(const SceneSpec& spec, velk::Scene& scene, velk::Element& root, velk::RenderContext& ctx,
                       velk::Camera& camera, bool reference)
{
    BuiltScene out;
    SceneBuilder b{scene, root, ctx, camera, out};
    (reference && spec.build_reference ? spec.build_reference : spec.build)(b);
    return out;
}

void remove_scene(BuiltScene& built, velk::Scene& scene)
{
    for (auto& element : built.elements) {
        scene.remove(element);
    }
    built = BuiltScene{};
}

Place motion_place(const ViewSpec& view, int k)
{
    if (view.move_frames <= 0 || k <= 0) {
        return view.place;
    }
    const float t = std::min(static_cast<float>(k) / static_cast<float>(view.move_frames), 1.f);
    Place p;
    p.at = view.place.at + (view.move_to.at - view.place.at) * t;
    p.yaw = view.place.yaw + (view.move_to.yaw - view.place.yaw) * t;
    p.pitch = view.place.pitch + (view.move_to.pitch - view.place.pitch) * t;
    return p;
}

velk::vec3 forward_of(float yaw_degrees, float pitch_degrees)
{
    const float yaw = yaw_degrees * kPi / 180.f;
    const float pitch = pitch_degrees * kPi / 180.f;
    return {-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

} // namespace inspect
