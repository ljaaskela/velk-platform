#ifndef VELK_SCENE_AGGREGATE_H
#define VELK_SCENE_AGGREGATE_H

#include <velk/string_view.h>

#include <velk-render/interface/intf_mesh.h>

#include <cstdint>

namespace velk::impl {

/// What one instance of a mesh is in its partition's quad tree (kQuadNodeWords), in the mesh's
/// own space: a quad across the two longer sides of its bounds, through their middle, facing
/// along the third, in its materials' colour.
struct MeshQuad
{
    float center[3] = {};
    float u[3] = {};  ///< Half axis along the longest side.
    float v[3] = {};  ///< Half axis along the other; the normal is u cross v.
    uint32_t color = 0;  ///< RGBA8, red in the low byte. 0 for a mesh with no quad.
};
/// Words of a MeshQuad in the GPU table of a visual's meshes.
inline constexpr uint32_t kMeshQuadWords = 10;
static_assert(sizeof(MeshQuad) == kMeshQuadWords * sizeof(uint32_t), "Mirrors quad_tree_build_src");

/// @p mesh's quad: from its bounds, coloured by its primitives' materials' average albedo, by
/// the surface each covers. False when it has no bounds.
bool mesh_quad(const IMesh& mesh, MeshQuad& out);

/// A quad across the two longer sides of world bounds @p lo to @p hi, facing along the third,
/// in @p color (RGBA8): a plain object's, in the world.
MeshQuad bounds_quad(const float lo[3], const float hi[3], uint32_t color);

/// The area-weighted colour (RGBA8) of @p mesh's materials, as mesh_quad gives it.
uint32_t mesh_color(const IMesh& mesh);

/// Words per job of the quad tree build: the word of the partition's list of members (instance
/// indices in its visual's raster region, kCullNoSlot for none), its length, the word of its
/// tree, its leaves, the word of its visual's table of mesh quads, the vec4 index of its
/// visual's resting matrices (three rows an instance) or kCullNoSlot to read the instances'
/// own, the element index of its visual's raster region in the instance arena, and its
/// visual's mesh count. A plain partition's job has kCullNoSlot for that element index, and
/// the word of its members' own quads in the world (MeshQuad each, in member order) for the
/// table.
inline constexpr uint32_t kQuadJobWords = 8;

/// Builds partitions' quad trees, a work group per job (kQuadJobWords, from the culling word
/// in the push constant): the leaves from the members' meshes' quads placed where they are, then
/// each level above from the one below, each node one quad with both its children's areas.
extern const string_view quad_tree_build_src;

} // namespace velk::impl

#endif // VELK_SCENE_AGGREGATE_H
