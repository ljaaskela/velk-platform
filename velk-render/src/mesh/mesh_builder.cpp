#include "mesh/mesh_builder.h"

#include "mesh/mesh.h"

#include <velk/api/velk.h>

namespace velk::impl {

IMeshBuffer::Ptr MeshBuilder::build_buffer(
    const void* vbo_data, size_t vbo_size,
    const void* ibo_data, size_t ibo_size)
{
    auto buffer = ::velk::instance().create<IMeshBuffer>(::velk::ClassId::MeshBuffer);
    if (!buffer) return nullptr;
    buffer->set_data(vbo_data, vbo_size, ibo_data, ibo_size);
    return buffer;
}

IMeshPrimitive::Ptr MeshBuilder::build_primitive_in_buffer(
    IMeshBuffer::Ptr buffer,
    uint32_t vertex_offset, uint32_t vertex_count,
    uint32_t index_offset,  uint32_t index_count,
    array_view<VertexAttribute> attributes,
    uint32_t vertex_stride,
    MeshTopology topology,
    const aabb& bounds,
    IMeshBuffer::Ptr uv1_buffer,
    uint32_t uv1_offset)
{
    if (!buffer) return nullptr;

    auto prim_intf = ::velk::instance().create<IMeshPrimitive>(::velk::ClassId::MeshPrimitive);
    auto* prim = interface_cast<IMeshPrimitiveInternal>(prim_intf.get());
    if (!prim) return nullptr;

    prim->init(buffer,
               vertex_offset, vertex_count,
               index_offset, index_count,
               attributes, vertex_stride,
               topology, bounds,
               uv1_buffer, uv1_offset);
    return prim_intf;
}

IMeshPrimitive::Ptr MeshBuilder::build_primitive(
    array_view<VertexAttribute> attributes,
    uint32_t vertex_stride,
    const void* vertex_data, uint32_t vertex_count,
    const uint32_t* indices, uint32_t index_count,
    MeshTopology topology,
    const aabb& bounds)
{
    const size_t vbo_size = size_t(vertex_count) * vertex_stride;
    const size_t ibo_size = indices ? size_t(index_count) * sizeof(uint32_t) : 0;

    auto buffer = build_buffer(vertex_data, vbo_size, indices, ibo_size);
    if (!buffer) return nullptr;

    return build_primitive_in_buffer(buffer,
                                     /*vertex_offset*/ 0, vertex_count,
                                     /*index_offset*/ 0, index_count,
                                     attributes, vertex_stride,
                                     topology, bounds);
}

IMesh::Ptr MeshBuilder::build(array_view<IMeshPrimitive::Ptr> primitives)
{
    auto mesh_intf = ::velk::instance().create<IMesh>(::velk::ClassId::Mesh);
    auto* mesh = interface_cast<IMeshInternal>(mesh_intf.get());
    if (!mesh) return nullptr;

    mesh->init(primitives, aabb{}, /*has_explicit_bounds*/ false);
    return mesh_intf;
}

IMesh::Ptr MeshBuilder::build(array_view<VertexAttribute> attributes,
                              uint32_t vertex_stride,
                              const void* vertex_data, uint32_t vertex_count,
                              const uint32_t* indices, uint32_t index_count,
                              MeshTopology topology,
                              const aabb& bounds)
{
    auto prim = build_primitive(attributes, vertex_stride,
                                 vertex_data, vertex_count,
                                 indices, index_count,
                                 topology, bounds);
    if (!prim) return nullptr;

    auto mesh_intf = ::velk::instance().create<IMesh>(::velk::ClassId::Mesh);
    auto* mesh = interface_cast<IMeshInternal>(mesh_intf.get());
    if (!mesh) return nullptr;

    IMeshPrimitive::Ptr list[] = { prim };
    mesh->init({list, 1}, bounds, /*has_explicit_bounds*/ true);
    return mesh_intf;
}

IMesh::Ptr MeshBuilder::mesh_from_geometry(const CachedGeometry& g)
{
    // Fresh primitive referencing the shared buffer. Material defaults
    // to empty ObjectRef; callers set their own.
    auto prim_intf = ::velk::instance().create<IMeshPrimitive>(::velk::ClassId::MeshPrimitive);
    auto* prim = interface_cast<IMeshPrimitiveInternal>(prim_intf.get());
    if (!prim) return nullptr;

    prim->init(g.buffer,
               /*vertex_offset*/ 0, g.vertex_count,
               /*index_offset*/ 0, g.index_count,
               {g.attributes.data(), g.attributes.size()},
               g.vertex_stride,
               g.topology,
               g.bounds,
               /*uv1_buffer*/ nullptr, /*uv1_offset*/ 0);

    auto mesh_intf = ::velk::instance().create<IMesh>(::velk::ClassId::Mesh);
    auto* mesh = interface_cast<IMeshInternal>(mesh_intf.get());
    if (!mesh) return nullptr;

    IMeshPrimitive::Ptr list[] = { prim_intf };
    mesh->init({list, 1}, g.bounds, /*has_explicit_bounds*/ true);
    return mesh_intf;
}

IMesh::Ptr MeshBuilder::get_unit_quad()
{
    if (unit_quad_) {
        return unit_quad_;
    }

    // 4-vertex TriangleStrip. Strip order (0,1,2)(1,2,3) produces the
    // two triangles (0,0,0)-(1,0,0)-(0,1,0) and (1,0,0)-(0,1,0)-(1,1,0)
    // — the unit quad in the XY plane at z = 0, facing +Z. Every 2D
    // visual (rect, text, image, texture, env) and any mesh-style
    // visual share this same vertex layout (VelkVertex3D: vec3 pos +
    // vec3 normal + vec2 uv, 32 B scalar). No IBO; drawn with
    // vkCmdDraw(vertex_count=4).
    struct Vertex { float pos[3]; float normal[3]; float uv[2]; };
    static const Vertex verts[] = {
        { {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f} },
        { {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f} },
        { {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f} },
        { {1.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 1.f} },
    };
    static_assert(sizeof(Vertex) == 32, "Vertex must match scalar-layout VelkVertex3D");

    static const VertexAttribute attrs[] = {
        { VertexAttributeSemantic::Position,  VertexAttributeFormat::Float3, 0 },
        { VertexAttributeSemantic::Normal,    VertexAttributeFormat::Float3, 12 },
        { VertexAttributeSemantic::TexCoord0, VertexAttributeFormat::Float2, 24 },
    };

    aabb bounds{};
    bounds.position = { 0.f, 0.f, 0.f };
    bounds.extent = { 1.f, 1.f, 0.f };

    unit_quad_ = build({ attrs, 3 },
                       /*vertex_stride*/ sizeof(Vertex),
                       verts, /*vertex_count*/ 4,
                       /*indices*/ nullptr, /*index_count*/ 0,
                       MeshTopology::TriangleStrip,
                       bounds);
    return unit_quad_;
}

IMesh::Ptr MeshBuilder::get_cube(uint32_t subdivisions)
{
    auto it = cube_cache_.find(subdivisions);
    if (it == cube_cache_.end()) {
        it = cube_cache_.emplace(subdivisions, make_cube_geometry(subdivisions)).first;
    }
    return mesh_from_geometry(it->second);
}

IMesh::Ptr MeshBuilder::get_sphere(uint32_t subdivisions)
{
    auto it = sphere_cache_.find(subdivisions);
    if (it == sphere_cache_.end()) {
        it = sphere_cache_.emplace(subdivisions, make_sphere_geometry(subdivisions)).first;
    }
    return mesh_from_geometry(it->second);
}

} // namespace velk::impl
