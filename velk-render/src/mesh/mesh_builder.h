#ifndef VELK_RENDER_MESH_BUILDER_H
#define VELK_RENDER_MESH_BUILDER_H

#include <velk/ext/object.h>

#include <unordered_map>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Concrete IMeshBuilder.
 *
 * Owned by the IRenderContext (one per context). Allocates Mesh and
 * MeshPrimitive objects through the velk type registry.
 *
 * Procedural shape helpers (`get_cube`, `get_sphere`, `get_unit_quad`)
 * cache the underlying IMeshBuffer across calls so the GPU bytes upload
 * exactly once per (shape, subdivisions) combination. Each call still
 * returns a fresh IMesh + IMeshPrimitive so callers may set their own
 * per-instance material on the primitive.
 *
 * Exception: `get_unit_quad` returns the same singleton IMesh across
 * calls (its primitive carries no material and never will); saves
 * per-frame allocations in the 2D fallback path.
 */
class MeshBuilder
    : public ::velk::ext::Object<MeshBuilder, IMeshBuilder>
{
public:
    VELK_CLASS_UID(::velk::ClassId::MeshBuilder, "MeshBuilder");

    IMeshPrimitive::Ptr build_primitive(
        array_view<VertexAttribute> attributes,
        uint32_t vertex_stride,
        const void* vertex_data, uint32_t vertex_count,
        const uint32_t* indices, uint32_t index_count,
        MeshTopology topology,
        const aabb& bounds) override;

    IMeshBuffer::Ptr build_buffer(
        const void* vbo_data, size_t vbo_size,
        const void* ibo_data, size_t ibo_size) override;

    IMeshPrimitive::Ptr build_primitive_in_buffer(
        IMeshBuffer::Ptr buffer,
        uint32_t vertex_offset, uint32_t vertex_count,
        uint32_t index_offset,  uint32_t index_count,
        array_view<VertexAttribute> attributes,
        uint32_t vertex_stride,
        MeshTopology topology,
        const aabb& bounds,
        IMeshBuffer::Ptr uv1_buffer = nullptr,
        uint32_t uv1_offset = 0) override;

    IMesh::Ptr build(array_view<IMeshPrimitive::Ptr> primitives) override;

    IMesh::Ptr build(array_view<VertexAttribute> attributes,
                     uint32_t vertex_stride,
                     const void* vertex_data, uint32_t vertex_count,
                     const uint32_t* indices, uint32_t index_count,
                     MeshTopology topology,
                     const aabb& bounds) override;

    IMesh::Ptr build_deforming(const IMesh::Ptr& source) override;

    IMesh::Ptr get_unit_quad() override;
    IMesh::Ptr get_cube(uint32_t subdivisions = 0) override;
    IMesh::Ptr get_sphere(uint32_t subdivisions = 0) override;

private:
    /// Per-shape cached GPU geometry. A primitive's layout parameters
    /// (attributes / stride / counts / topology / bounds) are
    /// deterministic from the shape inputs, so we stash them alongside
    /// the buffer and re-use both when spinning up fresh primitives.
    struct CachedGeometry
    {
        IMeshBuffer::Ptr buffer;
        vector<VertexAttribute> attributes;
        uint32_t vertex_stride = 0;
        uint32_t vertex_count = 0;
        uint32_t index_count = 0;
        MeshTopology topology = MeshTopology::TriangleList;
        aabb bounds{};
    };

    CachedGeometry make_cube_geometry(uint32_t subdivisions);
    CachedGeometry make_sphere_geometry(uint32_t subdivisions);

    IMesh::Ptr mesh_from_geometry(const CachedGeometry& g);

    IMesh::Ptr unit_quad_;
    std::unordered_map<uint32_t, CachedGeometry> cube_cache_;
    std::unordered_map<uint32_t, CachedGeometry> sphere_cache_;
};

} // namespace velk::impl

#endif // VELK_RENDER_MESH_BUILDER_H
