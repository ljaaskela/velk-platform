#include "mesh/mesh_lod.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <velk/api/velk.h>
#include <velk-render/gpu_data.h>

#include "meshoptimizer.h"

namespace velk::impl {

namespace {

/// Clusters simplified together. Partitions come out up to a third larger.
constexpr size_t kGroupClusters = 16;
/// A group that keeps more than this share of its triangles is not a level: its clusters go
/// into the next level's groups instead.
constexpr float kStopAboveKept = 0.85f;
constexpr uint32_t kMaxLevels = 32;
constexpr uint32_t kNoGroup = 0xFFFFFFFFu;
/// A separate piece of at most this many triangles is small: it is kept in one cluster, and
/// becomes a quad rather than being simplified as a surface, which would thin it to nothing.
constexpr uint32_t kSmallPieceTriangles = 64;
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

/// Level 0 as index lists over the mesh's vertices, a list per cluster, and in @p roots the
/// node of @p blas whose subtree holds each list's triangles.
vector<vector<uint32_t>> gather_clusters(const BlasBuild& blas, vector<uint32_t>& roots)
{
    vector<BlasBuild::Cluster> sorted;
    sorted.reserve(blas.clusters.size());
    for (const auto& cluster : blas.clusters) {
        sorted.push_back(cluster);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const BlasBuild::Cluster& a, const BlasBuild::Cluster& b) { return a.position_word < b.position_word; });
    roots.clear();
    for (const auto& cluster : sorted) {
        roots.push_back(cluster.root_node);
    }

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
    cluster.index_word = static_cast<uint32_t>(lod.indices.size());
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
    lod.cluster_group.push_back(kLodNoGroup);
    lod.cluster_made_from.push_back(kLodNoGroup);
}

/// Clusters can split a separate piece, which would lock its vertices as shared between groups
/// and keep it from becoming a quad, so each small piece moves to one cluster. A larger piece, a
/// surface, stays split.
void gather_pieces(vector<vector<uint32_t>>& lists, const vector<uint32_t>& remap)
{
    // Over the points these lists use only.
    vector<uint32_t> sorted;
    for (const auto& list : lists) {
        for (uint32_t id : list) {
            sorted.push_back(remap[id]);
        }
    }
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    const auto slot = [&sorted, &remap](uint32_t id) {
        return static_cast<uint32_t>(std::lower_bound(sorted.begin(), sorted.end(), remap[id]) - sorted.begin());
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
    for (const auto& list : lists) {
        for (size_t i = 0; i + 2 < list.size(); i += 3) {
            const uint32_t a = find(slot(list[i]));
            parent[find(slot(list[i + 1]))] = a;
            parent[find(slot(list[i + 2]))] = a;
        }
    }
    vector<uint32_t> triangles;
    triangles.resize(sorted.size(), 0u);
    for (const auto& list : lists) {
        for (size_t i = 0; i + 2 < list.size(); i += 3) {
            ++triangles[find(slot(list[i]))];
        }
    }
    vector<uint32_t> owner;
    owner.resize(sorted.size(), kNoGroup);
    vector<vector<uint32_t>> gathered;
    gathered.resize(lists.size());
    for (size_t c = 0; c < lists.size(); ++c) {
        const auto& list = lists[c];
        for (size_t i = 0; i + 2 < list.size(); i += 3) {
            const uint32_t root = find(slot(list[i]));
            if (owner[root] == kNoGroup) {
                owner[root] = static_cast<uint32_t>(c);
            }
            const size_t target = triangles[root] <= kSmallPieceTriangles ? owner[root] : c;
            gathered[target].push_back(list[i]);
            gathered[target].push_back(list[i + 1]);
            gathered[target].push_back(list[i + 2]);
        }
    }
    lists = std::move(gathered);
}

/// The mesh's vertices as a level sees them, which grows as pieces become quads.
struct LodVertices
{
    vector<float> positions;
    vector<float> normals;
    vector<uint32_t> remap;
    vector<unsigned char> locks;
    vector<uint8_t> bytes;
    /// For each vertex, the mesh's vertex it was made from, once any has been added.
    vector<uint32_t> origin;
    uint32_t stride = 0;
    uint32_t normal_offset = 0;
};

/// Numbers the separate pieces of @p list, in the order they first appear: @p piece_of gets each
/// triangle's. Returns how many there are.
uint32_t number_pieces(const LodVertices& v, const vector<uint32_t>& list, vector<uint32_t>& piece_of)
{
    vector<uint32_t> sorted;
    sorted.reserve(list.size());
    for (uint32_t id : list) {
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
    for (size_t i = 0; i + 2 < list.size(); i += 3) {
        const uint32_t a = find(slot(list[i]));
        parent[find(slot(list[i + 1]))] = a;
        parent[find(slot(list[i + 2]))] = a;
    }
    constexpr uint32_t kNone = 0xFFFFFFFFu;
    vector<uint32_t> number;
    number.resize(sorted.size(), kNone);
    uint32_t pieces = 0;
    piece_of.clear();
    for (size_t i = 0; i + 2 < list.size(); i += 3) {
        const uint32_t root = find(slot(list[i]));
        if (number[root] == kNone) {
            number[root] = pieces++;
        }
        piece_of.push_back(number[root]);
    }
    return pieces;
}

/// Spreads the bits of a 10-bit value three apart, for a Morton key.
uint32_t spread_bits(uint32_t x)
{
    x &= 0x3FFu;
    x = (x | (x << 16)) & 0x030000FFu;
    x = (x | (x << 8)) & 0x0300F00Fu;
    x = (x | (x << 4)) & 0x030C30C3u;
    x = (x | (x << 2)) & 0x09249249u;
    return x;
}

/// One level of a group: its surfaces simplified, and its small separate pieces (each at most
/// kSmallPieceTriangles, sharing nothing with another group) turned into quads, the smallest
/// first, until the group has at most half its triangles where it can. A surface simplifier
/// would thin such pieces to nothing at almost no reported error, so they never go to it. Each
/// quad stands for neighbouring pieces: across their bounds, with their area, facing as they do
/// together, with the attributes of one of their vertices. With @p whole, everything becomes
/// one quad, for a cluster nothing else reduces. @p kept gets the level, over new vertices for
/// the quads.
void simplify_group(LodVertices& v, const vector<uint32_t>& merged, bool whole, vector<uint32_t>& kept)
{
    kept.clear();
    vector<uint32_t> piece_of;
    const uint32_t pieces = number_pieces(v, merged, piece_of);
    const size_t triangle_count = merged.size() / 3;

    // Each piece's triangles, area, middle, facing, bounds, and whether a vertex is locked.
    struct Piece
    {
        uint32_t triangles = 0;
        float area = 0.f;
        float center[3] = {0.f, 0.f, 0.f};
        float facing[3] = {0.f, 0.f, 0.f};  ///< Its triangles' normals by area, as wound.
        float shading[3] = {0.f, 0.f, 0.f}; ///< Its vertices' normals by area.
        float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        uint32_t first_vertex = 0;
        bool locked = false;
    };
    vector<Piece> info;
    info.resize(pieces);
    const bool has_normals = v.normals.size() == v.positions.size();
    for (size_t t = 0; t < triangle_count; ++t) {
        Piece& piece = info[piece_of[t]];
        const uint32_t* ids = merged.data() + t * 3;
        if (piece.triangles == 0) {
            piece.first_vertex = ids[0];
        }
        ++piece.triangles;
        const float* a = v.positions.data() + size_t(ids[0]) * 3;
        const float* b = v.positions.data() + size_t(ids[1]) * 3;
        const float* c = v.positions.data() + size_t(ids[2]) * 3;
        const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        const float area = std::max(0.5f * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), 1e-12f);
        piece.area += area;
        for (int k = 0; k < 3; ++k) {
            piece.center[k] += (a[k] + b[k] + c[k]) / 3.f * area;
            piece.facing[k] += 0.5f * n[k];
            if (has_normals) {
                piece.shading[k] += v.normals[size_t(ids[0]) * 3 + k] * area;
            }
        }
        for (int corner = 0; corner < 3; ++corner) {
            const uint32_t id = ids[corner];
            piece.locked = piece.locked || v.locks[v.remap[id]] != 0;
            for (int k = 0; k < 3; ++k) {
                piece.lo[k] = std::min(piece.lo[k], v.positions[size_t(id) * 3 + k]);
                piece.hi[k] = std::max(piece.hi[k], v.positions[size_t(id) * 3 + k]);
            }
        }
    }
    for (Piece& piece : info) {
        for (int k = 0; k < 3; ++k) {
            piece.center[k] /= piece.area;
        }
    }
    const auto is_small = [whole](const Piece& piece) {
        return whole || (!piece.locked && piece.triangles <= kSmallPieceTriangles);
    };

    // The surfaces, simplified to half.
    vector<uint32_t> surface;
    for (size_t t = 0; t < triangle_count; ++t) {
        if (!is_small(info[piece_of[t]])) {
            surface.insert(surface.end(), merged.begin() + t * 3, merged.begin() + t * 3 + 3);
        }
    }
    if (!surface.empty()) {
        vector<uint32_t> simplified;
        simplified.resize(surface.size());
        // Clamped keeps normals' error from overstating how far the surface moved.
        const auto simplify = [&](unsigned int extra) {
            return meshopt_simplifyWithAttributes(
                simplified.data(), surface.data(), surface.size(), v.positions.data(), v.positions.size() / 3, sizeof(float) * 3,
                has_normals ? v.normals.data() : nullptr, sizeof(float) * 3, has_normals ? kNormalWeights : nullptr,
                has_normals ? 3 : 0, v.locks.data(), surface.size() / 6 * 3, FLT_MAX,
                meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute | meshopt_SimplifyErrorClamped | extra, nullptr);
        };
        size_t count = simplify(0u);
        // Seams (vertices split for uvs or normals) block collapses, so a surface with many keeps
        // nearly all of it. Where it does, collapses may cross them; the locks still hold.
        if (count == 0 || static_cast<float>(count) > kStopAboveKept * static_cast<float>(surface.size())) {
            const size_t before = count;
            vector<uint32_t> first;
            first.assign(simplified.begin(), simplified.begin() + before);
            count = simplify(meshopt_SimplifyPermissive);
            if (count == 0 || (before != 0 && count >= before)) {
                std::copy(first.begin(), first.end(), simplified.begin());
                count = before;
            }
        }
        if (count == 0) {
            kept = surface;
        } else {
            kept.assign(simplified.begin(), simplified.begin() + count);
        }
    }

    // The small pieces, smallest first. Each quad takes `per_quad` of them, as many as keeps
    // the quads to at most half the triangles they stand for.
    vector<uint32_t> small;
    uint32_t small_triangles = 0;
    for (uint32_t p = 0; p < pieces; ++p) {
        if (is_small(info[p])) {
            small.push_back(p);
            small_triangles += info[p].triangles;
        }
    }
    if (small.empty()) {
        return;
    }
    std::sort(small.begin(), small.end(), [&info](uint32_t a, uint32_t b) { return info[a].area < info[b].area; });
    const auto per_quad = [](size_t count, uint32_t triangles) {
        return std::max<size_t>(1u, (count * 4u + triangles - 1u) / std::max(triangles, 1u));
    };
    const auto quad_triangles = [&](size_t count, uint32_t triangles) {
        return count == 0 ? 0u : static_cast<uint32_t>((count + per_quad(count, triangles) - 1u) / per_quad(count, triangles) * 2u);
    };
    // The largest stay as they are while the level still fits in half the group.
    const auto target = static_cast<uint32_t>(std::max<size_t>(triangle_count / 2, 2u));
    size_t converted = small.size();
    uint32_t converted_triangles = small_triangles;
    uint32_t copied_triangles = 0;
    const auto kept_triangles = static_cast<uint32_t>(kept.size() / 3);
    while (!whole && converted > 0) {
        const Piece& largest = info[small[converted - 1]];
        const uint32_t rest = converted_triangles - largest.triangles;
        if (kept_triangles + copied_triangles + largest.triangles + quad_triangles(converted - 1, rest) > target) {
            break;
        }
        copied_triangles += largest.triangles;
        converted_triangles = rest;
        --converted;
    }
    vector<uint8_t> stays;
    stays.resize(pieces, 0u);
    for (size_t i = converted; i < small.size(); ++i) {
        stays[small[i]] = 1u;
    }
    for (size_t t = 0; t < triangle_count; ++t) {
        if (stays[piece_of[t]] != 0u) {
            kept.insert(kept.end(), merged.begin() + t * 3, merged.begin() + t * 3 + 3);
        }
    }
    if (converted == 0) {
        return;
    }

    // Neighbours together: the converted pieces in Morton order of their middles.
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t i = 0; i < converted; ++i) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], info[small[i]].center[k]);
            hi[k] = std::max(hi[k], info[small[i]].center[k]);
        }
    }
    vector<std::pair<uint32_t, uint32_t>> order;
    for (size_t i = 0; i < converted; ++i) {
        uint32_t key = 0;
        for (int k = 0; k < 3; ++k) {
            const float span = hi[k] - lo[k];
            const float at = span > 0.f ? (info[small[i]].center[k] - lo[k]) / span : 0.f;
            key |= spread_bits(static_cast<uint32_t>(at * 1023.f)) << k;
        }
        order.push_back({key, small[i]});
    }
    std::sort(order.begin(), order.end());

    const size_t step = whole ? order.size() : per_quad(converted, converted_triangles);
    for (size_t first = 0; first < order.size(); first += step) {
        const size_t last = std::min(order.size(), first + step);
        float area = 0.f;
        float center[3] = {0.f, 0.f, 0.f};
        float facing[3] = {0.f, 0.f, 0.f};
        float shading[3] = {0.f, 0.f, 0.f};
        float box_lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float box_hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (size_t i = first; i < last; ++i) {
            const Piece& piece = info[order[i].second];
            area += piece.area;
            for (int k = 0; k < 3; ++k) {
                center[k] += piece.center[k] * piece.area;
                facing[k] += piece.facing[k];
                shading[k] += piece.shading[k];
                box_lo[k] = std::min(box_lo[k], piece.lo[k]);
                box_hi[k] = std::max(box_hi[k], piece.hi[k]);
            }
        }
        for (float& c : center) {
            c /= area;
        }
        // Facing as the pieces are wound together, or as the first is where they cancel out.
        const auto normalize = [](float* x) {
            const float length = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
            if (length <= 1e-12f) {
                return false;
            }
            for (int k = 0; k < 3; ++k) {
                x[k] /= length;
            }
            return true;
        };
        float n[3] = {facing[0], facing[1], facing[2]};
        if (!normalize(n)) {
            const Piece& piece = info[order[first].second];
            std::memcpy(n, piece.facing, sizeof(n));
            if (!normalize(n)) {
                n[0] = 0.f;
                n[1] = -1.f;
                n[2] = 0.f;
            }
        }
        // Along the bounds' longest side, as the plane holds it, and across.
        int longest = 0;
        for (int k = 1; k < 3; ++k) {
            longest = box_hi[k] - box_lo[k] > box_hi[longest] - box_lo[longest] ? k : longest;
        }
        float t[3] = {0.f, 0.f, 0.f};
        for (int attempt = 0; attempt < 3; ++attempt) {
            float axis[3] = {0.f, 0.f, 0.f};
            axis[(longest + attempt) % 3] = 1.f;
            const float d = axis[0] * n[0] + axis[1] * n[1] + axis[2] * n[2];
            for (int k = 0; k < 3; ++k) {
                t[k] = axis[k] - d * n[k];
            }
            if (normalize(t)) {
                break;
            }
        }
        const float b[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};
        float along = 0.f;
        float across = 0.f;
        for (int corner = 0; corner < 8; ++corner) {
            float d[3];
            for (int k = 0; k < 3; ++k) {
                d[k] = ((corner >> k) & 1 ? box_hi[k] : box_lo[k]) - center[k];
            }
            along = std::max(along, std::abs(d[0] * t[0] + d[1] * t[1] + d[2] * t[2]));
            across = std::max(across, std::abs(d[0] * b[0] + d[1] * b[1] + d[2] * b[2]));
        }
        // Shaped as their bounds in the plane, with their area: half sides hu and hv.
        const float ratio = std::min(std::max(along / std::max(across, 1e-6f), 1.f / 16.f), 16.f);
        const float quarter = area * 0.25f;
        const float hu = std::sqrt(quarter * ratio);
        const float hv = quarter / std::max(hu, 1e-12f);

        float normal[3] = {shading[0], shading[1], shading[2]};
        if (!normalize(normal)) {
            std::memcpy(normal, n, sizeof(normal));
        }
        // How much of the pieces faces along it, either way: the mean of |cos| to it is 1 for
        // pieces all in one plane and a half for pieces facing every way. Carried as the length
        // of its vertices' normal, which marks it as standing for them (kLodStandInShortest).
        if (has_normals && area > 0.f) {
            float along_normal = 0.f;
            for (size_t i = first; i < last; ++i) {
                const float* s = info[order[i].second].shading;
                along_normal += std::abs(s[0] * normal[0] + s[1] * normal[1] + s[2] * normal[2]);
            }
            const float flatness = std::min(std::max(2.f * along_normal / area - 1.f, 0.f), 1.f);
            const float length = kLodStandInShortest + (kLodStandInLongest - kLodStandInShortest) * flatness;
            for (float& x : normal) {
                x *= length;
            }
        }
        const uint32_t source = info[order[first].second].first_vertex;
        const auto first_new = static_cast<uint32_t>(v.positions.size() / 3);
        const float signs[4][2] = {{-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f}};
        for (const auto& s : signs) {
            float p[3];
            for (int k = 0; k < 3; ++k) {
                p[k] = center[k] + t[k] * hu * s[0] + b[k] * hv * s[1];
            }
            const auto id = static_cast<uint32_t>(v.positions.size() / 3);
            v.positions.insert(v.positions.end(), p, p + 3);
            if (has_normals) {
                v.normals.insert(v.normals.end(), normal, normal + 3);
            }
            v.remap.push_back(id);
            v.locks.push_back(0);
            if (v.origin.empty()) {
                v.origin.resize(id);
                for (uint32_t i = 0; i < id; ++i) {
                    v.origin[i] = i;
                }
            }
            v.origin.push_back(v.origin[source]);
            if (!v.bytes.empty()) {
                const size_t first_byte = v.bytes.size();
                v.bytes.resize(first_byte + v.stride);
                std::memcpy(v.bytes.data() + first_byte, v.bytes.data() + size_t(source) * v.stride, v.stride);
                std::memcpy(v.bytes.data() + first_byte, p, sizeof(p));
                if (v.normal_offset != kLodNoAttribute && has_normals) {
                    std::memcpy(v.bytes.data() + first_byte + v.normal_offset, normal, sizeof(normal));
                }
            }
        }
        // Wound as the pieces are: t, then b, turn about n.
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
            kept.push_back(first_new + i);
        }
    }
}

/// Fills the sphere around a cluster's own triangles, and the diagonal of the largest box
/// around one of them.
void fill_cluster_bounds(MeshLodCluster& cluster, const vector<uint32_t>& indices, const vector<float>& positions)
{
    const uint32_t* ids = indices.data() + cluster.index_word;
    const size_t count = size_t(cluster.triangle_count) * 3;
    vector<float> points;
    points.reserve(count * 3);
    for (size_t i = 0; i < count; ++i) {
        points.insert(points.end(), positions.data() + size_t(ids[i]) * 3, positions.data() + size_t(ids[i]) * 3 + 3);
    }
    const meshopt_Bounds bounds = meshopt_computeSphereBounds(points.data(), count, sizeof(float) * 3, nullptr, 0);
    std::memcpy(cluster.own_center, bounds.center, sizeof(float) * 3);
    cluster.own_radius = bounds.radius;

    for (size_t i = 0; i + 2 < count; i += 3) {
        float diagonal = 0.f;
        for (int k = 0; k < 3; ++k) {
            const float a = positions[size_t(ids[i]) * 3 + k];
            const float b = positions[size_t(ids[i + 1]) * 3 + k];
            const float c = positions[size_t(ids[i + 2]) * 3 + k];
            const float extent = std::max(std::max(a, b), c) - std::min(std::min(a, b), c);
            diagonal += extent * extent;
        }
        cluster.largest = std::max(cluster.largest, std::sqrt(diagonal));
    }
}

/// Builds the tree over a hierarchy's clusters.
struct LodTreeBuilder
{
    MeshLod& lod;
    vector<uint32_t> order;  ///< Cluster indices, sorted within a level as its tree splits them.

    /// The smallest sphere holding both, a little larger, so that rounding cannot leave
    /// either outside.
    static void hold(MeshLodNode::Child& out, const float* center, float radius)
    {
        float between[3];
        float distance = 0.f;
        for (int k = 0; k < 3; ++k) {
            between[k] = center[k] - out.center[k];
            distance += between[k] * between[k];
        }
        distance = std::sqrt(distance);
        if (out.radius >= distance + radius) {
            return;
        }
        if (radius >= distance + out.radius) {
            std::memcpy(out.center, center, sizeof(float) * 3);
            out.radius = radius;
            return;
        }
        const float merged = 0.5f * (distance + out.radius + radius);
        const float along = (merged - out.radius) / distance;
        for (int k = 0; k < 3; ++k) {
            out.center[k] += between[k] * along;
        }
        out.radius = merged * 1.0001f;
    }

    static void hold(MeshLodNode::Child& out, const MeshLodNode::Child& other)
    {
        if (other.ref == kLodNoChild) {
            return;
        }
        if (out.radius < 0.f) {
            const uint32_t ref = out.ref;
            out = other;
            out.ref = ref;
            return;
        }
        hold(out, other.center, other.radius);
        out.min_error = std::min(out.min_error, other.min_error);
        out.max_parent_error = std::max(out.max_parent_error, other.max_parent_error);
    }

    /// What a node says of the child @p ref, from that node's or cluster's own contents.
    MeshLodNode::Child describe(uint32_t ref) const
    {
        MeshLodNode::Child child;
        child.ref = ref;
        if ((ref & kLodLeaf) != 0u) {
            const MeshLodCluster& cluster = lod.clusters[ref & ~kLodLeaf];
            std::memcpy(child.center, cluster.center, sizeof(float) * 3);
            child.radius = cluster.radius;
            child.min_error = cluster.error;
            child.max_parent_error = cluster.parent_error;
            if (cluster.parent_error < std::numeric_limits<float>::max()) {
                hold(child, cluster.parent_center, cluster.parent_radius);
            }
            return child;
        }
        child.radius = -1.f;
        hold(child, lod.nodes[ref].children[0]);
        hold(child, lod.nodes[ref].children[1]);
        return child;
    }

    uint32_t add_node()
    {
        lod.nodes.push_back({});
        return static_cast<uint32_t>(lod.nodes.size() - 1);
    }

    /// A tree over the clusters at @p begin to @p end of `order`, halved along the axis they
    /// spread most on. Returns its reference.
    uint32_t build_clusters(size_t begin, size_t end)
    {
        if (end - begin == 1) {
            return kLodLeaf | order[begin];
        }
        float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (size_t i = begin; i < end; ++i) {
            const float* center = lod.clusters[order[i]].own_center;
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], center[k]);
                hi[k] = std::max(hi[k], center[k]);
            }
        }
        int axis = 0;
        for (int k = 1; k < 3; ++k) {
            if (hi[k] - lo[k] > hi[axis] - lo[axis]) {
                axis = k;
            }
        }
        const size_t middle = begin + (end - begin) / 2;
        std::nth_element(order.begin() + begin, order.begin() + middle, order.begin() + end, [&](uint32_t a, uint32_t b) {
            const float ca = lod.clusters[a].own_center[axis];
            const float cb = lod.clusters[b].own_center[axis];
            return ca != cb ? ca < cb : a < b;
        });
        const uint32_t node = add_node();
        const uint32_t left = build_clusters(begin, middle);
        const uint32_t right = build_clusters(middle, end);
        lod.nodes[node].children[0] = describe(left);
        lod.nodes[node].children[1] = describe(right);
        return node;
    }

    /// A tree over the levels at @p begin to @p end of @p firsts, which holds where in
    /// `order` each level starts and, last, where they all end.
    uint32_t build_levels(const vector<size_t>& firsts, size_t begin, size_t end)
    {
        if (end - begin == 1) {
            return build_clusters(firsts[begin], firsts[begin + 1]);
        }
        const size_t middle = begin + (end - begin) / 2;
        const uint32_t node = add_node();
        const uint32_t left = build_levels(firsts, begin, middle);
        const uint32_t right = build_levels(firsts, middle, end);
        lod.nodes[node].children[0] = describe(left);
        lod.nodes[node].children[1] = describe(right);
        return node;
    }

    uint32_t depth(uint32_t ref) const
    {
        if ((ref & kLodLeaf) != 0u) {
            return 0;
        }
        uint32_t deepest = 0;
        for (const auto& child : lod.nodes[ref].children) {
            if (child.ref != kLodNoChild) {
                deepest = std::max(deepest, depth(child.ref));
            }
        }
        return deepest + 1;
    }

    void build()
    {
        const size_t count = lod.clusters.size();
        order.resize(count);
        vector<size_t> firsts;
        for (size_t i = 0; i < count; ++i) {
            order[i] = static_cast<uint32_t>(i);
            if (i == 0 || lod.clusters[i].level != lod.clusters[i - 1].level) {
                firsts.push_back(i);
            }
            lod.level0_clusters += lod.clusters[i].level == 0 ? 1u : 0u;
        }
        firsts.push_back(count);

        const uint32_t root = build_levels(firsts, 0, firsts.size() - 1);
        // A single cluster still gets a node, so that the walk always starts at one.
        if ((root & kLodLeaf) != 0u) {
            lod.nodes[add_node()].children[0] = describe(root);
        }
        if (depth(0) > kLodMaxDepth) {
            VELK_LOG(E, "build_mesh_lod: the cluster tree is deeper than its walk reaches, parts will not be drawn");
        }
    }
};

/// Gives every cluster of @p lod a run of vertices of its own, copied from @p bytes, and
/// points its indices at them. @p v follows, so that what is built from it afterwards names
/// the same vertices.
void own_vertices(MeshLod& lod, LodVertices& v, const uint8_t* bytes, const float* uv1, const uint32_t* colors)
{
    const size_t source_count = v.positions.size() / 3;
    const bool has_normals = v.normals.size() == v.positions.size();
    constexpr uint32_t kNone = 0xFFFFFFFFu;
    // Where a vertex went in the cluster that last took it.
    vector<uint32_t> taken_by;
    vector<uint32_t> taken_as;
    taken_by.resize(source_count, kNone);
    taken_as.resize(source_count, 0u);

    vector<float> positions;
    vector<float> normals;
    for (size_t c = 0; c < lod.clusters.size(); ++c) {
        MeshLodCluster& cluster = lod.clusters[c];
        const auto first = static_cast<uint32_t>(positions.size() / 3);
        uint32_t* indices = lod.indices.data() + cluster.index_word;
        for (size_t i = 0; i < size_t(cluster.triangle_count) * 3; ++i) {
            const uint32_t old_id = indices[i];
            if (taken_by[old_id] != c) {
                taken_by[old_id] = static_cast<uint32_t>(c);
                taken_as[old_id] = static_cast<uint32_t>(positions.size() / 3);
                positions.insert(positions.end(), v.positions.data() + size_t(old_id) * 3, v.positions.data() + size_t(old_id) * 3 + 3);
                if (has_normals) {
                    normals.insert(normals.end(), v.normals.data() + size_t(old_id) * 3, v.normals.data() + size_t(old_id) * 3 + 3);
                }
                const size_t first_byte = lod.vertices.size();
                lod.vertices.resize(first_byte + v.stride);
                std::memcpy(lod.vertices.data() + first_byte, bytes + size_t(old_id) * v.stride, v.stride);
                const size_t from = v.origin.empty() ? old_id : v.origin[old_id];
                if (uv1) {
                    lod.uv1.insert(lod.uv1.end(), uv1 + from * 2, uv1 + from * 2 + 2);
                }
                if (colors) {
                    lod.colors.push_back(colors[from]);
                }
            }
            indices[i] = taken_as[old_id] - first;
        }
        cluster.vertex_word = first * (v.stride / 4u);
        cluster.vertex_count = static_cast<uint32_t>(positions.size() / 3) - first;
        cluster.uv1_word = uv1 ? first * 2u : kLodNoAttribute;
        cluster.color_word = colors ? first : kLodNoAttribute;
    }
    v.positions = std::move(positions);
    v.normals = std::move(normals);
}

/// Fills `ray_nodes` and `ray_words` of @p lod. With @p rays, each cluster gets a tree over its
/// triangles, and the nodes of the tree over the clusters get their children's boxes.
void build_rays(MeshLod& lod, const LodVertices& v, bool rays)
{
    struct Box
    {
        float lo[3] = {1e30f, 1e30f, 1e30f};
        float hi[3] = {1e30f, 1e30f, 1e30f};
        bool any = false;

        void add(const float* other_lo, const float* other_hi)
        {
            for (int k = 0; k < 3; ++k) {
                lo[k] = any ? std::min(lo[k], other_lo[k]) : other_lo[k];
                hi[k] = any ? std::max(hi[k], other_hi[k]) : other_hi[k];
            }
            any = true;
        }
    };

    const size_t upper = lod.nodes.size();
    lod.ray_nodes.resize(upper * 2);
    for (size_t n = 0; n < upper; ++n) {
        std::memcpy(&lod.ray_nodes[n * 2 + 1], &lod.nodes[n], sizeof(MeshLodNode));
    }
    if (!rays) {
        return;
    }

    // Positions, and normals where the mesh has them, as the tree builder reads vertices.
    const size_t vertex_count = v.positions.size() / 3;
    const bool has_normals = v.normals.size() == v.positions.size();
    const uint32_t floats_per_vertex = has_normals ? 6u : 3u;
    vector<float> vertices;
    vertices.resize(vertex_count * floats_per_vertex);
    for (size_t i = 0; i < vertex_count; ++i) {
        std::memcpy(vertices.data() + i * floats_per_vertex, v.positions.data() + i * 3, sizeof(float) * 3);
        if (has_normals) {
            std::memcpy(vertices.data() + i * floats_per_vertex + 3, v.normals.data() + i * 3, sizeof(float) * 3);
        }
    }

    using Block = MeshLod::Block;
    const size_t cluster_count = lod.clusters.size();
    const uint32_t words_per_vertex = v.stride / 4u;
    vector<Box> cluster_boxes;
    vector<uint32_t> cluster_refs;
    vector<Block>& blocks = lod.blocks;
    cluster_boxes.resize(cluster_count);
    cluster_refs.resize(cluster_count, kBlasNoChild);
    blocks.resize(cluster_count);
    lod.ray_words.resize(cluster_count * kClusterHeaderWords, 0u);
    for (size_t c = 0; c < cluster_count; ++c) {
        const MeshLodCluster& cluster = lod.clusters[c];
        // Over its own run of vertices, which its indices and so its vertex ids count from.
        const size_t first_vertex = cluster.vertex_word / words_per_vertex;
        if (first_vertex + cluster.vertex_count > vertex_count) {
            continue;
        }
        const BlasBuild blas = build_triangles_blas(vertices.data() + first_vertex * floats_per_vertex, floats_per_vertex,
                                                    cluster.vertex_count, lod.indices.data() + cluster.index_word,
                                                    cluster.triangle_count);
        if (blas.nodes.empty()) {
            continue;
        }
        const BlasGpuTree tree = blas_gpu_tree(blas);
        Block& block = blocks[c];
        block.node_first = static_cast<uint32_t>(lod.ray_nodes.size());
        block.node_count = static_cast<uint32_t>(tree.nodes.size());
        block.word_first = static_cast<uint32_t>(lod.ray_words.size());
        block.word_count = static_cast<uint32_t>(tree.words.size());
        lod.ray_nodes.insert(lod.ray_nodes.end(), tree.nodes.begin(), tree.nodes.end());
        lod.ray_words.insert(lod.ray_words.end(), tree.words.begin(), tree.words.end());

        uint32_t* header = lod.ray_words.data() + c * kClusterHeaderWords;
        std::memcpy(header, cluster.center, sizeof(float) * 3);
        std::memcpy(header + 3, &cluster.radius, sizeof(float));
        std::memcpy(header + 4, &cluster.error, sizeof(float));
        std::memcpy(header + 5, cluster.parent_center, sizeof(float) * 3);
        std::memcpy(header + 8, &cluster.parent_radius, sizeof(float));
        std::memcpy(header + 9, &cluster.parent_error, sizeof(float));
        header[10] = tree.root_ref;
        header[11] = static_cast<uint32_t>(c);
        header[12] = cluster.vertex_word;
        header[13] = block.node_first;
        header[14] = block.word_first;

        const BlasNode& root = blas.nodes[blas.root_index];
        cluster_boxes[c].add(root.aabb_min, root.aabb_max);
        cluster_refs[c] = kBlasClusterRef | static_cast<uint32_t>(c * kClusterHeaderWords / 4u);
    }

    // A child comes after its parent, so going backwards its box is known.
    vector<Box> node_boxes;
    node_boxes.resize(upper);
    for (size_t n = upper; n-- > 0;) {
        BlasGpuNode& out = lod.ray_nodes[n * 2];
        out = {};
        for (int k = 0; k < 2; ++k) {
            const uint32_t ref = lod.nodes[n].children[k].ref;
            Box box;
            uint32_t ray_ref = kBlasNoChild;
            if (ref != kLodNoChild && (ref & kLodLeaf) != 0u) {
                box = cluster_boxes[ref & ~kLodLeaf];
                ray_ref = cluster_refs[ref & ~kLodLeaf];
            } else if (ref != kLodNoChild && node_boxes[ref].any) {
                box = node_boxes[ref];
                ray_ref = kBlasUpperRef | (ref * 2u);
            }
            if (ray_ref == kBlasNoChild) {
                box = Box{};
            } else {
                node_boxes[n].add(box.lo, box.hi);
            }
            std::memcpy(k == 0 ? out.left_min : out.right_min, box.lo, sizeof(box.lo));
            std::memcpy(k == 0 ? out.left_max : out.right_max, box.hi, sizeof(box.hi));
            (k == 0 ? out.left : out.right) = ray_ref;
        }
    }

    // The walk on the GPU has no end but an empty stack, so nothing it follows may lead
    // back or outside: every node's children come after it, and every word is in range.
    // Above the clusters: a node further on, or a cluster that has a tree.
    const auto upper_leads_on = [&](uint32_t ref, uint32_t from) {
        if (ref == kBlasNoChild) {
            return true;
        }
        if ((ref & kBlasLeafRef) != 0u) {
            return false;
        }
        if ((ref & kBlasClusterRef) != 0u) {
            const uint32_t word = (ref & ~kBlasClusterRef) * 4u;
            return word % kClusterHeaderWords == 0u && word / kClusterHeaderWords < cluster_count
                && cluster_refs[word / kClusterHeaderWords] == ref;
        }
        const uint32_t node = ref & ~kBlasUpperRef;
        return (ref & kBlasUpperRef) != 0u && (node & 1u) == 0u && node > from && node + 1u < upper * 2u;
    };
    // Within a cluster's tree: a leaf whose triangles and positions are in its words, or a
    // node further on. @p from is the node asking, or none for the root.
    const auto block_leads_on = [&](const Block& block, uint32_t ref, uint32_t from) {
        if ((ref & kBlasLeafRef) != 0u) {
            const uint32_t leaf = ref & ~kBlasLeafRef;
            if (leaf + 2u >= block.word_count) {
                return false;
            }
            const uint32_t* words = lod.ray_words.data() + block.word_first;
            const uint32_t triangles = words[leaf + 1] & 0xFFFFu;
            const uint32_t corners = words[leaf + 1] >> 16;
            return words[leaf] + triangles <= block.word_count && words[leaf + 2] + corners * 4u <= block.word_count;
        }
        if ((ref & (kBlasClusterRef | kBlasUpperRef)) != 0u || ref >= block.node_count) {
            return false;
        }
        return from == kBlasNoChild ? ref == 0u : ref > from;
    };
    bool sound = upper != 0;
    for (uint32_t n = 0; sound && n < upper; ++n) {
        const BlasGpuNode& node = lod.ray_nodes[n * 2u];
        sound = upper_leads_on(node.left, n * 2u) && upper_leads_on(node.right, n * 2u);
    }
    for (size_t c = 0; sound && c < cluster_count; ++c) {
        if (cluster_refs[c] == kBlasNoChild) {
            continue;
        }
        const Block& block = blocks[c];
        sound = block_leads_on(block, lod.ray_words[c * kClusterHeaderWords + 10u], kBlasNoChild);
        for (uint32_t n = 0; sound && n < block.node_count; ++n) {
            const BlasGpuNode& node = lod.ray_nodes[block.node_first + n];
            sound = block_leads_on(block, node.left, n) && block_leads_on(block, node.right, n);
        }
    }
    if (!sound) {
        VELK_LOG(E, "build_mesh_lod: what rays would walk does not hold together, so rays will not hit this mesh");
        lod.ray_words.clear();
    }
}

} // namespace

MeshLod build_mesh_lod(const BlasBuild& blas, const uint8_t* vertices, const float* uv1, const uint32_t* colors,
                       uint32_t stride, uint32_t normal_offset, bool deforming)
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
            // Of unit length, as only a quad standing for small pieces has a shorter one
            // (kLodStandInShortest); none stays none.
            float* n = normals.data() + v * 3;
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (length > 0.f) {
                for (int k = 0; k < 3; ++k) n[k] /= length;
            }
        }
    }
    // Quads take their attributes from the vertices they stand for.
    if (vertices) {
        lod_vertices.bytes.resize(vertex_count * stride);
        std::memcpy(lod_vertices.bytes.data(), vertices, vertex_count * stride);
        if (has_normals) {
            for (size_t v = 0; v < vertex_count; ++v) {
                std::memcpy(lod_vertices.bytes.data() + v * stride + normal_offset, normals.data() + v * 3, sizeof(float) * 3);
            }
        }
    }

    // Index lists of all clusters so far, parallel to lod.clusters.
    vector<vector<uint32_t>> lists = gather_clusters(blas, lod.level0_tree_nodes);
    // Pieces gathered whole only matter to coarser levels, which a deforming mesh has none of; its
    // clusters stay the subtrees of the mesh's tree, which rays then walk.
    if (!deforming) {
        gather_pieces(lists, remap);
        lod.level0_tree_nodes.clear();
    }
    vector<uint32_t> pending;
    for (size_t i = 0; i < lists.size(); ++i) {
        add_cluster(lod, lists[i], positions, 0, nullptr, 0.f);
        pending.push_back(static_cast<uint32_t>(i));
    }

    vector<uint32_t> first_seen;
    locks.resize(vertex_count);

    // Down to one quad: one cluster of two triangles.
    const auto more_to_do = [&]() { return pending.size() > 1 || (pending.size() == 1 && lists[pending[0]].size() > 6); };
    for (uint32_t level = 1; !deforming && level < kMaxLevels && more_to_do(); ++level) {
        vertex_count = positions.size() / 3;
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

        // A group's level: its clusters' parent, made of new clusters that go on to the next.
        vector<uint32_t> next;
        const auto add_group = [&](const vector<uint32_t>& group, const vector<uint32_t>& simplified) {
            vector<float> spheres;
            for (uint32_t c : group) {
                const MeshLodCluster& child = lod.clusters[c];
                spheres.push_back(child.center[0]);
                spheres.push_back(child.center[1]);
                spheres.push_back(child.center[2]);
                spheres.push_back(child.radius);
            }
            const size_t kept = simplified.size();
            vertex_count = positions.size() / 3;

            const meshopt_Bounds merged_bounds = meshopt_computeSphereBounds(spheres.data(), group.size(), sizeof(float) * 4,
                                                                             spheres.data() + 3, sizeof(float) * 4);
            const float sphere[4] = {merged_bounds.center[0], merged_bounds.center[1], merged_bounds.center[2],
                                     merged_bounds.radius};
            // The cut takes a level by the size of what it draws: its longest triangle edge,
            // against kLodElementPixels. Never less than its children's, so sizes only grow
            // going up.
            float longest = 0.f;
            for (size_t i = 0; i + 2 < simplified.size(); i += 3) {
                for (int e = 0; e < 3; ++e) {
                    const float* a = positions.data() + size_t(simplified[i + e]) * 3;
                    const float* b = positions.data() + size_t(simplified[i + (e + 1) % 3]) * 3;
                    const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                    longest = std::max(longest, d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                }
            }
            float chosen_by = std::sqrt(longest) / kLodElementPixels * kLodMaxErrorPixels;
            for (uint32_t c : group) {
                chosen_by = std::max(chosen_by, lod.clusters[c].error);
            }
            const uint32_t group_id = lod.group_count++;
            for (uint32_t c : group) {
                MeshLodCluster& child = lod.clusters[c];
                std::memcpy(child.parent_center, sphere, sizeof(float) * 3);
                child.parent_radius = sphere[3];
                child.parent_error = chosen_by;
                lod.cluster_group[c] = group_id;
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
            vector<vector<uint32_t>> made;
            for (size_t m = 0; m < meshlet_count; ++m) {
                const meshopt_Meshlet& meshlet = meshlets[m];
                vector<uint32_t> indices;
                indices.reserve(size_t(meshlet.triangle_count) * 3);
                for (uint32_t i = 0; i < meshlet.triangle_count * 3; ++i) {
                    indices.push_back(meshlet_vertices[meshlet.vertex_offset + meshlet_triangles[meshlet.triangle_offset + i]]);
                }
                made.push_back(std::move(indices));
            }
            // Clusters split pieces as they please; small ones are kept whole again.
            gather_pieces(made, remap);
            for (auto& indices : made) {
                if (indices.empty()) {
                    continue;
                }
                next.push_back(static_cast<uint32_t>(lod.clusters.size()));
                add_cluster(lod, indices, positions, level, sphere, chosen_by);
                lod.cluster_made_from.back() = group_id;
                lists.push_back(std::move(indices));
            }
        };

        bool reduced = false;
        for (const auto& group : groups) {
            vector<uint32_t> merged;
            for (uint32_t c : group) {
                merged.insert(merged.end(), lists[c].begin(), lists[c].end());
            }
            vector<uint32_t> simplified;
            simplify_group(lod_vertices, merged, false, simplified);
            if (simplified.empty() || static_cast<float>(simplified.size()) > kStopAboveKept * static_cast<float>(merged.size())) {
                // Not here: its clusters go into the next level's groups, whose edges are elsewhere.
                next.insert(next.end(), group.begin(), group.end());
                continue;
            }
            reduced = true;
            add_group(group, simplified);
        }
        if (!reduced) {
            // Nothing reduced anywhere: each cluster becomes one quad. One that is a quad
            // already stays, and when all are, the quads are what the mesh ends in.
            next.clear();
            for (uint32_t c : pending) {
                if (lists[c].size() <= 6) {
                    next.push_back(c);
                    continue;
                }
                vector<uint32_t> simplified;
                simplify_group(lod_vertices, lists[c], true, simplified);
                add_group({c}, simplified);
                reduced = true;
            }
        }
        pending = std::move(next);
        if (!reduced) {
            break;
        }
    }

    for (MeshLodCluster& cluster : lod.clusters) {
        fill_cluster_bounds(cluster, lod.indices, positions);
    }
    LodTreeBuilder{lod}.build();
    if (vertices && !deforming) {
        own_vertices(lod, lod_vertices, lod_vertices.bytes.empty() ? vertices : lod_vertices.bytes.data(), uv1, colors);
    }
    build_rays(lod, lod_vertices, !deforming);
    return lod;
}

} // namespace velk::impl
