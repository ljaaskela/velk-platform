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

/// Coarse levels hold only triangles over the mesh's own vertices.
struct MeshLod
{
    vector<uint32_t> indices;
    vector<MeshLodCluster> clusters;  ///< By level, level 0 first.
    vector<MeshLodLevel> levels;
};

/// Builds the hierarchy over the clusters of @p blas, whose triangles are level 0.
/// @param vertices The vertices @p blas indexes, so that normals are kept too. Null to go by
///        positions alone.
/// @param stride Bytes per vertex.
/// @param normal_offset Bytes to a vertex's normal, three floats.
MeshLod build_mesh_lod(const BlasBuild& blas, const uint8_t* vertices, uint32_t stride, uint32_t normal_offset);

} // namespace velk::impl

#endif // VELK_RENDER_MESH_LOD_H
