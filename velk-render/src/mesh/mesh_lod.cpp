#include "mesh/mesh_lod.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <limits>

#include "meshoptimizer.h"

namespace velk::impl {

namespace {

/// Clusters simplified together. Partitions come out up to a third larger.
constexpr size_t kGroupClusters = 16;
/// A group that keeps more than this share of its triangles stops there, and its clusters
/// are roots of the hierarchy.
constexpr float kStopAboveKept = 0.85f;
constexpr uint32_t kMaxLevels = 32;
constexpr uint32_t kNoGroup = 0xFFFFFFFFu;

/// Every vertex's lowest-numbered vertex at the same position, since vertices split for
/// normals or uvs are one point of the surface.
vector<uint32_t> position_remap(const vector<float>& positions)
{
    const size_t count = positions.size() / 3;
    vector<uint32_t> order;
    order.resize(count);
    for (size_t i = 0; i < count; ++i) {
        order[i] = static_cast<uint32_t>(i);
    }
    const float* p = positions.data();
    const auto less = [p](uint32_t a, uint32_t b) {
        for (int k = 0; k < 3; ++k) {
            const float pa = p[size_t(a) * 3 + k];
            const float pb = p[size_t(b) * 3 + k];
            if (pa != pb) {
                return pa < pb;
            }
        }
        return a < b;
    };
    std::sort(order.begin(), order.end(), less);

    vector<uint32_t> remap;
    remap.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t v = order[i];
        const bool same = i > 0 && std::memcmp(p + size_t(order[i - 1]) * 3, p + size_t(v) * 3, sizeof(float) * 3) == 0;
        remap[v] = same ? remap[order[i - 1]] : v;
    }
    return remap;
}

/// The mesh's positions by vertex id, from its clusters.
vector<float> gather_positions(const BlasBuild& blas)
{
    uint32_t vertex_count = 0;
    for (const auto& cluster : blas.clusters) {
        for (uint32_t i = 0; i < cluster.vertex_count; ++i) {
            vertex_count = std::max(vertex_count, blas.cluster_words[cluster.position_word + cluster.vertex_count * 3 + i] + 1u);
        }
    }
    vector<float> positions;
    positions.resize(size_t(vertex_count) * 3, 0.f);
    for (const auto& cluster : blas.clusters) {
        for (uint32_t i = 0; i < cluster.vertex_count; ++i) {
            const uint32_t id = blas.cluster_words[cluster.position_word + cluster.vertex_count * 3 + i];
            std::memcpy(positions.data() + size_t(id) * 3, blas.cluster_words.data() + cluster.position_word + i * 3,
                        sizeof(float) * 3);
        }
    }
    return positions;
}

/// Level 0 as index lists over the mesh's vertices, a list per cluster.
vector<vector<uint32_t>> gather_clusters(const BlasBuild& blas)
{
    vector<BlasBuild::Cluster> sorted;
    sorted.reserve(blas.clusters.size());
    for (const auto& cluster : blas.clusters) {
        sorted.push_back(cluster);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const BlasBuild::Cluster& a, const BlasBuild::Cluster& b) { return a.position_word < b.position_word; });

    vector<vector<uint32_t>> lists;
    lists.resize(sorted.size());
    for (const auto& node : blas.nodes) {
        const uint32_t triangles = node.shape_count & 0xFFFFu;
        if (node.child_count != 0 || triangles == 0) {
            continue;
        }
        const uint32_t vertex_count = node.shape_count >> 16;
        const uint32_t ids = node.first_child + vertex_count * 3;
        const auto* found = std::lower_bound(sorted.begin(), sorted.end(), node.first_child,
                                             [](const BlasBuild::Cluster& c, uint32_t word) { return c.position_word < word; });
        auto& list = lists[static_cast<size_t>(found - sorted.begin())];
        for (uint32_t t = 0; t < triangles; ++t) {
            const uint32_t word = blas.cluster_words[node.first_shape + t];
            list.push_back(blas.cluster_words[ids + (word & 0xFFu)]);
            list.push_back(blas.cluster_words[ids + ((word >> 8) & 0xFFu)]);
            list.push_back(blas.cluster_words[ids + ((word >> 16) & 0xFFu)]);
        }
    }
    return lists;
}

/// Appends a cluster from its index list. Without @p sphere, its sphere is its vertices'.
void add_cluster(MeshLod& lod, const vector<uint32_t>& indices, const vector<float>& positions, uint32_t level,
                 const float* sphere, float error)
{
    MeshLodCluster cluster;
    cluster.first_index = static_cast<uint32_t>(lod.indices.size());
    cluster.triangle_count = static_cast<uint32_t>(indices.size() / 3);
    cluster.level = level;
    cluster.error = error;
    cluster.parent_error = std::numeric_limits<float>::max();
    if (sphere) {
        std::memcpy(cluster.center, sphere, sizeof(float) * 3);
        cluster.radius = sphere[3];
    } else {
        vector<float> points;
        points.reserve(indices.size() * 3);
        for (uint32_t id : indices) {
            for (int k = 0; k < 3; ++k) {
                points.push_back(positions[size_t(id) * 3 + k]);
            }
        }
        const meshopt_Bounds bounds = meshopt_computeSphereBounds(points.data(), indices.size(), sizeof(float) * 3, nullptr, 0);
        std::memcpy(cluster.center, bounds.center, sizeof(float) * 3);
        cluster.radius = bounds.radius;
    }
    lod.indices.insert(lod.indices.end(), indices.begin(), indices.end());
    lod.clusters.push_back(cluster);
}

} // namespace

MeshLod build_mesh_lod(const BlasBuild& blas)
{
    MeshLod lod;
    if (blas.clusters.empty()) {
        return lod;
    }
    const vector<float> positions = gather_positions(blas);
    const vector<uint32_t> remap = position_remap(positions);
    const size_t vertex_count = positions.size() / 3;

    // Index lists of all clusters so far, parallel to lod.clusters.
    vector<vector<uint32_t>> lists = gather_clusters(blas);
    vector<uint32_t> pending;
    for (size_t i = 0; i < lists.size(); ++i) {
        add_cluster(lod, lists[i], positions, 0, nullptr, 0.f);
        pending.push_back(static_cast<uint32_t>(i));
    }
    lod.level_count = 1;

    vector<uint32_t> first_seen;
    vector<unsigned char> locks;
    first_seen.resize(vertex_count);
    locks.resize(vertex_count);

    for (uint32_t level = 1; level < kMaxLevels && pending.size() > 1; ++level) {
        vector<uint32_t> all_indices;
        vector<uint32_t> index_counts;
        for (uint32_t c : pending) {
            all_indices.insert(all_indices.end(), lists[c].begin(), lists[c].end());
            index_counts.push_back(static_cast<uint32_t>(lists[c].size()));
        }
        vector<uint32_t> partition;
        partition.resize(pending.size());
        const size_t group_count = meshopt_partitionClusters(partition.data(), all_indices.data(), all_indices.size(),
                                                             index_counts.data(), pending.size(), positions.data(), vertex_count,
                                                             sizeof(float) * 3, kGroupClusters);
        vector<vector<uint32_t>> groups;
        groups.resize(group_count);
        for (size_t i = 0; i < pending.size(); ++i) {
            groups[partition[i]].push_back(pending[i]);
        }

        // Whatever two groups share stays where it is, so that any mix of levels across
        // their edge still meets without a crack.
        std::fill(first_seen.begin(), first_seen.end(), kNoGroup);
        std::fill(locks.begin(), locks.end(), static_cast<unsigned char>(0));
        for (uint32_t g = 0; g < group_count; ++g) {
            for (uint32_t c : groups[g]) {
                for (uint32_t v : lists[c]) {
                    const uint32_t point = remap[v];
                    if (first_seen[point] == kNoGroup) {
                        first_seen[point] = g;
                    } else if (first_seen[point] != g) {
                        locks[point] = meshopt_SimplifyVertex_Lock;
                    }
                }
            }
        }
        for (size_t v = 0; v < vertex_count; ++v) {
            locks[v] = locks[remap[v]];
        }

        vector<uint32_t> next;
        for (const auto& group : groups) {
            vector<uint32_t> merged;
            float children_error = 0.f;
            vector<float> spheres;
            for (uint32_t c : group) {
                merged.insert(merged.end(), lists[c].begin(), lists[c].end());
                const MeshLodCluster& child = lod.clusters[c];
                children_error = std::max(children_error, child.error);
                spheres.push_back(child.center[0]);
                spheres.push_back(child.center[1]);
                spheres.push_back(child.center[2]);
                spheres.push_back(child.radius);
            }

            vector<uint32_t> simplified;
            simplified.resize(merged.size());
            float error = 0.f;
            const size_t target = merged.size() / 6 * 3;
            const size_t kept = meshopt_simplifyWithAttributes(
                simplified.data(), merged.data(), merged.size(), positions.data(), vertex_count, sizeof(float) * 3, nullptr, 0,
                nullptr, 0, locks.data(), target, FLT_MAX, meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute, &error);
            if (kept == 0 || static_cast<float>(kept) > kStopAboveKept * static_cast<float>(merged.size())) {
                continue;
            }

            const meshopt_Bounds merged_bounds = meshopt_computeSphereBounds(spheres.data(), group.size(), sizeof(float) * 4,
                                                                             spheres.data() + 3, sizeof(float) * 4);
            const float sphere[4] = {merged_bounds.center[0], merged_bounds.center[1], merged_bounds.center[2],
                                     merged_bounds.radius};
            // Errors add up, since each step is measured against the one before.
            const float group_error = children_error + error;
            for (uint32_t c : group) {
                MeshLodCluster& child = lod.clusters[c];
                std::memcpy(child.parent_center, sphere, sizeof(float) * 3);
                child.parent_radius = sphere[3];
                child.parent_error = group_error;
            }

            const size_t bound = meshopt_buildMeshletsBound(kept, kClusterMaxVertices, kClusterMaxTriangles);
            vector<meshopt_Meshlet> meshlets;
            vector<unsigned int> meshlet_vertices;
            vector<unsigned char> meshlet_triangles;
            meshlets.resize(bound);
            meshlet_vertices.resize(kept);
            meshlet_triangles.resize(kept);
            const size_t meshlet_count = meshopt_buildMeshlets(meshlets.data(), meshlet_vertices.data(), meshlet_triangles.data(),
                                                               simplified.data(), kept, positions.data(), vertex_count,
                                                               sizeof(float) * 3, kClusterMaxVertices, kClusterMaxTriangles, 0.f);
            for (size_t m = 0; m < meshlet_count; ++m) {
                const meshopt_Meshlet& meshlet = meshlets[m];
                vector<uint32_t> indices;
                indices.reserve(size_t(meshlet.triangle_count) * 3);
                for (uint32_t i = 0; i < meshlet.triangle_count * 3; ++i) {
                    indices.push_back(meshlet_vertices[meshlet.vertex_offset + meshlet_triangles[meshlet.triangle_offset + i]]);
                }
                next.push_back(static_cast<uint32_t>(lod.clusters.size()));
                add_cluster(lod, indices, positions, level, sphere, group_error);
                lists.push_back(std::move(indices));
            }
        }
        if (!next.empty()) {
            lod.level_count = level + 1;
        }
        pending = std::move(next);
    }
    return lod;
}

} // namespace velk::impl
