#include "aggregate.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>

#include <velk-render/interface/material/intf_material.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace velk::impl {

namespace {

uint32_t pack_color(float r, float g, float b)
{
    const auto unit = [](float v) { return static_cast<uint32_t>(std::min(std::max(v, 0.f), 1.f) * 255.f + 0.5f); };
    return unit(r) | (unit(g) << 8) | (unit(b) << 16) | (255u << 24);
}

} // namespace

uint32_t mesh_color(const IMesh& mesh)
{
    // Its materials' colour, by how much of its surface each covers.
    float total = 0.f;
    float sum[3] = {0.f, 0.f, 0.f};
    for (const auto& primitive : mesh.get_primitives()) {
        color tint = color::white();
        if (auto state = read_state<IMeshPrimitive>(primitive.get()); state && state->material) {
            if (auto material = state->material.template get<IMaterial>()) {
                tint = material->get_average_albedo();
            }
        }
        const float area = primitive->get_surface_area();
        total += area;
        sum[0] += tint.r * area;
        sum[1] += tint.g * area;
        sum[2] += tint.b * area;
    }
    return total > 0.f ? pack_color(sum[0] / total, sum[1] / total, sum[2] / total) : pack_color(1.f, 1.f, 1.f);
}

MeshQuad bounds_quad(const float lo[3], const float hi[3], uint32_t color)
{
    // Across the two longer sides, facing along the thinnest; u cross v points along it.
    MeshQuad out;
    int thin = 0;
    for (int k = 1; k < 3; ++k) {
        thin = hi[k] - lo[k] < hi[thin] - lo[thin] ? k : thin;
    }
    const int a = (thin + 1) % 3;
    const int b = (thin + 2) % 3;
    for (int k = 0; k < 3; ++k) {
        out.center[k] = 0.5f * (lo[k] + hi[k]);
    }
    out.u[a] = 0.5f * (hi[a] - lo[a]);
    out.v[b] = 0.5f * (hi[b] - lo[b]);
    out.color = color;
    return out;
}

namespace {

/// A node as quad_tree_build_src holds it: colour as it is stored, radius negative for none.
struct TreeQuad
{
    float c[3] = {0.f, 0.f, 0.f};
    float u[3] = {0.f, 0.f, 0.f};
    float v[3] = {0.f, 0.f, 0.f};
    uint32_t color = 0;
    float radius = -1.f;
};

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float length3(const float* a) { return std::sqrt(dot3(a, a)); }
void cross3(const float* a, const float* b, float* out)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/// The shader's merge, step for step, with the colour stored as RGBA8 after each.
TreeQuad merge_tree_quads(const TreeQuad& a, const TreeQuad& b)
{
    if (a.radius < 0.f) return b;
    if (b.radius < 0.f) return a;
    float na[3];
    float nb[3];
    cross3(a.u, a.v, na);
    cross3(b.u, b.v, nb);
    const float quarter_a = length3(na);
    const float quarter_b = length3(nb);
    const float quarter = quarter_a + quarter_b;
    const float wa = quarter > 0.f ? quarter_a / quarter : 0.5f;
    const TreeQuad& big = quarter_a >= quarter_b ? a : b;

    TreeQuad q;
    for (int k = 0; k < 3; ++k) {
        q.c[k] = b.c[k] + (a.c[k] - b.c[k]) * wa;
    }
    uint32_t color = 0;
    for (int k = 0; k < 4; ++k) {
        const float ca = static_cast<float>((a.color >> (k * 8)) & 0xFFu) / 255.f;
        const float cb = static_cast<float>((b.color >> (k * 8)) & 0xFFu) / 255.f;
        const float mixed = std::min(std::max(cb + (ca - cb) * wa, 0.f), 1.f);
        color |= static_cast<uint32_t>(std::round(mixed * 255.f)) << (k * 8);
    }
    q.color = color;
    float n[3] = {na[0] + nb[0], na[1] + nb[1], na[2] + nb[2]};
    if (dot3(n, n) <= 1e-12f * quarter * quarter) {
        cross3(big.u, big.v, n);
    }
    const float n_length = length3(n);
    if (n_length > 0.f) {
        for (float& x : n) {
            x /= n_length;
        }
    } else {
        n[0] = 0.f;
        n[1] = 1.f;
        n[2] = 0.f;
    }
    const float d = dot3(big.u, n);
    float along[3] = {big.u[0] - d * n[0], big.u[1] - d * n[1], big.u[2] - d * n[2]};
    if (dot3(along, along) <= 1e-12f * dot3(big.u, big.u)) {
        const float x_axis[3] = {1.f, 0.f, 0.f};
        const float y_axis[3] = {0.f, 1.f, 0.f};
        cross3(n, std::abs(n[0]) < 0.9f ? x_axis : y_axis, along);
    }
    const float along_length = length3(along);
    for (float& x : along) {
        x /= along_length;
    }
    const float lu = length3(big.u);
    const float lv = length3(big.v);
    const float ratio = lv > 0.f ? lu / lv : 1.f;
    float hu = std::sqrt(quarter * ratio);
    float hv = std::sqrt(quarter / std::max(ratio, 1e-6f));
    const float longest = std::max(std::max(length3(a.u), length3(a.v)), std::max(length3(b.u), length3(b.v)));
    if (std::max(hu, hv) < longest) {
        if (hu >= hv) {
            hu = longest;
            hv = quarter / longest;
        } else {
            hv = longest;
            hu = quarter / longest;
        }
    }
    float across[3];
    cross3(n, along, across);
    for (int k = 0; k < 3; ++k) {
        q.u[k] = along[k] * hu;
        q.v[k] = across[k] * hv;
    }
    float to_a[3];
    float to_b[3];
    for (int k = 0; k < 3; ++k) {
        to_a[k] = a.c[k] - q.c[k];
        to_b[k] = b.c[k] - q.c[k];
    }
    q.radius = std::max(std::max(length3(to_a) + a.radius, length3(to_b) + b.radius), length3(q.u) + length3(q.v));
    return q;
}

} // namespace

vector<MeshQuad> merge_pairs(const vector<MeshQuad>& quads)
{
    // Spheres only tell a quad from none here: what a merge makes does not depend on them.
    const auto tree_quad = [](const MeshQuad& m) {
        TreeQuad q;
        std::memcpy(q.c, m.center, sizeof(q.c));
        std::memcpy(q.u, m.u, sizeof(q.u));
        std::memcpy(q.v, m.v, sizeof(q.v));
        q.color = m.color;
        q.radius = m.color == 0u ? -1.f : 0.f;
        return q;
    };
    vector<MeshQuad> up;
    up.resize(quads.size() / 2);
    for (size_t i = 0; i < up.size(); ++i) {
        const TreeQuad q = merge_tree_quads(tree_quad(quads[2 * i]), tree_quad(quads[2 * i + 1]));
        if (q.radius < 0.f) {
            continue;  // Both none.
        }
        std::memcpy(up[i].center, q.c, sizeof(q.c));
        std::memcpy(up[i].u, q.u, sizeof(q.u));
        std::memcpy(up[i].v, q.v, sizeof(q.v));
        up[i].color = q.color;
    }
    return up;
}

bool mesh_quad(const IMesh& mesh, MeshQuad& out)
{
    out = MeshQuad{};
    const aabb bounds = mesh.get_bounds();
    const vec3 lo = bounds.min();
    const vec3 hi = bounds.max();
    const float min[3] = {lo.x, lo.y, lo.z};
    const float max[3] = {hi.x, hi.y, hi.z};
    if (!(max[0] >= min[0] && max[1] >= min[1] && max[2] >= min[2])) {
        return false;
    }
    out = bounds_quad(min, max, mesh_color(mesh));
    return true;
}

const string_view quad_tree_build_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 256) in;

struct BuildInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer BuildInstances { BuildInstance data[]; } build_instances;
layout(set = 1, binding = 16, std430) coherent buffer BuildWords { uint data[]; } build_words;
layout(set = 1, binding = 16, std430) readonly buffer BuildRows { vec4 data[]; } build_rows;

layout(push_constant) uniform PC {
    uint jobs_word;
} pc;

const uint kNone = 0xFFFFFFFFu;
const uint kNodeWords = 11u;
const uint kJobWords = 8u;
const uint kMeshQuadWords = 10u;

struct Quad {
    vec3 c;
    vec3 u;
    vec3 v;
    vec4 color;
    float radius;  // negative for none
};

vec3 load3(uint w)
{
    return vec3(uintBitsToFloat(build_words.data[w]), uintBitsToFloat(build_words.data[w + 1u]),
                uintBitsToFloat(build_words.data[w + 2u]));
}

void store3(uint w, vec3 x)
{
    build_words.data[w] = floatBitsToUint(x.x);
    build_words.data[w + 1u] = floatBitsToUint(x.y);
    build_words.data[w + 2u] = floatBitsToUint(x.z);
}

Quad load_node(uint w)
{
    Quad q;
    q.c = load3(w);
    q.u = load3(w + 3u);
    q.v = load3(w + 6u);
    q.color = unpackUnorm4x8(build_words.data[w + 9u]);
    q.radius = uintBitsToFloat(build_words.data[w + 10u]);
    return q;
}

void store_node(uint w, Quad q)
{
    store3(w, q.c);
    store3(w + 3u, q.u);
    store3(w + 6u, q.v);
    build_words.data[w + 9u] = packUnorm4x8(q.color);
    build_words.data[w + 10u] = floatBitsToUint(q.radius);
}

Quad no_quad()
{
    Quad q;
    q.c = vec3(0.0);
    q.u = vec3(0.0);
    q.v = vec3(0.0);
    q.color = vec4(0.0);
    q.radius = -1.0;
    return q;
}

// A member's quad: its mesh's, placed by its world matrix.
Quad leaf(mat4 world, uint mesh_word)
{
    Quad q;
    q.c = (world * vec4(load3(mesh_word), 1.0)).xyz;
    q.u = mat3(world) * load3(mesh_word + 3u);
    q.v = mat3(world) * load3(mesh_word + 6u);
    uint color = build_words.data[mesh_word + 9u];
    q.color = unpackUnorm4x8(color);
    q.radius = color == 0u ? -1.0 : length(q.u) + length(q.v);
    return q;
}

// One quad with both their areas: centre and colour weighted by area, facing the way their
// surfaces face together, shaped as the larger one but with its longer side at least either
// one's. Its sphere holds both of theirs. So a node's size on screen never shrinks going up.
// Mirrored by merge_tree_quads in C++: change both together.
Quad merge(Quad a, Quad b)
{
    if (a.radius < 0.0) return b;
    if (b.radius < 0.0) return a;
    // A quad's u cross v is as long as a quarter of its area.
    vec3 na = cross(a.u, a.v);
    vec3 nb = cross(b.u, b.v);
    float quarter_a = length(na);
    float quarter_b = length(nb);
    float quarter = quarter_a + quarter_b;
    float wa = quarter > 0.0 ? quarter_a / quarter : 0.5;
    Quad big = quarter_a >= quarter_b ? a : b;

    Quad q;
    q.c = mix(b.c, a.c, wa);
    q.color = mix(b.color, a.color, wa);
    vec3 n = na + nb;
    if (dot(n, n) <= 1e-12 * quarter * quarter) {
        n = cross(big.u, big.v);
    }
    n = dot(n, n) > 0.0 ? normalize(n) : vec3(0.0, 1.0, 0.0);
    vec3 along = big.u - dot(big.u, n) * n;
    if (dot(along, along) <= 1e-12 * dot(big.u, big.u)) {
        along = abs(n.x) < 0.9 ? cross(n, vec3(1.0, 0.0, 0.0)) : cross(n, vec3(0.0, 1.0, 0.0));
    }
    along = normalize(along);
    float lu = length(big.u);
    float lv = length(big.v);
    float ratio = lv > 0.0 ? lu / lv : 1.0;
    // |u| |v| is then the two's quarters together.
    float hu = sqrt(quarter * ratio);
    float hv = sqrt(quarter / max(ratio, 1e-6));
    // Stretched along its longer side where a child's is longer, the other side shortened to keep the area.
    float longest = max(max(length(a.u), length(a.v)), max(length(b.u), length(b.v)));
    if (max(hu, hv) < longest) {
        if (hu >= hv) {
            hu = longest;
            hv = quarter / longest;
        } else {
            hv = longest;
            hu = quarter / longest;
        }
    }
    q.u = along * hu;
    q.v = cross(n, along) * hv;
    q.radius = max(max(length(a.c - q.c) + a.radius, length(b.c - q.c) + b.radius), length(q.u) + length(q.v));
    return q;
}

mat4 resting(uint row, uint index)
{
    vec4 row0 = build_rows.data[row + index * 3u];
    vec4 row1 = build_rows.data[row + index * 3u + 1u];
    vec4 row2 = build_rows.data[row + index * 3u + 2u];
    return transpose(mat4(row0, row1, row2, vec4(0.0, 0.0, 0.0, 1.0)));
}

void main()
{
    uint job = pc.jobs_word + gl_WorkGroupID.x * kJobWords;
    uint members = build_words.data[job];
    uint member_count = build_words.data[job + 1u];
    uint tree = build_words.data[job + 2u];
    uint leaves = build_words.data[job + 3u];
    uint table = build_words.data[job + 4u];
    uint rest_row = build_words.data[job + 5u];
    uint instances_base = build_words.data[job + 6u];
    uint mesh_count = build_words.data[job + 7u];

    for (uint i = gl_LocalInvocationID.x; i < leaves; i += gl_WorkGroupSize.x) {
        Quad q = no_quad();
        if (instances_base == kNone) {
            // Plain objects: their quads in the world already, one each in the table.
            if (i < member_count) q = leaf(mat4(1.0), table + i * kMeshQuadWords);
            store_node(tree + (leaves - 1u + i) * kNodeWords, q);
            continue;
        }
        uint index = i < member_count ? build_words.data[members + i] : kNone;
        if (index != kNone) {
            BuildInstance inst = build_instances.data[instances_base + index];
            uint mesh = inst.params.y;
            if (mesh < mesh_count) {
                mat4 world = rest_row != kNone ? resting(rest_row, index) : inst.world_matrix;
                q = leaf(world, table + mesh * kMeshQuadWords);
            }
        }
        store_node(tree + (leaves - 1u + i) * kNodeWords, q);
    }
    for (uint width = leaves >> 1u; width > 0u; width >>= 1u) {
        memoryBarrierBuffer();
        barrier();
        for (uint i = gl_LocalInvocationID.x; i < width; i += gl_WorkGroupSize.x) {
            uint node = width - 1u + i;
            store_node(tree + node * kNodeWords,
                       merge(load_node(tree + (2u * node + 1u) * kNodeWords), load_node(tree + (2u * node + 2u) * kNodeWords)));
        }
    }
}
)";

} // namespace velk::impl
