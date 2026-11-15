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
/// of the clusters below it, and an error is never less than theirs. Above level 0 an error
/// is never less than its sphere's width over kLodClusterWidth either, so that what is chosen
/// is never wider on screen than that many times the error limit.
struct MeshLodCluster
{
    /// The word of its first index. From the start of MeshLod::indices as built, and in the
    /// mesh-word arena once published. Its indices count from its first vertex.
    uint32_t index_word = 0;
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
    /// The word of its first vertex: its own run, which only its indices name, from the
    /// start of MeshLod::vertices as built and in the mesh-word arena once published. For a
    /// deforming mesh, the mesh's vertices, which all its clusters name.
    uint32_t vertex_word = 0;
    uint32_t vertex_count = 0;  ///< In its own run, or 0.
    /// The word of its first vertex's second uv, likewise, or kLodNoAttribute.
    uint32_t uv1_word = 0xFFFFFFFFu;
    /// Its error as built. `error` is 0 instead while the finer clusters it was made from
    /// are not loaded, so that it is drawn in their place.
    float full_error = 0.f;
    /// The word of a mark in the culling arena, once published, or kLodNoAttribute: whoever
    /// uses the cluster where its error as built is too large sets the mark, which says the
    /// group of finer clusters it was made from is wanted.
    uint32_t mark_word = 0xFFFFFFFFu;
    /// Likewise the mark of its own group, which whoever uses the cluster sets, so that the
    /// group is known to be wanted while it is loaded.
    uint32_t own_mark_word = 0xFFFFFFFFu;
    /// The word of its first vertex's colour, likewise, or kLodNoAttribute. See
    /// IMeshPrimitive::get_color_buffer.
    uint32_t color_word = 0xFFFFFFFFu;
    uint32_t spare[3] = {};
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

/// A mesh's level of detail hierarchy. Every cluster has a run of vertices of its own, so that
/// it needs nothing another cluster holds. A deforming mesh's clusters name the mesh's
/// vertices instead, so that they follow it.
struct MeshLod
{
    vector<uint8_t> vertices;  ///< Each cluster's run, one after another. Empty for a deforming mesh.
    vector<float> uv1;         ///< Two floats for each of `vertices`, when the mesh has a second uv.
    vector<uint32_t> colors;   ///< A colour for each of `vertices`, when the mesh has colours.
    vector<uint32_t> indices;
    vector<MeshLodCluster> clusters;  ///< By level, level 0 first.
    uint32_t level0_clusters = 0;
    /// The tree over all clusters, each level's clusters by where they are and the levels
    /// under one root, which is the first node. A child comes after its parent.
    vector<MeshLodNode> nodes;

    /// What rays walk. `ray_nodes` starts with two for each of `nodes`: the boxes of its
    /// children with their references (kBlasUpperRef with twice a node's index,
    /// kBlasClusterRef with a cluster's index times kClusterHeaderWords over four, or
    /// kBlasNoChild), then the MeshLodNode itself. Unless the mesh deforms, the nodes of the
    /// trees over each cluster's triangles follow, and `ray_words` holds every cluster's
    /// header (kClusterHeaderWords), in order, and then what blas_gpu_tree gives for each
    /// one's triangles. As built, everything a reference or a header names counts from the
    /// start of the vector it is in; publishing adds where that is.
    vector<BlasGpuNode> ray_nodes;
    vector<uint32_t> ray_words;

    /// Where the tree over one cluster's triangles is in `ray_nodes` and `ray_words`.
    struct Block
    {
        uint32_t node_first = 0;
        uint32_t node_count = 0;
        uint32_t word_first = 0;
        uint32_t word_count = 0;
    };
    vector<Block> blocks;  ///< By cluster. Empty counts for one without a tree.

    /// A group is the clusters that were simplified together, which share a parent. It is
    /// what loads as one: with all of a group there, or none of it, what is drawn has no
    /// cracks. A cluster is of no group (kLodNoGroup) when nothing replaces it, and was made
    /// from none at level 0. A group needs the clusters made from it, and so their groups.
    vector<uint32_t> cluster_group;      ///< By cluster: the group it is in.
    vector<uint32_t> cluster_made_from;  ///< By cluster: the group it was simplified from.
    uint32_t group_count = 0;
};

/// For MeshLod: no group.
inline constexpr uint32_t kLodNoGroup = 0xFFFFFFFFu;

/// For build_mesh_lod: a vertex has no such attribute.
inline constexpr uint32_t kLodNoAttribute = 0xFFFFFFFFu;

/// Builds the hierarchy over the clusters of @p blas, whose triangles are level 0.
/// @param vertices The vertices @p blas indexes, which clusters get runs of their own of, and
///        whose normals are kept. Null only for a deforming mesh without them.
/// @param uv1 Two floats for each of @p vertices, or null.
/// @param colors A colour (RGBA8) for each of @p vertices, or null.
/// @param stride Bytes per vertex.
/// @param normal_offset Bytes to a vertex's normal, three floats, or kLodNoAttribute.
/// @param merge_pieces Merge its separate pieces rather than simplifying them as a surface. See
///        IMeshPrimitive::set_merge_pieces. Needs normals.
/// @param deforming The mesh's vertices move: level 0 alone, over the mesh's own vertices, and
///        nothing for rays, so that `ray_nodes` holds only the two nodes for each of `nodes`,
///        with no boxes, and `ray_words` is empty.
MeshLod build_mesh_lod(const BlasBuild& blas, const uint8_t* vertices, const float* uv1, const uint32_t* colors,
                       uint32_t stride, uint32_t normal_offset, bool merge_pieces, bool deforming);

} // namespace velk::impl

#endif // VELK_RENDER_MESH_LOD_H
