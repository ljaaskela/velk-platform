#include "nature_trees.h"

#include <velk-render/api/material/material.h>
#include <velk-render/blas.h>
#include <velk-scene/api/mesh.h>

#include <algorithm>
#include <cmath>

namespace velk_samples {

velk::IMaterial::Ptr tree_material(const velk::color& c, float roughness, bool double_sided, bool wind, float transmission)
{
    auto material = velk::instance().create<ITreeMaterial>(TreeMaterial::static_class_id());
    material->albedo().set_value(c);
    material->roughness().set_value(roughness);
    material->wind().set_value(wind);
    material->transmission().set_value(transmission);
    velk::Material options(interface_pointer_cast<velk::IObject>(material));
    options.options().set_double_sided(double_sided);
    return interface_pointer_cast<velk::IMaterial>(material);
}

velk::IMesh::Ptr TreeGrower::build(velk::IRenderContext& ctx, const velk::IMaterial::Ptr& bark, const velk::IMaterial::Ptr& foliage)
{
    grow(0, {0.f, 0.f, 0.f}, {0.f, -1.f, 0.f}, shape_.trunk_length, shape_.trunk_radius);
    shade_leaves();
    velk::IMeshPrimitive::Ptr leaves = make_primitive(ctx, leaves_, leaf_indices_, foliage);
    velk::IMeshPrimitive::Ptr primitives[] = {make_primitive(ctx, wood_, wood_indices_, bark), leaves};
    return ctx.get_mesh_builder().build({primitives, 2});
}

float TreeGrower::random()
{
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    return static_cast<float>(state_ % 100000u) / 100000.f;
}

velk::vec3 TreeGrower::random_direction()
{
    const float z = random() * 2.f - 1.f;
    const float theta = random() * 6.2831853f;
    const float r = std::sqrt(std::max(1.f - z * z, 0.f));
    return {r * std::cos(theta), z, r * std::sin(theta)};
}

namespace {

velk::vec3 perpendicular(const velk::vec3& d)
{
    const velk::vec3 axis = std::abs(d.y) < 0.9f ? velk::vec3{0.f, 1.f, 0.f} : velk::vec3{1.f, 0.f, 0.f};
    return velk::vec3::normalize(velk::vec3::cross(d, axis));
}

} // namespace

/// One branch and those growing from it.
void TreeGrower::grow(int level, const velk::vec3& base, const velk::vec3& direction, float length, float radius)
{
    const int segments = level == 0 ? 6 : 3;
    const uint32_t sides = level == 0 ? 7u : level == 1 ? 5u : 3u;
    const velk::vec3 up{0.f, -1.f, 0.f};
    velk::vector<velk::vec3> points;
    velk::vector<velk::vec3> directions;
    velk::vec3 p = base;
    velk::vec3 d = direction;
    for (int s = 0; s <= segments; ++s) {
        points.push_back(p);
        directions.push_back(d);
        d = velk::vec3::normalize(d + up * (shape_.rise / static_cast<float>(segments)) + random_direction() * 0.12f);
        p = p + d * (length / static_cast<float>(segments));
    }

    // Rings along the branch, narrowing towards its end.
    const auto first_vertex = static_cast<uint32_t>(wood_.size());
    velk::vec3 u = perpendicular(directions[0]);
    for (int s = 0; s <= segments; ++s) {
        const velk::vec3& dir = directions[static_cast<size_t>(s)];
        u = velk::vec3::normalize(u - dir * velk::vec3::dot(u, dir));
        const velk::vec3 v = velk::vec3::cross(dir, u);
        const float along = static_cast<float>(s) / static_cast<float>(segments);
        const float ring_radius = radius * (1.f - 0.75f * along);
        for (uint32_t side = 0; side < sides; ++side) {
            const float theta = static_cast<float>(side) / static_cast<float>(sides) * 6.2831853f;
            const velk::vec3 n = u * std::cos(theta) + v * std::sin(theta);
            const velk::vec3 at = points[static_cast<size_t>(s)] + n * ring_radius;
            wood_.push_back({{at.x, at.y, at.z}, {n.x, n.y, n.z}, {0.f, along}, {dir.x, dir.y, dir.z, 1.f}});
        }
    }
    for (int s = 0; s < segments; ++s) {
        for (uint32_t side = 0; side < sides; ++side) {
            const uint32_t next = (side + 1) % sides;
            const uint32_t a = first_vertex + static_cast<uint32_t>(s) * sides + side;
            const uint32_t b = first_vertex + static_cast<uint32_t>(s) * sides + next;
            for (uint32_t i : {a, b + sides, a + sides, a, b, b + sides}) {
                wood_indices_.push_back(i);
            }
        }
    }

    if (level == shape_.levels) {
        for (int leaf = 0; leaf < shape_.leaves; ++leaf) {
            const float t = 0.25f + 0.75f * random();
            const auto at = static_cast<size_t>(t * static_cast<float>(segments));
            add_leaf(points[at] + random_direction() * (length * 0.3f), directions[at]);
        }
        return;
    }

    for (int child = 0; child < shape_.children; ++child) {
        const float t = shape_.first_child + (1.f - shape_.first_child) * (static_cast<float>(child) + random() * 0.6f)
            / static_cast<float>(shape_.children);
        const float position = t * static_cast<float>(segments);
        const auto at = std::min(static_cast<size_t>(position), static_cast<size_t>(segments - 1));
        const float blend = position - static_cast<float>(at);
        const velk::vec3 from = points[at] + (points[at + 1] - points[at]) * blend;
        const velk::vec3 dir = directions[at];
        // Around the parent by the golden angle, so branches do not line up.
        const float around = static_cast<float>(child) * 2.39996f + random() * 0.5f;
        const velk::vec3 side_u = perpendicular(dir);
        const velk::vec3 side_v = velk::vec3::cross(dir, side_u);
        const velk::vec3 out = side_u * std::cos(around) + side_v * std::sin(around);
        const float angle = shape_.spread * (0.8f + 0.4f * random());
        const velk::vec3 child_direction = velk::vec3::normalize(dir * std::cos(angle) + out * std::sin(angle));
        const float scale = shape_.taper_with_height ? (1.05f - t) : (0.8f + 0.4f * random());
        grow(level + 1, from, child_direction, length * shape_.length_ratio * scale,
             radius * (1.f - 0.75f * t) * shape_.radius_ratio);
    }
}

void TreeGrower::add_leaf(const velk::vec3& at, const velk::vec3& direction)
{
    const velk::vec3 axis = velk::vec3::normalize(direction + random_direction());
    const velk::vec3 across = velk::vec3::normalize(velk::vec3::cross(axis, random_direction())) * (shape_.leaf_width * 0.5f);
    const velk::vec3 mid = at + axis * (shape_.leaf_length * 0.5f);
    const velk::vec3 tip = at + axis * shape_.leaf_length;
    const velk::vec3 corners[] = {at, mid + across, tip, mid - across};
    const velk::vec3 normal = velk::vec3::normalize(velk::vec3::cross(across, axis));
    const auto first = static_cast<uint32_t>(leaves_.size());
    for (const auto& c : corners) {
        leaves_.push_back({{c.x, c.y, c.z}, {normal.x, normal.y, normal.z}, {1.f, 0.f}, {axis.x, axis.y, axis.z, 1.f}});
    }
    for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
        leaf_indices_.push_back(first + i);
    }
}

/// Normals lean away from the crown's middle, so it shades as one soft shape.
void TreeGrower::shade_leaves()
{
    velk::vec3 center{0.f, 0.f, 0.f};
    for (const auto& leaf : leaves_) {
        center = center + velk::vec3{leaf.position[0], leaf.position[1], leaf.position[2]};
    }
    center = center * (1.f / static_cast<float>(std::max<size_t>(leaves_.size(), 1)));
    for (auto& leaf : leaves_) {
        const velk::vec3 out = velk::vec3::normalize(velk::vec3{leaf.position[0], leaf.position[1], leaf.position[2]} - center);
        const velk::vec3 own{leaf.normal[0], leaf.normal[1], leaf.normal[2]};
        const velk::vec3 n = velk::vec3::normalize(own * 0.35f + out * 0.65f);
        leaf.normal[0] = n.x;
        leaf.normal[1] = n.y;
        leaf.normal[2] = n.z;
    }
}

velk::IMeshPrimitive::Ptr TreeGrower::make_primitive(velk::IRenderContext& ctx, const velk::vector<Vertex>& vertices,
                                                     const velk::vector<uint32_t>& indices, const velk::IMaterial::Ptr& material)
{
    static constexpr velk::VertexAttribute kAttributes[] = {
        {velk::VertexAttributeSemantic::Position, velk::VertexAttributeFormat::Float3, 0},
        {velk::VertexAttributeSemantic::Normal, velk::VertexAttributeFormat::Float3, 12},
        {velk::VertexAttributeSemantic::TexCoord0, velk::VertexAttributeFormat::Float2, 24},
        {velk::VertexAttributeSemantic::Tangent, velk::VertexAttributeFormat::Float4, 32},
    };
    velk::vec3 lo{vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]};
    velk::vec3 hi = lo;
    for (const auto& vertex : vertices) {
        lo.x = std::min(lo.x, vertex.position[0]);
        lo.y = std::min(lo.y, vertex.position[1]);
        lo.z = std::min(lo.z, vertex.position[2]);
        hi.x = std::max(hi.x, vertex.position[0]);
        hi.y = std::max(hi.y, vertex.position[1]);
        hi.z = std::max(hi.z, vertex.position[2]);
    }
    velk::aabb bounds{};
    bounds.position = lo;
    bounds.extent = {hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
    const auto vertex_count = static_cast<uint32_t>(vertices.size());
    const auto index_count = static_cast<uint32_t>(indices.size());
    auto primitive = ctx.get_mesh_builder().build_primitive({kAttributes, 4}, sizeof(Vertex), vertices.data(), vertex_count,
                                                            indices.data(), index_count, velk::MeshTopology::TriangleList, bounds);
    primitive->set_rt_blas(velk::build_mesh_blas(reinterpret_cast<const float*>(vertices.data()), sizeof(Vertex), vertex_count,
                                                 indices.data(), index_count / 3));
    velk::MeshPrimitive(primitive).set_material(material);
    return primitive;
}

NatureTrees build_nature_trees(velk::IRenderContext& ctx, bool wind, float leaf_density)
{
    const auto foliage = [&](const velk::color& c) { return tree_material(c, 0.75f, true, wind, kLeafTransmission); };
    const auto bark = [&](const velk::color& c) { return tree_material(c, 0.9f, false, wind, 0.f); };
    //                   seed trunk radius lvl kids first spread ratio radius rise  taper  leaves leaf sizes
    TreeShape oak_shape{ 11u, 22.f, 2.4f,  3,  5,   0.5f, 0.85f, 0.62f, 0.62f, 0.35f, false, 26,  3.0f, 2.2f};
    TreeShape birch_shape{23u, 34.f, 1.3f, 3,  4,   0.4f, 0.55f, 0.58f, 0.60f, -0.15f, false, 22, 2.2f, 1.6f};
    TreeShape pine_shape{37u, 78.f, 1.9f,  2,  9,   0.18f, 1.25f, 0.34f, 0.5f, 0.05f, true,  20,  3.4f, 0.8f};
    // Fewer, larger leaves keep a crown's area.
    for (TreeShape* shape : {&oak_shape, &birch_shape, &pine_shape}) {
        shape->leaves = std::max(1, static_cast<int>(static_cast<float>(shape->leaves) * leaf_density + 0.5f));
        shape->leaf_length /= std::sqrt(leaf_density);
        shape->leaf_width /= std::sqrt(leaf_density);
    }
    NatureTrees trees;
    trees.oak = TreeGrower(oak_shape).build(ctx, bark(velk::color{0.24f, 0.17f, 0.11f, 1.f}), foliage(velk::color{0.11f, 0.26f, 0.05f, 1.f}));
    trees.birch = TreeGrower(birch_shape).build(ctx, bark(velk::color{0.58f, 0.55f, 0.50f, 1.f}), foliage(velk::color{0.26f, 0.38f, 0.08f, 1.f}));
    trees.pine = TreeGrower(pine_shape).build(ctx, bark(velk::color{0.20f, 0.14f, 0.10f, 1.f}), foliage(velk::color{0.04f, 0.15f, 0.07f, 1.f}));
    return trees;
}

} // namespace velk_samples
