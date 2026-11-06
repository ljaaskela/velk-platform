#ifndef VELK_RENDER_BLAS_H
#define VELK_RENDER_BLAS_H

#include <velk/vector.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

namespace velk {

/// GPU-side BLAS node. Mirrors the GLSL `BvhNode` layout in
/// shader_compiler.cpp (and the analogous `GpuBvhNode` in
/// scene_collector.h on the velk-ui side). 48 bytes; aabb_min/aabb_max
/// `.w` is padding. For inner nodes, `first_child` is the index of the
/// left child (right child sits at `first_child + 1` because every
/// inner node we emit is binary), `child_count` is 2 and the shape
/// fields are zero. For leaves, `first_shape` indexes into the BLAS's
/// triangle_indices array, `shape_count` is the triangle count, and
/// the child fields are zero.
struct alignas(16) BlasNode
{
    float    aabb_min[4];
    float    aabb_max[4];
    uint32_t first_shape;
    uint32_t shape_count;
    uint32_t first_child;
    uint32_t child_count;
};
static_assert(sizeof(BlasNode) == 48, "BlasNode layout mismatch");

/// Most triangles and vertices in one cluster. Local vertex indices are 8 bits.
inline constexpr uint32_t kClusterMaxTriangles = 256;
inline constexpr uint32_t kClusterMaxVertices = 256;

/// A tree over a set of primitives. `triangle_indices` is the per-leaf
/// permutation: leaf node `n` covers items
/// `triangle_indices[n.first_shape .. n.first_shape + n.shape_count]`.
///
/// `build_mesh_blas` also groups the tree's leaves into clusters and fills
/// `cluster_words`, one block per cluster:
///
///   triangles   one word each, three 8-bit local vertex indices
///   positions   three float words per local vertex
///   vertex ids  one word per local vertex, its index in the mesh's vertices
///
/// A mesh leaf then reads, with all offsets into `cluster_words`:
///   first_shape               its first triangle word
///   shape_count & 0xFFFF      its triangle count
///   shape_count >> 16         its cluster's vertex count
///   first_child               its cluster's first position word
/// and `triangle_indices` is empty. A child's index is always greater than its
/// parent's.
struct BlasBuild
{
    /// Where one cluster's positions are in `cluster_words`.
    struct Cluster
    {
        uint32_t position_word;
        uint32_t vertex_count;
    };

    vector<BlasNode> nodes;
    vector<uint32_t> triangle_indices;
    vector<uint32_t> cluster_words;
    vector<Cluster>  clusters;
    uint32_t         root_index = 0;
};

namespace detail {

inline void blas_merge_aabb(float bmin[3], float bmax[3],
                            const float t_min[3], const float t_max[3])
{
    for (int i = 0; i < 3; ++i) {
        if (t_min[i] < bmin[i]) bmin[i] = t_min[i];
        if (t_max[i] > bmax[i]) bmax[i] = t_max[i];
    }
}

struct BlasTriInfo
{
    uint32_t index;
    float    aabb_min[3];
    float    aabb_max[3];
    float    center[3];
};

inline void blas_fill_node(BlasBuild& out, uint32_t my_idx,
                           vector<BlasTriInfo>& tris, size_t begin, size_t end)
{
    constexpr size_t kLeafThreshold = 4;
    constexpr float kInf = std::numeric_limits<float>::max();

    float bmin[3] = { kInf,  kInf,  kInf};
    float bmax[3] = {-kInf, -kInf, -kInf};
    for (size_t i = begin; i < end; ++i) {
        blas_merge_aabb(bmin, bmax, tris[i].aabb_min, tris[i].aabb_max);
    }

    BlasNode node{};
    for (int i = 0; i < 3; ++i) {
        node.aabb_min[i] = bmin[i];
        node.aabb_max[i] = bmax[i];
    }
    node.aabb_min[3] = 0.f;
    node.aabb_max[3] = 0.f;

    const size_t count = end - begin;

    auto emit_leaf = [&]() {
        const uint32_t first = static_cast<uint32_t>(out.triangle_indices.size());
        for (size_t i = begin; i < end; ++i) {
            out.triangle_indices.push_back(tris[i].index);
        }
        node.first_shape = first;
        node.shape_count = static_cast<uint32_t>(count);
        node.first_child = 0;
        node.child_count = 0;
        out.nodes[my_idx] = node;
    };

    if (count <= kLeafThreshold) {
        emit_leaf();
        return;
    }

    float cmin[3] = { kInf,  kInf,  kInf};
    float cmax[3] = {-kInf, -kInf, -kInf};
    for (size_t i = begin; i < end; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (tris[i].center[j] < cmin[j]) cmin[j] = tris[i].center[j];
            if (tris[i].center[j] > cmax[j]) cmax[j] = tris[i].center[j];
        }
    }
    int axis = 0;
    const float dx = cmax[0] - cmin[0];
    const float dy = cmax[1] - cmin[1];
    const float dz = cmax[2] - cmin[2];
    if (dy >= dx && dy >= dz)      axis = 1;
    else if (dz >= dx && dz >= dy) axis = 2;

    // Median split. If all centroids coincide on the chosen axis, the
    // sort would be a no-op and the split would oscillate forever — fall
    // back to a leaf.
    if (cmax[axis] == cmin[axis]) {
        emit_leaf();
        return;
    }

    std::sort(tris.begin() + begin, tris.begin() + end,
              [axis](const BlasTriInfo& a, const BlasTriInfo& b) {
                  return a.center[axis] < b.center[axis];
              });

    // SAH split: pick the candidate split position whose
    // surface-area heuristic cost beats both the leaf cost and any
    // other split. Same algorithm as the TLAS builder in
    // velk-ui/src/renderer/scene_collector.h::build_scene_bvh.
    constexpr float kCostTraversal = 1.0f;
    constexpr float kCostIntersect = 1.5f;
    auto surface_area = [](const float lo[3], const float hi[3]) {
        const float dx = std::max(0.f, hi[0] - lo[0]);
        const float dy = std::max(0.f, hi[1] - lo[1]);
        const float dz = std::max(0.f, hi[2] - lo[2]);
        return 2.f * (dx * dy + dy * dz + dz * dx);
    };

    // Right-to-left sweep: right_lo[i] / right_hi[i] = AABB of
    // tris[begin + i .. end). Only allocated when we need them.
    vector<float> right_lo(count * 3);
    vector<float> right_hi(count * 3);
    {
        float lo[3] = { kInf,  kInf,  kInf};
        float hi[3] = {-kInf, -kInf, -kInf};
        for (size_t i = count; i-- > 0;) {
            const auto& t = tris[begin + i];
            for (int k = 0; k < 3; ++k) {
                if (t.aabb_min[k] < lo[k]) lo[k] = t.aabb_min[k];
                if (t.aabb_max[k] > hi[k]) hi[k] = t.aabb_max[k];
                right_lo[i * 3 + k] = lo[k];
                right_hi[i * 3 + k] = hi[k];
            }
        }
    }

    const float parent_sa = surface_area(bmin, bmax);
    const float leaf_cost = kCostIntersect * static_cast<float>(count);

    float best_cost = leaf_cost;
    size_t best_split = 0; // 0 == fall back to leaf

    float left_lo[3] = { kInf,  kInf,  kInf};
    float left_hi[3] = {-kInf, -kInf, -kInf};
    for (size_t j = 0; j + 1 < count; ++j) {
        const auto& t = tris[begin + j];
        for (int k = 0; k < 3; ++k) {
            if (t.aabb_min[k] < left_lo[k]) left_lo[k] = t.aabb_min[k];
            if (t.aabb_max[k] > left_hi[k]) left_hi[k] = t.aabb_max[k];
        }
        const float left_sa = surface_area(left_lo, left_hi);
        const float right_sa = surface_area(
            right_lo.data() + (j + 1) * 3, right_hi.data() + (j + 1) * 3);
        const float l_count = static_cast<float>(j + 1);
        const float r_count = static_cast<float>(count - j - 1);
        const float cost = kCostTraversal + kCostIntersect *
            (left_sa * l_count + right_sa * r_count) / std::max(parent_sa, 1e-30f);
        if (cost < best_cost) {
            best_cost = cost;
            best_split = j + 1;
        }
    }

    if (best_split == 0) {
        emit_leaf();
        return;
    }

    const size_t mid = begin + best_split;

    const uint32_t left_idx  = static_cast<uint32_t>(out.nodes.size());
    out.nodes.push_back({});
    const uint32_t right_idx = static_cast<uint32_t>(out.nodes.size());
    out.nodes.push_back({});

    node.first_shape = 0;
    node.shape_count = 0;
    node.first_child = left_idx;
    node.child_count = 2;
    out.nodes[my_idx] = node;

    blas_fill_node(out, left_idx,  tris, begin, mid);
    blas_fill_node(out, right_idx, tris, mid,   end);
}

/// Turns a tree over a mesh's triangles into clusters.
struct ClusterBuilder
{
    BlasBuild& out;
    const float* vertices;
    uint32_t floats_per_vertex;
    const uint32_t* indices;
    vector<uint32_t> subtree_triangles;  ///< Per node.
    vector<uint32_t> leaf_triangles;     ///< Triangle ids of the leaves being grouped.
    vector<uint32_t> local_vertices;     ///< Sorted vertex ids of the current cluster.

    uint32_t unique_vertices(const uint32_t* triangles, size_t count)
    {
        local_vertices.clear();
        for (size_t t = 0; t < count; ++t) {
            for (uint32_t c = 0; c < 3; ++c) {
                local_vertices.push_back(indices[size_t(triangles[t]) * 3 + c]);
            }
        }
        std::sort(local_vertices.begin(), local_vertices.end());
        local_vertices.erase(std::unique(local_vertices.begin(), local_vertices.end()), local_vertices.end());
        return static_cast<uint32_t>(local_vertices.size());
    }

    /// A leaf too large for one cluster, which SAH keeps when splitting would not
    /// pay, becomes two children over halves of its triangles.
    void split_oversized_leaves()
    {
        for (uint32_t n = 0; n < out.nodes.size(); ++n) {
            const BlasNode node = out.nodes[n];
            if (node.child_count != 0 || node.shape_count <= 1) {
                continue;
            }
            const uint32_t* first = out.triangle_indices.data() + node.first_shape;
            if (node.shape_count <= kClusterMaxTriangles
                && unique_vertices(first, node.shape_count) <= kClusterMaxVertices) {
                continue;
            }
            const uint32_t half = node.shape_count / 2;
            const auto left = static_cast<uint32_t>(out.nodes.size());
            out.nodes.push_back(leaf_over(node.first_shape, half));
            out.nodes.push_back(leaf_over(node.first_shape + half, node.shape_count - half));
            BlasNode& parent = out.nodes[n];
            parent.first_shape = 0;
            parent.shape_count = 0;
            parent.first_child = left;
            parent.child_count = 2;
        }
    }

    BlasNode leaf_over(uint32_t first, uint32_t count) const
    {
        BlasNode node{};
        node.first_shape = first;
        node.shape_count = count;
        for (int k = 0; k < 3; ++k) {
            node.aabb_min[k] = std::numeric_limits<float>::max();
            node.aabb_max[k] = -std::numeric_limits<float>::max();
        }
        for (uint32_t t = 0; t < count; ++t) {
            const size_t triangle = out.triangle_indices[first + t];
            for (uint32_t c = 0; c < 3; ++c) {
                const float* v = vertices + size_t(indices[triangle * 3 + c]) * floats_per_vertex;
                for (int k = 0; k < 3; ++k) {
                    node.aabb_min[k] = std::min(node.aabb_min[k], v[k]);
                    node.aabb_max[k] = std::max(node.aabb_max[k], v[k]);
                }
            }
        }
        return node;
    }

    uint32_t count_triangles(uint32_t n)
    {
        const BlasNode node = out.nodes[n];
        uint32_t count = node.shape_count;
        for (uint32_t c = 0; c < node.child_count; ++c) {
            count += count_triangles(node.first_child + c);
        }
        subtree_triangles[n] = count;
        return count;
    }

    void gather_leaf_triangles(uint32_t n)
    {
        const BlasNode& node = out.nodes[n];
        for (uint32_t t = 0; t < node.shape_count; ++t) {
            leaf_triangles.push_back(out.triangle_indices[node.first_shape + t]);
        }
        for (uint32_t c = 0; c < node.child_count; ++c) {
            gather_leaf_triangles(node.first_child + c);
        }
    }

    /// Points the leaves under @p n at their triangles, in the order they were gathered.
    void stamp_leaves(uint32_t n, uint32_t& next_triangle_word, uint32_t position_word, uint32_t vertex_count)
    {
        BlasNode& node = out.nodes[n];
        if (node.child_count != 0) {
            const uint32_t first_child = node.first_child;
            const uint32_t child_count = node.child_count;
            for (uint32_t c = 0; c < child_count; ++c) {
                stamp_leaves(first_child + c, next_triangle_word, position_word, vertex_count);
            }
            return;
        }
        const uint32_t count = node.shape_count;
        node.first_shape = next_triangle_word;
        node.shape_count = count | (vertex_count << 16);
        node.first_child = position_word;
        next_triangle_word += count;
    }

    void emit_cluster(uint32_t n)
    {
        leaf_triangles.clear();
        gather_leaf_triangles(n);
        const uint32_t vertex_count = unique_vertices(leaf_triangles.data(), leaf_triangles.size());

        auto& words = out.cluster_words;
        auto triangle_word = static_cast<uint32_t>(words.size());
        const auto position_word = triangle_word + static_cast<uint32_t>(leaf_triangles.size());
        for (uint32_t triangle : leaf_triangles) {
            uint32_t word = 0;
            for (uint32_t c = 0; c < 3; ++c) {
                const uint32_t id = indices[size_t(triangle) * 3 + c];
                const auto local = static_cast<uint32_t>(
                    std::lower_bound(local_vertices.begin(), local_vertices.end(), id) - local_vertices.begin());
                word |= local << (c * 8);
            }
            words.push_back(word);
        }
        for (uint32_t id : local_vertices) {
            const float* v = vertices + size_t(id) * floats_per_vertex;
            for (int k = 0; k < 3; ++k) {
                uint32_t bits;
                std::memcpy(&bits, &v[k], sizeof(bits));
                words.push_back(bits);
            }
        }
        for (uint32_t id : local_vertices) {
            words.push_back(id);
        }
        stamp_leaves(n, triangle_word, position_word, vertex_count);
        out.clusters.push_back({position_word, vertex_count});
    }

    /// The largest subtrees that fit in a cluster become clusters.
    void assign(uint32_t n)
    {
        if (subtree_triangles[n] <= kClusterMaxTriangles) {
            leaf_triangles.clear();
            gather_leaf_triangles(n);
            if (unique_vertices(leaf_triangles.data(), leaf_triangles.size()) <= kClusterMaxVertices) {
                emit_cluster(n);
                return;
            }
        }
        const uint32_t first_child = out.nodes[n].first_child;
        const uint32_t child_count = out.nodes[n].child_count;
        for (uint32_t c = 0; c < child_count; ++c) {
            assign(first_child + c);
        }
    }

    void build()
    {
        split_oversized_leaves();
        subtree_triangles.resize(out.nodes.size());
        count_triangles(0);
        assign(0);
        out.triangle_indices.clear();
    }
};

} // namespace detail

/// Builds a mesh primitive's tree and clusters. Vertices are the flat float array
/// of the mesh's VBO, with positions first; vertex_stride_bytes is the stride
/// between consecutive vertices. Indices is the flat uint32 array of the mesh's IBO
/// (3 indices per triangle). The vertex/index data is read once during build and
/// not retained.
inline BlasBuild build_mesh_blas(const float* vertices, uint32_t vertex_stride_bytes,
                                 uint32_t vertex_count,
                                 const uint32_t* indices, uint32_t triangle_count)
{
    BlasBuild out;
    if (!vertices || !indices || triangle_count == 0 || vertex_count == 0
        || vertex_stride_bytes == 0 || (vertex_stride_bytes % sizeof(float)) != 0) {
        return out;
    }
    const uint32_t floats_per_vertex = vertex_stride_bytes / sizeof(float);

    vector<detail::BlasTriInfo> tris;
    tris.resize(triangle_count);
    for (uint32_t t = 0; t < triangle_count; ++t) {
        const uint32_t i0 = indices[t * 3 + 0];
        const uint32_t i1 = indices[t * 3 + 1];
        const uint32_t i2 = indices[t * 3 + 2];
        if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) {
            // Malformed mesh; bail out empty so the shader skips RT
            // for it instead of risking OOB reads.
            return BlasBuild{};
        }
        const float* v0 = vertices + static_cast<size_t>(i0) * floats_per_vertex;
        const float* v1 = vertices + static_cast<size_t>(i1) * floats_per_vertex;
        const float* v2 = vertices + static_cast<size_t>(i2) * floats_per_vertex;
        auto& ti = tris[t];
        ti.index = t;
        for (int j = 0; j < 3; ++j) {
            const float a = v0[j], b = v1[j], c = v2[j];
            ti.aabb_min[j] = std::min({a, b, c});
            ti.aabb_max[j] = std::max({a, b, c});
            ti.center[j]   = (ti.aabb_min[j] + ti.aabb_max[j]) * 0.5f;
        }
    }

    out.nodes.push_back({});
    out.root_index = 0;
    detail::blas_fill_node(out, 0, tris, 0, tris.size());
    detail::ClusterBuilder{out, vertices, floats_per_vertex, indices}.build();
    return out;
}

/// Moves a mesh's clusters and tree onto new vertex positions, with its triangles
/// and tree shape unchanged. @p vertices has the layout the tree was built from.
inline void refit_mesh_blas(BlasBuild& blas, const float* vertices, uint32_t vertex_stride_bytes)
{
    const uint32_t floats_per_vertex = vertex_stride_bytes / sizeof(float);
    uint32_t* words = blas.cluster_words.data();
    for (const auto& cluster : blas.clusters) {
        uint32_t* positions = words + cluster.position_word;
        const uint32_t* ids = positions + size_t(cluster.vertex_count) * 3;
        for (uint32_t v = 0; v < cluster.vertex_count; ++v) {
            std::memcpy(positions + size_t(v) * 3, vertices + size_t(ids[v]) * floats_per_vertex, sizeof(float) * 3);
        }
    }

    // Children come after their parent, so a reverse sweep sees them first.
    for (size_t n = blas.nodes.size(); n-- > 0;) {
        BlasNode& node = blas.nodes[n];
        float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max()};
        float hi[3] = {-lo[0], -lo[0], -lo[0]};
        if (node.child_count != 0) {
            for (uint32_t c = 0; c < node.child_count; ++c) {
                const BlasNode& child = blas.nodes[node.first_child + c];
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], child.aabb_min[k]);
                    hi[k] = std::max(hi[k], child.aabb_max[k]);
                }
            }
        } else {
            const uint32_t* positions = words + node.first_child;
            const uint32_t triangle_count = node.shape_count & 0xFFFFu;
            for (uint32_t t = 0; t < triangle_count; ++t) {
                const uint32_t triangle = words[node.first_shape + t];
                for (uint32_t c = 0; c < 3; ++c) {
                    float p[3];
                    std::memcpy(p, positions + size_t((triangle >> (c * 8)) & 0xFFu) * 3, sizeof(p));
                    for (int k = 0; k < 3; ++k) {
                        lo[k] = std::min(lo[k], p[k]);
                        hi[k] = std::max(hi[k], p[k]);
                    }
                }
            }
        }
        for (int k = 0; k < 3; ++k) {
            node.aabb_min[k] = lo[k];
            node.aabb_max[k] = hi[k];
        }
    }
}

/// The nodes' total surface area against the root's. Grows as a refit tree degrades.
inline float blas_tree_cost(const BlasBuild& blas)
{
    const auto area = [](const BlasNode& node) {
        const float x = node.aabb_max[0] - node.aabb_min[0];
        const float y = node.aabb_max[1] - node.aabb_min[1];
        const float z = node.aabb_max[2] - node.aabb_min[2];
        return 2.f * (x * y + y * z + z * x);
    };
    float total = 0.f;
    for (const auto& node : blas.nodes) {
        total += area(node);
    }
    return blas.nodes.empty() ? 0.f : total / std::max(area(blas.nodes[blas.root_index]), 1e-30f);
}

} // namespace velk

#endif // VELK_RENDER_BLAS_H
