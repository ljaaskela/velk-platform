#include "mesh_builder.h"

#include "mesh.h"

#include <velk/api/velk.h>

namespace velk::impl {

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

    // 4-vertex TriangleStrip. Strip order (0,1,2)(1,2,3) produces the
    // two triangles (0,0,0)-(1,0,0)-(0,1,0) and (1,0,0)-(0,1,0)-(1,1,0)
    // — the unit quad in the XY plane at z = 0, facing +Z. Every 2D
    // visual (rect, text, image, texture, env) and any mesh-style
    // visual share this same vertex layout (VelkVertex3D: vec3 pos +
    // vec3 normal + vec2 uv, 32 B scalar). No IBO; drawn with
    // vkCmdDraw(vertex_count=4).
    struct Vertex { float pos[3]; float normal[3]; float uv[2]; };
    static const Vertex verts[] = {
        // position       normal          uv
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

} // namespace velk::impl
