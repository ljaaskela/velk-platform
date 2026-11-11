#include "mesh/mesh_lod.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>

#include "meshoptimizer.h"

#include <velk-render/api/half_float.h>

namespace velk::impl {

namespace {

/// Clusters simplified together. Partitions come out up to a third larger.
constexpr size_t kGroupClusters = 16;
/// A group that keeps more than this share of its triangles stops there, and its clusters
/// are roots of the hierarchy.
constexpr float kStopAboveKept = 0.85f;
constexpr uint32_t kMaxLevels = 32;
constexpr uint32_t kNoGroup = 0xFFFFFFFFu;
/// A normal turning fully over a distance counts as much as moving by that distance.
constexpr float kNormalWeights[3] = {1.f, 1.f, 1.f};

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

/// Clusters are made for rays, and can split a separate piece. That would lock its vertices as
/// shared between groups, so each piece moves to one cluster.
void gather_pieces(vector<vector<uint32_t>>& lists, const vector<uint32_t>& remap)
{
    vector<uint32_t> parent;
    parent.resize(remap.size());
    for (size_t i = 0; i < parent.size(); ++i) {
        parent[i] = static_cast<uint32_t>(i);
    }
    const auto find = [&parent](uint32_t s) {
        while (parent[s] != s) {
            parent[s] = parent[parent[s]];
            s = parent[s];
        }
        return s;
    };
    for (const auto& list : lists) {
        for (size_t i = 0; i + 2 < list.size(); i += 3) {
            const uint32_t a = find(remap[list[i]]);
            parent[find(remap[list[i + 1]])] = a;
            parent[find(remap[list[i + 2]])] = a;
        }
    }
    vector<uint32_t> owner;
    owner.resize(remap.size(), kNoGroup);
    vector<vector<uint32_t>> gathered;
    gathered.resize(lists.size());
    for (size_t c = 0; c < lists.size(); ++c) {
        const auto& list = lists[c];
        for (size_t i = 0; i + 2 < list.size(); i += 3) {
            const uint32_t root = find(remap[list[i]]);
            if (owner[root] == kNoGroup) {
                owner[root] = static_cast<uint32_t>(c);
            }
            const size_t target = owner[root];
            gathered[target].push_back(list[i]);
            gathered[target].push_back(list[i + 1]);
            gathered[target].push_back(list[i + 2]);
        }
    }
    lists = std::move(gathered);
}

float triangle_area(const vector<float>& positions, uint32_t a, uint32_t b, uint32_t c)
{
    const float* pa = positions.data() + size_t(a) * 3;
    const float* pb = positions.data() + size_t(b) * 3;
    const float* pc = positions.data() + size_t(c) * 3;
    const float u[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
    const float v[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
    const float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    return 0.5f * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
}

/// The mesh's vertices as a level sees them, which grows as pieces merge.
struct LodVertices
{
    vector<float> positions;
    vector<float> normals;
    vector<uint32_t> remap;
    vector<unsigned char> locks;
    vector<uint8_t> bytes;
    uint32_t stride = 0;
    uint32_t normal_offset = 0;
    bool grown = false;
};

/// For a primitive whose separate pieces stand for a whole rather than themselves, which a
/// surface simplifier flattens to slivers at almost no reported error: merges each piece with
/// its nearest, into one between them with both their areas, so that a level looks like the
/// one before it. @p kept gets the result, over new vertices, and @p error how far the surface
/// moved. False if the group has fewer than two pieces, or any locked vertex.
bool merge_piece_pairs(LodVertices& v, const vector<uint32_t>& merged, vector<uint32_t>& kept, float& error)
{
    vector<uint32_t> sorted;
    sorted.reserve(merged.size());
    for (uint32_t id : merged) {
        sorted.push_back(v.remap[id]);
    }
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    const auto slot = [&sorted, &v](uint32_t id) {
        return static_cast<uint32_t>(std::lower_bound(sorted.begin(), sorted.end(), v.remap[id]) - sorted.begin());
    };
    vector<uint32_t> parent;
    parent.resize(sorted.size());
    for (size_t i = 0; i < parent.size(); ++i) {
        parent[i] = static_cast<uint32_t>(i);
    }
    const auto find = [&parent](uint32_t s) {
        while (parent[s] != s) {
            parent[s] = parent[parent[s]];
            s = parent[s];
        }
        return s;
    };
    for (size_t i = 0; i + 2 < merged.size(); i += 3) {
        const uint32_t a = find(slot(merged[i]));
        parent[find(slot(merged[i + 1]))] = a;
        parent[find(slot(merged[i + 2]))] = a;
    }
    for (uint32_t point : sorted) {
        if (v.locks[point] != 0) {
            return false;
        }
    }

    // Pieces numbered in the order they first appear, with their bounds.
    constexpr uint32_t kNone = 0xFFFFFFFFu;
    vector<uint32_t> number;
    number.resize(sorted.size(), kNone);
    vector<float> bounds;
    uint32_t pieces = 0;
    for (size_t i = 0; i < merged.size(); ++i) {
        const uint32_t root = find(slot(merged[i]));
        if (number[root] == kNone) {
            number[root] = pieces++;
            const float* p = v.positions.data() + size_t(merged[i]) * 3;
            bounds.insert(bounds.end(), p, p + 3);
            bounds.insert(bounds.end(), p, p + 3);
        }
        float* box = bounds.data() + size_t(number[root]) * 6;
        for (int k = 0; k < 3; ++k) {
            const float value = v.positions[size_t(merged[i]) * 3 + k];
            box[k] = std::min(box[k], value);
            box[k + 3] = std::max(box[k + 3], value);
        }
    }
    if (pieces < 2) {
        return false;
    }

    // Each piece's middle and area.
    vector<float> centers;
    centers.resize(size_t(pieces) * 3, 0.f);
    vector<float> areas;
    areas.resize(pieces, 0.f);
    vector<float> normals;
    normals.resize(size_t(pieces) * 3, 0.f);
    for (size_t i = 0; i + 2 < merged.size(); i += 3) {
        const uint32_t piece = number[find(slot(merged[i]))];
        const float area = std::max(triangle_area(v.positions, merged[i], merged[i + 1], merged[i + 2]), 1e-12f);
        areas[piece] += area;
        for (int k = 0; k < 3; ++k) {
            const float mid = (v.positions[size_t(merged[i]) * 3 + k] + v.positions[size_t(merged[i + 1]) * 3 + k]
                               + v.positions[size_t(merged[i + 2]) * 3 + k]) / 3.f;
            centers[size_t(piece) * 3 + k] += mid * area;
            normals[size_t(piece) * 3 + k] += v.normals[size_t(merged[i]) * 3 + k] * area;
        }
    }
    for (uint32_t piece = 0; piece < pieces; ++piece) {
        for (int k = 0; k < 3; ++k) {
            centers[size_t(piece) * 3 + k] /= areas[piece];
        }
    }

    // Each piece with its nearest free neighbour. The first of a pair stays, and takes on both.
    vector<uint32_t> mate;
    mate.resize(pieces, kNone);
    float width_sum = 0.f;
    float distance_sum = 0.f;
    uint32_t pairs = 0;
    for (uint32_t a = 0; a < pieces; ++a) {
        const float* box = bounds.data() + size_t(a) * 6;
        width_sum += std::max({box[3] - box[0], box[4] - box[1], box[5] - box[2]});
        if (mate[a] != kNone) {
            continue;
        }
        uint32_t nearest = kNone;
        float nearest2 = std::numeric_limits<float>::max();
        for (uint32_t b = a + 1; b < pieces; ++b) {
            if (mate[b] != kNone) {
                continue;
            }
            float d2 = 0.f;
            for (int k = 0; k < 3; ++k) {
                const float d = centers[size_t(a) * 3 + k] - centers[size_t(b) * 3 + k];
                d2 += d * d;
            }
            if (d2 < nearest2) {
                nearest2 = d2;
                nearest = b;
            }
        }
        if (nearest != kNone) {
            mate[a] = nearest;
            mate[nearest] = a;
            distance_sum += std::sqrt(nearest2);
            ++pairs;
        }
    }

    kept.clear();
    vector<uint32_t> moved_from;
    vector<uint32_t> moved_to;
    for (size_t i = 0; i < merged.size(); ++i) {
        const uint32_t a = number[find(slot(merged[i / 3 * 3]))];
        const uint32_t b = mate[a];
        if (b != kNone && b < a) {
            continue;  // Taken on by its pair's first.
        }
        const uint32_t old_id = merged[i];
        if (b == kNone) {
            kept.push_back(old_id);
            continue;
        }
        uint32_t new_id = kNone;
        for (size_t m = moved_from.size(); m > 0 && moved_from.size() - m < 64u; --m) {
            if (moved_from[m - 1] == old_id) {
                new_id = moved_to[m - 1];
                break;
            }
        }
        if (new_id == kNone) {
            const float total = areas[a] + areas[b];
            const float scale = std::sqrt(total / areas[a]);
            float grown[3];
            float normal[3];
            for (int k = 0; k < 3; ++k) {
                const float center = centers[size_t(a) * 3 + k];
                const float together = (center * areas[a] + centers[size_t(b) * 3 + k] * areas[b]) / total;
                grown[k] = together + (v.positions[size_t(old_id) * 3 + k] - center) * scale;
                normal[k] = normals[size_t(a) * 3 + k] + normals[size_t(b) * 3 + k];
            }
            const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            if (length > 1e-12f) {
                for (float& n : normal) {
                    n /= length;
                }
            } else {
                std::memcpy(normal, v.normals.data() + size_t(old_id) * 3, sizeof(normal));
            }
            new_id = static_cast<uint32_t>(v.positions.size() / 3);
            v.positions.insert(v.positions.end(), grown, grown + 3);
            v.normals.insert(v.normals.end(), normal, normal + 3);
            v.remap.push_back(new_id);
            v.locks.push_back(0);
            const size_t first_byte = v.bytes.size();
            v.bytes.resize(first_byte + v.stride);
            std::memcpy(v.bytes.data() + first_byte, v.bytes.data() + size_t(old_id) * v.stride, v.stride);
            std::memcpy(v.bytes.data() + first_byte, grown, sizeof(grown));
            std::memcpy(v.bytes.data() + first_byte + v.normal_offset, normal, sizeof(normal));
            moved_from.push_back(old_id);
            moved_to.push_back(new_id);
            v.grown = true;
        }
        kept.push_back(new_id);
    }
    // A piece moves half the way to its mate, and changes shape by about its size. The mean is
    // what makes a level look different, not one piece that happens to be far from the rest.
    const float mean_width = width_sum / static_cast<float>(pieces);
    const float mean_distance = distance_sum / static_cast<float>(std::max(pairs, 1u));
    error = std::max(mean_width, mean_distance) * 0.5f;
    return true;
}

uint32_t pack_halves(float a, float b)
{
    return uint32_t(::velk::detail::float_to_half_scalar(a)) | (uint32_t(::velk::detail::float_to_half_scalar(b)) << 16);
}

/// Fills what a cluster looks like as a whole: the sphere around its own triangles, and its
/// normal and uvs by area. @p normals is empty and @p uv_bytes null when there are none.
void fill_cluster_surface(MeshLodCluster& cluster, const vector<uint32_t>& indices, const vector<float>& positions,
                          const vector<float>& normals, const uint8_t* uv_bytes, uint32_t stride, uint32_t uv_offset)
{
    const uint32_t* ids = indices.data() + cluster.first_index;
    const size_t count = size_t(cluster.triangle_count) * 3;
    vector<float> points;
    points.reserve(count * 3);
    for (size_t i = 0; i < count; ++i) {
        points.insert(points.end(), positions.data() + size_t(ids[i]) * 3, positions.data() + size_t(ids[i]) * 3 + 3);
    }
    const meshopt_Bounds bounds = meshopt_computeSphereBounds(points.data(), count, sizeof(float) * 3, nullptr, 0);
    std::memcpy(cluster.own_center, bounds.center, sizeof(float) * 3);
    cluster.own_radius = bounds.radius;

    const auto uv_of = [&](uint32_t id, int k) {
        float value;
        std::memcpy(&value, uv_bytes + size_t(id) * stride + uv_offset + k * sizeof(float), sizeof(float));
        return value;
    };
    float area = 0.f;
    float normal[3] = {};
    float uv[2] = {};
    float uv_min[2] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    float uv_max[2] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    for (size_t i = 0; i + 2 < count; i += 3) {
        const float a = triangle_area(positions, ids[i], ids[i + 1], ids[i + 2]);
        area += a;
        if (!normals.empty()) {
            for (int k = 0; k < 3; ++k) {
                normal[k] += a * (normals[size_t(ids[i]) * 3 + k] + normals[size_t(ids[i + 1]) * 3 + k]
                                  + normals[size_t(ids[i + 2]) * 3 + k]) / 3.f;
            }
        }
        if (uv_bytes) {
            for (int k = 0; k < 2; ++k) {
                for (size_t corner = 0; corner < 3; ++corner) {
                    const float value = uv_of(ids[i + corner], k);
                    uv[k] += a * value / 3.f;
                    uv_min[k] = std::min(uv_min[k], value);
                    uv_max[k] = std::max(uv_max[k], value);
                }
            }
        }
    }
    if (area <= 0.f) {
        return;
    }
    if (!normals.empty()) {
        for (int k = 0; k < 3; ++k) {
            const float unit = std::min(std::max(normal[k] / area, -1.f), 1.f);
            cluster.normal |= static_cast<uint32_t>(static_cast<uint8_t>(static_cast<int8_t>(std::lround(unit * 127.f)))) << (k * 8);
        }
    }
    if (uv_bytes) {
        cluster.uv = pack_halves(uv[0] / area, uv[1] / area);
        cluster.uv_range = pack_halves(uv_max[0] - uv_min[0], uv_max[1] - uv_min[1]);
    }
}

} // namespace

MeshLod build_mesh_lod(const BlasBuild& blas, const uint8_t* vertices, uint32_t stride, uint32_t normal_offset,
                       uint32_t uv_offset, bool merge_pieces, bool coarser_levels)
{
    MeshLod lod;
    if (blas.clusters.empty()) {
        return lod;
    }
    LodVertices lod_vertices;
    lod_vertices.positions = gather_positions(blas);
    lod_vertices.remap = position_remap(lod_vertices.positions);
    lod_vertices.stride = stride;
    lod_vertices.normal_offset = normal_offset;
    vector<float>& positions = lod_vertices.positions;
    vector<float>& normals = lod_vertices.normals;
    vector<uint32_t>& remap = lod_vertices.remap;
    vector<unsigned char>& locks = lod_vertices.locks;
    size_t vertex_count = positions.size() / 3;

    const bool has_normals = vertices && normal_offset != kLodNoAttribute;
    if (has_normals) {
        normals.resize(vertex_count * 3);
        for (size_t v = 0; v < vertex_count; ++v) {
            std::memcpy(normals.data() + v * 3, vertices + v * stride + normal_offset, sizeof(float) * 3);
        }
    }
    merge_pieces = merge_pieces && has_normals;
    if (merge_pieces) {
        lod_vertices.bytes.resize(vertex_count * stride);
        std::memcpy(lod_vertices.bytes.data(), vertices, vertex_count * stride);
    }

    // Index lists of all clusters so far, parallel to lod.clusters.
    vector<vector<uint32_t>> lists = gather_clusters(blas);
    if (merge_pieces) {
        gather_pieces(lists, remap);
    }
    vector<uint32_t> pending;
    for (size_t i = 0; i < lists.size(); ++i) {
        add_cluster(lod, lists[i], positions, 0, nullptr, 0.f);
        pending.push_back(static_cast<uint32_t>(i));
    }

    vector<uint32_t> first_seen;
    locks.resize(vertex_count);

    for (uint32_t level = 1; coarser_levels && level < kMaxLevels && pending.size() > 1; ++level) {
        first_seen.resize(vertex_count);
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
            size_t kept = 0;
            vector<uint32_t> alternate;
            if (merge_pieces && merge_piece_pairs(lod_vertices, merged, alternate, error)) {
                kept = alternate.size();
                std::copy(alternate.begin(), alternate.end(), simplified.begin());
                vertex_count = positions.size() / 3;
            } else {
                // Clamped keeps normals' error from overstating how far the surface moved.
                kept = meshopt_simplifyWithAttributes(
                    simplified.data(), merged.data(), merged.size(), positions.data(), vertex_count, sizeof(float) * 3,
                    normals.empty() ? nullptr : normals.data(), sizeof(float) * 3, normals.empty() ? nullptr : kNormalWeights,
                    normals.empty() ? 0 : 3, locks.data(), target, FLT_MAX,
                    meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute | meshopt_SimplifyErrorClamped, &error);
            }
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
        pending = std::move(next);
    }

    for (size_t first = 0; first < lod.clusters.size();) {
        MeshLodLevel level;
        level.first_cluster = static_cast<uint32_t>(first);
        level.min_error = lod.clusters[first].error;
        vector<float> spheres;
        const auto add_sphere = [&spheres](const float* center, float radius) {
            spheres.insert(spheres.end(), center, center + 3);
            spheres.push_back(radius);
        };
        size_t end = first;
        for (; end < lod.clusters.size() && lod.clusters[end].level == lod.clusters[first].level; ++end) {
            const MeshLodCluster& cluster = lod.clusters[end];
            level.min_error = std::min(level.min_error, cluster.error);
            level.max_parent_error = std::max(level.max_parent_error, cluster.parent_error);
            add_sphere(cluster.center, cluster.radius);
            if (cluster.parent_error < std::numeric_limits<float>::max()) {
                add_sphere(cluster.parent_center, cluster.parent_radius);
            }
        }
        level.cluster_count = static_cast<uint32_t>(end - first);
        const meshopt_Bounds bounds = meshopt_computeSphereBounds(spheres.data(), spheres.size() / 4, sizeof(float) * 4,
                                                                  spheres.data() + 3, sizeof(float) * 4);
        std::memcpy(level.center, bounds.center, sizeof(float) * 3);
        level.radius = bounds.radius;
        lod.levels.push_back(level);
        first = end;
    }
    // Merged vertices are only in the copy, which holds the originals too.
    const uint8_t* uv_bytes = nullptr;
    if (vertices && uv_offset != kLodNoAttribute) {
        uv_bytes = lod_vertices.grown ? lod_vertices.bytes.data() : vertices;
    }
    for (MeshLodCluster& cluster : lod.clusters) {
        fill_cluster_surface(cluster, lod.indices, positions, normals, uv_bytes, stride, uv_offset);
    }
    if (lod_vertices.grown) {
        lod.vertices = std::move(lod_vertices.bytes);
    }
    return lod;
}

} // namespace velk::impl
