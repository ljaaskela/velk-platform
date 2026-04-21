#include "mesh.h"

#include <velk/api/velk.h>

#include <cstring>

namespace velk::impl {

void MeshBuffer::set_bytes(const void* data, size_t size)
{
    bytes_.resize(size);
    if (size > 0 && data) {
        std::memcpy(bytes_.data(), data, size);
    }
    dirty_ = true;
}

IMesh::Ptr MeshBuilder::build(array_view<VertexAttribute> attributes,
                              uint32_t vertex_stride,
                              const void* vertex_data, uint32_t vertex_count,
                              const uint32_t* indices, uint32_t index_count,
                              MeshTopology topology,
                              const aabb& bounds)
{
    auto mesh_intf = ::velk::instance().create<IMesh>(::velk::ClassId::Mesh);
    auto* mesh = dynamic_cast<Mesh*>(mesh_intf.get());
    if (!mesh) return nullptr;

    mesh->init(attributes, vertex_stride,
               vertex_data, vertex_count,
               indices, index_count,
               topology, bounds);
    return mesh_intf;
}

IMesh::Ptr MeshBuilder::get_unit_quad()
{
    if (unit_quad_) {
        return unit_quad_;
    }

    // 4 vec2 vertices arranged for TriangleStrip:
    // strip order (0,1,2)(1,2,3) makes triangles (0,0)-(1,0)-(0,1) and
    // (1,0)-(0,1)-(1,1). No IBO; drawn with vkCmdDraw(vertex_count=4).
    // VS runs exactly once per unique vertex.
    static const float verts[] = {
        0.f, 0.f,
        1.f, 0.f,
        0.f, 1.f,
        1.f, 1.f,
    };
    static const VertexAttribute attrs[] = {
        { VertexAttributeSemantic::Position, VertexAttributeFormat::Float2, 0 },
    };

    aabb bounds{};
    bounds.position = { 0.f, 0.f, 0.f };
    bounds.extent = { 1.f, 1.f, 0.f };

    unit_quad_ = build({ attrs, 1 },
                       /*vertex_stride*/ sizeof(float) * 2,
                       verts, /*vertex_count*/ 4,
                       /*indices*/ nullptr, /*index_count*/ 0,
                       MeshTopology::TriangleStrip,
                       bounds);
    return unit_quad_;
}

Mesh::Mesh() = default;

void Mesh::init(array_view<VertexAttribute> attributes,
                uint32_t vertex_stride,
                const void* vertex_data, uint32_t vertex_count,
                const uint32_t* indices, uint32_t index_count,
                MeshTopology topology,
                const aabb& bounds)
{
    attributes_.clear();
    for (auto& a : attributes) {
        attributes_.push_back(a);
    }
    vertex_stride_ = vertex_stride;
    vertex_count_ = vertex_count;
    index_count_ = index_count;
    topology_ = topology;
    bounds_ = bounds;

    if (!vbo_) {
        vbo_ = ::velk::instance().create<IBuffer>(::velk::ClassId::MeshBuffer);
    }
    if (auto* vb = dynamic_cast<MeshBuffer*>(vbo_.get())) {
        vb->set_bytes(vertex_data, size_t(vertex_count) * vertex_stride);
    }

    // IBO is optional: indexed draws when indices != null, plain
    // vkCmdDraw when null (e.g. the unit quad uses a 4-vertex
    // TriangleStrip with no IBO).
    if (indices && index_count > 0) {
        if (!ibo_) {
            ibo_ = ::velk::instance().create<IBuffer>(::velk::ClassId::MeshBuffer);
        }
        if (auto* ib = dynamic_cast<MeshBuffer*>(ibo_.get())) {
            ib->mark_as_index_buffer();
            ib->set_bytes(indices, size_t(index_count) * sizeof(uint32_t));
        }
    } else {
        ibo_.reset();
    }
}

} // namespace velk::impl
