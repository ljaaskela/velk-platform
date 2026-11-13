#ifndef VELK_RENDER_MESH_LOD_H
#define VELK_RENDER_MESH_LOD_H

#include <velk/vector.h>

#include <velk-render/blas.h>

#include <cstdint>
#include <limits>

namespace velk::impl {

/// One cluster of a mesh's level of detail hierarchy. It is drawn when its own error is
/// small enough on screen and its parent group's error is not, which picks one consistent
/// set of clusters with no cracks between them. Errors are in the mesh's units, and each is
/// measured against a sphere, so that it can be projected. A sphere always holds the spheres
/// of the clusters below it, and an error is never less than theirs.
struct MeshLodCluster
{
    uint32_t first_index = 0;     ///< Into MeshLod::indices.
    uint32_t triangle_count = 0;
    uint32_t level = 0;           ///< 0 is the full mesh.
    float center[3] = {};
    float radius = 0.f;
    float error = 0.f;
    float parent_center[3] = {};
    float parent_radius = 0.f;
    float parent_error = 0.f;     ///< Largest float when nothing replaces it.
    /// With own_radius, a sphere around the cluster's own triangles. The sphere above is its
    /// group's on coarser levels, so that the choice between levels stays consistent.
    float own_center[3] = {};
    float own_radius = 0.f;
    /// The diagonal of the largest box around one of its triangles, so that a cluster with a
    /// triangle too large for compute raster stays with the hardware.
    float largest = 0.f;
    /// Its own run in MeshLod::vertices, which only its indices name. Both 0 when the
    /// hierarchy has no vertices of its own.
    uint32_t first_vertex = 0;
    uint32_t vertex_count = 0;
};
static_assert(sizeof(MeshLodCluster) == kLodClusterWords * sizeof(uint32_t), "Mirrors VELK_LOD_SELECT_GLSL");

/// A node of the tree over a hierarchy's clusters. What it says of a child holds for all
/// clusters under that child, so that a child none of them would be chosen from is skipped
/// without testing them.
struct MeshLodNode
{
    struct Child
    {
        float center[3] = {};  ///< With radius, a sphere holding the spheres its clusters are tested with.
        float radius = 0.f;
        float min_error = std::numeric_limits<float>::max();
        float max_parent_error = 0.f;
        /// A node's index, a cluster's with kLodLeaf, or kLodNoChild.
        uint32_t ref = kLodNoChild;
        uint32_t spare = 0;
    };
    Child children[2];
};
static_assert(sizeof(MeshLodNode) == kLodNodeWords * sizeof(uint32_t), "Mirrors VELK_LOD_SELECT_GLSL");

/// A mesh's level of detail hierarchy. Built with the mesh's vertices, every cluster has a run
/// of vertices of its own, so that it needs nothing another cluster holds. Built without, all
/// clusters name the mesh's vertices, which is how a deforming mesh's follow it.
struct MeshLod
{
    vector<uint8_t> vertices;  ///< Each cluster's run, one after another, or empty.
    vector<float> uv1;         ///< Two floats for each of `vertices`, when the mesh has a second uv.
    vector<uint32_t> indices;
    vector<MeshLodCluster> clusters;  ///< By level, level 0 first.
    uint32_t level0_clusters = 0;
    /// The tree over all clusters, each level's clusters by where they are and the levels
    /// under one root, which is the first node. A child comes after its parent.
    vector<MeshLodNode> nodes;

    /// What rays walk, when asked for. `ray_nodes` starts with two for each of `nodes`: the
    /// boxes of its children with their references (kBlasUpperRef with twice a node's index,
    /// kBlasClusterRef with the word of a cluster's header, or kBlasNoChild), then the
    /// MeshLodNode itself. After those come the nodes of the trees over each cluster's
    /// triangles. `ray_words` holds, per cluster, its header (kClusterHeaderWords) and what
    /// blas_gpu_tree gives for its triangles. References count from the start of each.
    vector<BlasGpuNode> ray_nodes;
    vector<uint32_t> ray_words;
};

/// For build_mesh_lod: a vertex has no such attribute.
inline constexpr uint32_t kLodNoAttribute = 0xFFFFFFFFu;

/// Builds the hierarchy over the clusters of @p blas, whose triangles are level 0.
/// @param vertices The vertices @p blas indexes, which clusters then get runs of their own
///        of, and whose normals are kept. Null to go by positions alone and name the mesh's,
///        with no normals for rays to stop by.
/// @param uv1 Two floats for each of @p vertices, or null.
/// @param stride Bytes per vertex.
/// @param normal_offset Bytes to a vertex's normal, three floats, or kLodNoAttribute.
/// @param merge_pieces Merge its separate pieces rather than simplifying them as a surface. See
///        IMeshPrimitive::set_merge_pieces. Needs normals.
/// @param deforming The mesh's vertices move: level 0 alone, over the mesh's own vertices, and
///        nothing for rays, so that `ray_nodes` holds only the two nodes for each of `nodes`,
///        with no boxes, and `ray_words` is empty.
MeshLod build_mesh_lod(const BlasBuild& blas, const uint8_t* vertices, const float* uv1, uint32_t stride,
                       uint32_t normal_offset, bool merge_pieces, bool deforming);

} // namespace velk::impl

#endif // VELK_RENDER_MESH_LOD_H
