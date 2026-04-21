#ifndef VELK_RENDER_INTF_MESH_H
#define VELK_RENDER_INTF_MESH_H

#include <velk/array_view.h>
#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/intf_buffer.h>

#include <cstdint>

namespace velk {

/// Semantic role of a vertex attribute. Matches glTF 2.0 attribute names so
/// importers map 1:1. `Custom` is for app-defined attributes the engine
/// doesn't know about.
enum class VertexAttributeSemantic : uint8_t
{
    Position,
    Normal,
    Tangent,
    TexCoord0,
    TexCoord1,
    Color,
    Joints,
    Weights,
    Custom,
};

/// Storage format of a single vertex attribute.
enum class VertexAttributeFormat : uint8_t
{
    Float2,    ///< 2 x f32 (8 bytes)
    Float3,    ///< 3 x f32 (12 bytes)
    Float4,    ///< 4 x f32 (16 bytes)
    U8x4Norm,  ///< 4 x u8 normalised to [0,1] (4 bytes)
    U16x2,     ///< 2 x u16 (4 bytes)
    U32x4,     ///< 4 x u32 (16 bytes)
};

/// One attribute slot inside an IMesh's interleaved vertex buffer.
struct VertexAttribute
{
    VertexAttributeSemantic semantic;
    VertexAttributeFormat format;
    uint32_t offset;  ///< Byte offset within a vertex.
};

/// Primitive topology.
enum class MeshTopology : uint8_t
{
    TriangleList,   ///< 3 indices per triangle (used by glTF and most 3D meshes).
    TriangleStrip,  ///< Each new vertex extends a strip; ideal for the unit quad (4 verts, no IBO).
};

/**
 * @brief Geometry input to a draw call: vertex buffer + index buffer +
 *        layout descriptor.
 *
 * IMesh owns two `IBuffer`s (interleaved VBO and a uint32 IBO) plus the
 * attribute layout describing how vertices are packed. Both buffers are
 * read by the shader via `buffer_reference` device addresses (no
 * descriptor binds, no vertex input state on the pipeline).
 *
 * The renderer treats the VBO and IBO as opaque bytes; the shader's own
 * `struct Vertex { ... }` declaration defines the actual interpretation.
 * The attribute list exists so the C++ side and external tooling (glTF
 * importers, etc.) can know the stride and which attributes are present.
 */
class IMesh : public Interface<IMesh>
{
public:
    /// Interleaved vertex attribute layout, one entry per attribute slot.
    virtual array_view<VertexAttribute> get_attributes() const = 0;

    /// Bytes per vertex in the VBO.
    virtual uint32_t get_vertex_stride() const = 0;

    /// Number of vertices in the VBO.
    virtual uint32_t get_vertex_count() const = 0;

    /// Number of indices in the IBO.
    virtual uint32_t get_index_count() const = 0;

    virtual MeshTopology get_topology() const = 0;

    /// Local-space bounds of the mesh geometry.
    virtual aabb get_bounds() const = 0;

    /// Vertex buffer (interleaved attribute data).
    virtual IBuffer::Ptr get_vbo() const = 0;

    /// Index buffer (uint32 indices). Mandatory for now.
    virtual IBuffer::Ptr get_ibo() const = 0;
};

/**
 * @brief Factory for IMesh.
 *
 * One instance owned by the IRenderContext (`get_mesh_builder()`).
 * Keeps IMesh itself immutable — all "create and fill" mutation lives
 * here on the builder.
 *
 * Also caches engine built-in meshes (the unit quad shared by every 2D
 * visual) so they exist exactly once per render context and are
 * released cleanly when the context is.
 */
class IMeshBuilder : public Interface<IMeshBuilder>
{
public:
    /// Allocates a fresh mesh, fills it with the given geometry, and
    /// returns it. May return nullptr if the velk type registry hasn't
    /// loaded the render plugin (the Mesh class registration happens
    /// there).
    virtual IMesh::Ptr build(array_view<VertexAttribute> attributes,
                             uint32_t vertex_stride,
                             const void* vertex_data, uint32_t vertex_count,
                             const uint32_t* indices, uint32_t index_count,
                             MeshTopology topology,
                             const aabb& bounds) = 0;

    /// Returns the shared unit-quad mesh used by every 2D visual.
    /// Lazily built on first call; the same instance is returned for
    /// every subsequent call on this builder.
    virtual IMesh::Ptr get_unit_quad() = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_MESH_H
