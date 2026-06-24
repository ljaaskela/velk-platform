#include "mesh/mesh_builder.h"

#include <velk/api/velk.h>
#include <velk/vector.h>

#include <velk-render/gpu_data.h>

#include <cmath>

namespace velk::impl {

namespace {

// Vertex layout shared by all procedural primitives. Matches the
// `VelkVertex3D` struct in velk.glsl declared with `layout(scalar)`
// so vec3 fields pack tightly (12-byte aligned) instead of being
// rounded up to 16 by std430.
//
//   offset  0..12  position
//   offset 12..24  normal
//   offset 24..32  uv
//   offset 32..48  tangent (xyz dir + w handedness)
// Stride = 48 bytes.
constexpr uint32_t kVertex3DStride = 48;

const VertexAttribute kVertex3DAttributes[] = {
    { VertexAttributeSemantic::Position,  VertexAttributeFormat::Float3, 0 },
    { VertexAttributeSemantic::Normal,    VertexAttributeFormat::Float3, 12 },
    { VertexAttributeSemantic::TexCoord0, VertexAttributeFormat::Float2, 24 },
    { VertexAttributeSemantic::Tangent,   VertexAttributeFormat::Float4, 32 },
};

struct Vertex3D
{
    float position[3];
    float normal[3];
    float uv[2];
    float tangent[4];
};
static_assert(sizeof(Vertex3D) == kVertex3DStride,
              "Vertex3D must be tightly packed to match GLSL scalar layout");

// Appends one N×N face into the interleaved vertex and index buffers.
// `origin`, `u_axis`, `v_axis` span the [0,1]^2 face in the mesh's
// local unit-bounds space; `normal` is the outward-facing normal.
void append_face(::velk::vector<Vertex3D>& verts,
                 ::velk::vector<uint32_t>& indices,
                 const float origin[3],
                 const float u_axis[3],
                 const float v_axis[3],
                 const float normal[3],
                 uint32_t grid)
{
    const uint32_t base = static_cast<uint32_t>(verts.size());
    const uint32_t side = grid + 1;  // vertices per side

    for (uint32_t vy = 0; vy < side; ++vy) {
        for (uint32_t vx = 0; vx < side; ++vx) {
            float u = static_cast<float>(vx) / static_cast<float>(grid);
            float v = static_cast<float>(vy) / static_cast<float>(grid);
            Vertex3D vert{};
            for (int i = 0; i < 3; ++i) {
                vert.position[i] = origin[i] + u_axis[i] * u + v_axis[i] * v;
                vert.normal[i] = normal[i];
            }
            vert.uv[0] = u;
            vert.uv[1] = v;
            // Tangent runs along the face U axis (matches uv.x); the faces use
            // unit-length axes, so no normalization needed. Handedness +1.
            vert.tangent[0] = u_axis[0];
            vert.tangent[1] = u_axis[1];
            vert.tangent[2] = u_axis[2];
            vert.tangent[3] = 1.f;
            verts.push_back(vert);
        }
    }

    for (uint32_t vy = 0; vy < grid; ++vy) {
        for (uint32_t vx = 0; vx < grid; ++vx) {
            uint32_t i00 = base + vy * side + vx;
            uint32_t i10 = i00 + 1;
            uint32_t i01 = i00 + side;
            uint32_t i11 = i01 + 1;
            // CCW winding when viewed from outside (normal direction).
            indices.push_back(i00);
            indices.push_back(i10);
            indices.push_back(i11);
            indices.push_back(i00);
            indices.push_back(i11);
            indices.push_back(i01);
        }
    }
}

// Builds a shared IMeshBuffer from VBO + IBO bytes.
IMeshBuffer::Ptr upload_mesh_buffer(const void* vbo, size_t vbo_size,
                                     const uint32_t* ibo, size_t index_count)
{
    auto buffer = ::velk::instance().create<IMeshBuffer>(::velk::ClassId::MeshBuffer);
    if (!buffer) return nullptr;
    buffer->set_data(vbo, vbo_size,
                     ibo, ibo ? index_count * sizeof(uint32_t) : 0);
    return buffer;
}

} // namespace

MeshBuilder::CachedGeometry MeshBuilder::make_cube_geometry(uint32_t subdivisions)
{
    const uint32_t grid = subdivisions == 0 ? 1u : subdivisions;

    ::velk::vector<Vertex3D> verts;
    ::velk::vector<uint32_t> indices;

    // Unit cube occupying [0,1]^3. Faces ordered: -X, +X, -Y, +Y, -Z, +Z.
    // Per-face vertices so each face has its own normals/uvs.
    struct FaceDef
    {
        float origin[3];
        float u[3];
        float v[3];
        float n[3];
    };
    const FaceDef faces[] = {
        { {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, {-1, 0, 0} },
        { {1, 0, 1}, {0, 0, -1}, {0, 1, 0}, {1, 0, 0} },
        { {0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {0, -1, 0} },
        { {0, 1, 1}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0} },
        { {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, 0, -1} },
        { {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1} },
    };
    for (const auto& f : faces) {
        append_face(verts, indices, f.origin, f.u, f.v, f.n, grid);
    }

    CachedGeometry g;
    g.buffer = upload_mesh_buffer(verts.data(), verts.size() * sizeof(Vertex3D),
                                   indices.data(), indices.size());
    g.attributes.assign(std::begin(kVertex3DAttributes), std::end(kVertex3DAttributes));
    g.vertex_stride = kVertex3DStride;
    g.vertex_count = static_cast<uint32_t>(verts.size());
    g.index_count = static_cast<uint32_t>(indices.size());
    g.topology = MeshTopology::TriangleList;
    g.bounds.position = { 0.f, 0.f, 0.f };
    g.bounds.extent = { 1.f, 1.f, 1.f };
    return g;
}

MeshBuilder::CachedGeometry MeshBuilder::make_sphere_geometry(uint32_t subdivisions)
{
    const uint32_t segments = subdivisions == 0 ? 16u : subdivisions;
    const uint32_t rings = (segments / 2u) > 2u ? (segments / 2u) : 3u;

    // Unit sphere inscribed in [0,1]^3: radius 0.5, center (0.5, 0.5, 0.5).
    // Using unit-bounds convention so the instance `size` scale maps
    // 1:1 with the element's axis-aligned extent.
    constexpr float kCenter = 0.5f;
    constexpr float kRadius = 0.5f;
    constexpr float kPi = 3.14159265358979323846f;

    ::velk::vector<Vertex3D> verts;
    ::velk::vector<uint32_t> indices;

    for (uint32_t r = 0; r <= rings; ++r) {
        float vy = static_cast<float>(r) / static_cast<float>(rings);
        float phi = vy * kPi;
        float sin_phi = std::sin(phi);
        float cos_phi = std::cos(phi);
        for (uint32_t s = 0; s <= segments; ++s) {
            float vx = static_cast<float>(s) / static_cast<float>(segments);
            float theta = vx * kPi * 2.0f;
            float sin_theta = std::sin(theta);
            float cos_theta = std::cos(theta);

            Vertex3D vert{};
            vert.normal[0] = sin_phi * cos_theta;
            vert.normal[1] = cos_phi;
            vert.normal[2] = sin_phi * sin_theta;
            vert.position[0] = kCenter + kRadius * vert.normal[0];
            vert.position[1] = kCenter + kRadius * vert.normal[1];
            vert.position[2] = kCenter + kRadius * vert.normal[2];
            vert.uv[0] = vx;
            vert.uv[1] = vy;
            // Tangent = d(position)/d(theta), normalized (sin_phi factors out;
            // degenerate at the poles where it falls back to +X). Handedness +1.
            vert.tangent[0] = -sin_theta;
            vert.tangent[1] = 0.f;
            vert.tangent[2] = cos_theta;
            vert.tangent[3] = 1.f;
            verts.push_back(vert);
        }
    }

    const uint32_t stride = segments + 1;
    for (uint32_t r = 0; r < rings; ++r) {
        for (uint32_t s = 0; s < segments; ++s) {
            uint32_t i00 = r * stride + s;
            uint32_t i10 = i00 + 1;
            uint32_t i01 = i00 + stride;
            uint32_t i11 = i01 + 1;
            indices.push_back(i00);
            indices.push_back(i01);
            indices.push_back(i11);
            indices.push_back(i00);
            indices.push_back(i11);
            indices.push_back(i10);
        }
    }

    CachedGeometry g;
    g.buffer = upload_mesh_buffer(verts.data(), verts.size() * sizeof(Vertex3D),
                                   indices.data(), indices.size());
    g.attributes.assign(std::begin(kVertex3DAttributes), std::end(kVertex3DAttributes));
    g.vertex_stride = kVertex3DStride;
    g.vertex_count = static_cast<uint32_t>(verts.size());
    g.index_count = static_cast<uint32_t>(indices.size());
    g.topology = MeshTopology::TriangleList;
    g.bounds.position = { 0.f, 0.f, 0.f };
    g.bounds.extent = { 1.f, 1.f, 1.f };
    return g;
}

} // namespace velk::impl
