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
static_assert(sizeof(MeshLodCluster) == kLodClusterWords * sizeof(uint32_t), "Mirrors instance_cull_compute_src");

/// Coarse levels hold only triangles over the mesh's own vertices.
struct MeshLod
{
    vector<uint32_t> indices;
    vector<MeshLodCluster> clusters;  ///< Level 0 first.
    uint32_t level_count = 0;
};

/// Builds the hierarchy over the clusters of @p blas, whose triangles are level 0.
MeshLod build_mesh_lod(const BlasBuild& blas);

} // namespace velk::impl

#endif // VELK_RENDER_MESH_LOD_H
