#ifndef VELK_RENDER_MESH_H
#define VELK_RENDER_MESH_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/// IBuffer backed by an owned byte vector. Used as the VBO/IBO storage
/// inside a Mesh; the Mesh creates two of these internally.
class MeshBuffer
    : public ::velk::ext::GpuResource<MeshBuffer, IBuffer>
{
public:
    VELK_CLASS_UID(::velk::ClassId::MeshBuffer, "MeshBuffer");

    GpuResourceType get_type() const override { return GpuResourceType::Buffer; }

    /// Replaces the byte contents and marks dirty so the renderer re-uploads
    /// on the next frame. Resizes if needed.
    void set_bytes(const void* data, size_t size);

    /// Marks this buffer as an index buffer so the backend allocates it
    /// with INDEX_BUFFER usage. Must be called before the first upload.
    void mark_as_index_buffer() { is_index_ = true; }

    size_t get_data_size() const override { return bytes_.size(); }
    const uint8_t* get_data() const override
    {
        return bytes_.empty() ? nullptr : bytes_.data();
    }
    bool is_dirty() const override { return dirty_; }
    void clear_dirty() override { dirty_ = false; }
    uint64_t get_gpu_address() const override { return gpu_addr_; }
    void set_gpu_address(uint64_t addr) override { gpu_addr_ = addr; }
    bool is_index_buffer() const override { return is_index_; }

private:
    ::velk::vector<uint8_t> bytes_;
    uint64_t gpu_addr_ = 0;
    bool dirty_ = false;
    bool is_index_ = false;
};

/// Concrete IMesh holding interleaved vertex bytes + uint32 indices,
/// plus the attribute layout that describes the vertex packing.
class Mesh
    : public ::velk::ext::Object<Mesh, IMesh>
{
public:
    VELK_CLASS_UID(::velk::ClassId::Mesh, "Mesh");

    Mesh();

    /// Internal: populates the mesh's storage. Called by MeshBuilder;
    /// not part of the IMesh interface so the mesh stays immutable from
    /// the consumer's perspective.
    void init(array_view<VertexAttribute> attributes,
              uint32_t vertex_stride,
              const void* vertex_data, uint32_t vertex_count,
              const uint32_t* indices, uint32_t index_count,
              MeshTopology topology,
              const aabb& bounds);

    array_view<VertexAttribute> get_attributes() const override
    {
        return {attributes_.data(), attributes_.size()};
    }
    uint32_t get_vertex_stride() const override { return vertex_stride_; }
    uint32_t get_vertex_count() const override { return vertex_count_; }
    uint32_t get_index_count() const override { return index_count_; }
    MeshTopology get_topology() const override { return topology_; }
    aabb get_bounds() const override { return bounds_; }
    IBuffer::Ptr get_vbo() const override { return vbo_; }
    IBuffer::Ptr get_ibo() const override { return ibo_; }

private:
    ::velk::vector<VertexAttribute> attributes_;
    uint32_t vertex_stride_ = 0;
    uint32_t vertex_count_ = 0;
    uint32_t index_count_ = 0;
    MeshTopology topology_ = MeshTopology::TriangleList;
    aabb bounds_{};
    IBuffer::Ptr vbo_;
    IBuffer::Ptr ibo_;
};

/// Concrete IMeshBuilder. Allocates Meshes through the type registry
/// and populates them via Mesh::init. Caches the engine's unit-quad
/// mesh on the builder instance so its lifetime is owned by whoever
/// owns the builder (i.e. the IRenderContext).
class MeshBuilder
    : public ::velk::ext::Object<MeshBuilder, IMeshBuilder>
{
public:
    VELK_CLASS_UID(::velk::ClassId::MeshBuilder, "MeshBuilder");

    IMesh::Ptr build(array_view<VertexAttribute> attributes,
                     uint32_t vertex_stride,
                     const void* vertex_data, uint32_t vertex_count,
                     const uint32_t* indices, uint32_t index_count,
                     MeshTopology topology,
                     const aabb& bounds) override;

    IMesh::Ptr get_unit_quad() override;

private:
    IMesh::Ptr unit_quad_;
};

} // namespace velk::impl

#endif // VELK_RENDER_MESH_H
