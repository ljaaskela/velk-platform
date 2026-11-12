#ifndef VELK_RENDER_MESH_LOD_H
#define VELK_RENDER_MESH_LOD_H

#include <velk/vector.h>

#include <velk-render/blas.h>

#include <cstdint>

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
    uint32_t spare[2] = {};
};
static_assert(sizeof(MeshLodCluster) == kLodClusterWords * sizeof(uint32_t), "Mirrors VELK_LOD_SELECT_GLSL");

/// What holds for all clusters of a level, so that a level none of them would be chosen
/// from can be skipped without testing them.
struct MeshLodLevel
{
    uint32_t first_cluster = 0;
    uint32_t cluster_count = 0;
    float min_error = 0.f;
    float max_parent_error = 0.f;
    float center[3] = {};  ///< With radius, a sphere holding the spheres its clusters are tested with.
    float radius = 0.f;
};
static_assert(sizeof(MeshLodLevel) == kLodLevelWords * sizeof(uint32_t), "Mirrors VELK_LOD_SELECT_GLSL");

/// Coarse levels hold triangles over the mesh's own vertices, unless separate pieces merged.
struct MeshLod
{
    vector<uint8_t> vertices;  ///< The mesh's vertices and the merged ones, or empty if there are none.
    vector<uint32_t> indices;
    vector<MeshLodCluster> clusters;  ///< By level, level 0 first.
    vector<MeshLodLevel> levels;
};

/// For build_mesh_lod: a vertex has no such attribute.
inline constexpr uint32_t kLodNoAttribute = 0xFFFFFFFFu;

/// Builds the hierarchy over the clusters of @p blas, whose triangles are level 0.
/// @param vertices The vertices @p blas indexes, so that normals are kept too. Null to go by
///        positions alone.
/// @param stride Bytes per vertex.
/// @param normal_offset Bytes to a vertex's normal, three floats, or kLodNoAttribute.
/// @param merge_pieces Merge its separate pieces rather than simplifying them as a surface. See
///        IMeshPrimitive::set_merge_pieces. Needs normals.
/// @param coarser_levels False for level 0 alone.
MeshLod build_mesh_lod(const BlasBuild& blas, const uint8_t* vertices, uint32_t stride, uint32_t normal_offset,
                       bool merge_pieces, bool coarser_levels);

} // namespace velk::impl

#endif // VELK_RENDER_MESH_LOD_H
