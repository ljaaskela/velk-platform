#ifndef VELK_RENDER_INTF_MESH_H
#define VELK_RENDER_INTF_MESH_H

#include <velk/api/object_ref.h>
#include <velk/array_view.h>
#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>

#include <velk-render/blas.h>
#include <velk-render/interface/intf_buffer.h>

#include <cstdint>

namespace velk {

/**
 * @brief Combined vertex + index storage for one or more IMeshPrimitives.
 *
 * Layout inside the buffer: VBO bytes at offset 0, followed immediately
 * by IBO bytes at offset `get_ibo_offset()` (== `get_vbo_size()`).
 * A single underlying GPU buffer is allocated with shader device-
 * address usage (for buffer-reference VBO reads) and index-buffer usage
 * (so the IBO half can be bound for indexed draws at `get_ibo_offset()`).
 *
 * Primitives without an IBO (e.g. TriangleStrip unit quad) set
 * `ibo_size == 0`; the backend skips the index-bind and dispatches as
 * a non-indexed draw.
 *
 * A single IMeshBuffer may back many IMeshPrimitives (glTF's standard
 * layout: one buffer per mesh, each primitive carries a vertex/index
 * range into it). The MeshBuilder also shares buffers across fresh
 * IMesh instances for procedural primitives so repeated `get_cube(subs)`
 * calls reuse one GPU upload.
 */
class IMeshBuffer : public Interface<IMeshBuffer, IBuffer>
{
public:
    /// Replaces both VBO and IBO contents and marks the buffer dirty.
    /// Either side may be empty (pass `nullptr, 0`).
    virtual void set_data(const void* vbo_data, size_t vbo_size,
                          const void* ibo_data, size_t ibo_size) = 0;

    /// Size in bytes of the VBO region (starts at buffer offset 0).
    virtual size_t get_vbo_size() const = 0;

    /// Size in bytes of the IBO region (0 when the mesh is non-indexed).
    virtual size_t get_ibo_size() const = 0;

    /// Byte offset of the IBO region within the buffer. Always equals
    /// `get_vbo_size()`; provided for callers that pass it as the
    /// index-buffer offset to the backend's bind-index-buffer call.
    virtual size_t get_ibo_offset() const = 0;

    /// Partial VBO update: overwrites `size` bytes starting at
    /// `byte_offset` within the VBO region. Ranges already referenced
    /// by IMeshPrimitives stay valid by construction: this call never
    /// resizes. Returns Success on update, or Fail if the backend does
    /// not support partial updates yet.
    virtual ReturnValue update_vertex_range(size_t byte_offset,
                                            const void* data, size_t size) = 0;

    /// Partial IBO update. Same contract as update_vertex_range, with
    /// offsets relative to the IBO region (not the whole buffer).
    virtual ReturnValue update_index_range(size_t byte_offset,
                                           const void* data, size_t size) = 0;
};


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

/// One attribute slot inside an IMeshPrimitive's interleaved vertex buffer.
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
 * @brief One geometry + material unit: the glTF-style "primitive".
 *
 * Owns no transform (that's on the element / node), no name (that's on
 * the parent IMesh). Carries the VBO/IBO byte range describing its
 * geometry, the attribute layout, topology, local-space bounds, and
 * an ObjectRef to its authored material.
 *
 * The underlying IMeshBuffer may be shared with sibling primitives in
 * the same IMesh (glTF import) or exclusive to this primitive
 * (procedural). Callers treat it as opaque and draw using the
 * vertex/index offsets and counts reported here.
 */
class IMeshPrimitive : public Interface<IMeshPrimitive>
{
public:
    VELK_INTERFACE(
        (PROP, ObjectRef, material, {})
    )

    /// Shared VBO+IBO storage. Multiple primitives may return the same
    /// IMeshBuffer; callers key by this pointer for batching.
    virtual IMeshBuffer::Ptr get_buffer() const = 0;

    /// Byte offset of this primitive's first vertex within the buffer's
    /// VBO region. 0 when the primitive owns the buffer exclusively.
    virtual uint32_t get_vertex_offset() const = 0;

    /// Number of vertices.
    virtual uint32_t get_vertex_count() const = 0;

    /// Byte offset of this primitive's first index within the buffer's
    /// IBO region. 0 for non-indexed primitives or when exclusive.
    virtual uint32_t get_index_offset() const = 0;

    /// Number of indices. 0 for non-indexed primitives.
    virtual uint32_t get_index_count() const = 0;

    /// Interleaved vertex attribute layout, one entry per attribute slot.
    virtual array_view<VertexAttribute> get_attributes() const = 0;

    /// Bytes per vertex in the VBO.
    virtual uint32_t get_vertex_stride() const = 0;

    virtual MeshTopology get_topology() const = 0;

    /// Local-space bounds of this primitive's geometry.
    virtual aabb get_bounds() const = 0;

    /// Optional TEXCOORD_1 stream. Returns the IMeshBuffer that holds
    /// a parallel vec2-per-vertex array, or nullptr when the primitive
    /// has only one UV set. When non-null, the stream is one vec2 per
    /// vertex starting at `get_uv1_offset()` bytes into the buffer's
    /// VBO region, matched 1:1 with the primitive's vertex range.
    virtual IMeshBuffer::Ptr get_uv1_buffer() const = 0;

    /// Byte offset of this primitive's UV1 stream within its UV1
    /// buffer's VBO region. 0 when the primitive owns the buffer
    /// exclusively or has no UV1.
    virtual uint32_t get_uv1_offset() const = 0;

    /// Attaches a pre-built BVH for this primitive's triangles to be
    /// used by the RT/shadow path. Default no-op so primitive impls
    /// that don't participate in RT (or haven't built a BLAS yet) stay
    /// trivially compliant. Concrete `MeshPrimitive` overrides to
    /// store the build and serialise it through its IDrawData buffer.
    virtual void set_rt_blas(BlasBuild blas) { (void)blas; }
};

/**
 * @brief A mesh: an authored group of geometry primitives.
 *
 * Matches glTF's mesh vocabulary: a container of IMeshPrimitives plus
 * the union of their local-space bounds. Carries no transform, no
 * vertex/index data directly — those live on the primitives. A mesh
 * with exactly one primitive is the common case for procedural shapes
 * (cube, sphere) and trivial 2D fallbacks (unit quad).
 */
class IMesh : public Interface<IMesh>
{
public:
    /// Returns the mesh's primitives. Order is stable; callers that
    /// emit one DrawEntry per primitive iterate this directly.
    virtual array_view<IMeshPrimitive::Ptr> get_primitives() const = 0;

    /// Local-space AABB spanning every primitive's bounds. Lazily
    /// computed as the union on first call and cached; implementations
    /// may pre-populate at construction when the caller already knows
    /// the answer (e.g. procedural cube = [0,1]^3).
    virtual aabb get_bounds() const = 0;
};

/**
 * @brief Factory for IMesh and IMeshPrimitive.
 *
 * One instance owned by the IRenderContext (`get_mesh_builder()`).
 * Keeps IMesh/IMeshPrimitive themselves immutable — all "create and
 * fill" mutation lives here on the builder.
 *
 * Also caches engine built-in meshes (unit quad, procedural cube /
 * sphere variants). Cached meshes share their IMeshBuffer across
 * repeated calls: each call returns a fresh IMesh + IMeshPrimitive (so
 * material is per-caller), but the GPU bytes are uploaded exactly
 * once per (shape, subdivisions) combination.
 */
class IMeshBuilder : public Interface<IMeshBuilder>
{
public:
    /// Allocates a fresh primitive, uploads its geometry into a fresh
    /// exclusive IMeshBuffer, and returns it. Returns nullptr if the
    /// velk type registry hasn't loaded the render plugin.
    ///
    /// Convenience wrapper for the common "one primitive, one buffer"
    /// case. For glTF-style import where many primitives share one
    /// buffer, use `build_buffer` + `build_primitive_in_buffer`.
    virtual IMeshPrimitive::Ptr build_primitive(
        array_view<VertexAttribute> attributes,
        uint32_t vertex_stride,
        const void* vertex_data, uint32_t vertex_count,
        const uint32_t* indices, uint32_t index_count,
        MeshTopology topology,
        const aabb& bounds) = 0;

    /// Allocates an IMeshBuffer and uploads VBO and IBO bytes in one
    /// shot. Either side may be empty (pass `nullptr, 0`). Intended for
    /// glTF-style import where many primitives share one buffer;
    /// callers then construct primitives via `build_primitive_in_buffer`
    /// with byte ranges into this buffer.
    virtual IMeshBuffer::Ptr build_buffer(
        const void* vbo_data, size_t vbo_size,
        const void* ibo_data, size_t ibo_size) = 0;

    /// Constructs a primitive referencing a byte range within an
    /// existing IMeshBuffer. `vertex_offset` / `index_offset` are byte
    /// offsets into the buffer's VBO / IBO regions respectively. The
    /// buffer must outlive the primitive; velk's Ptr semantics handle
    /// that automatically.
    ///
    /// Optional @p uv1_buffer attaches a parallel TEXCOORD_1 stream
    /// sourced from a separate IMeshBuffer (typically built via a
    /// second `build_buffer` call). @p uv1_offset is the byte offset
    /// of this primitive's UV1 range within that buffer's VBO region.
    /// Pass null for primitives that do not provide UV1.
    virtual IMeshPrimitive::Ptr build_primitive_in_buffer(
        IMeshBuffer::Ptr buffer,
        uint32_t vertex_offset, uint32_t vertex_count,
        uint32_t index_offset,  uint32_t index_count,
        array_view<VertexAttribute> attributes,
        uint32_t vertex_stride,
        MeshTopology topology,
        const aabb& bounds,
        IMeshBuffer::Ptr uv1_buffer = nullptr,
        uint32_t uv1_offset = 0) = 0;

    /// Allocates a fresh mesh wrapping the given primitives. Computes
    /// the aggregate bounds lazily from primitive bounds.
    virtual IMesh::Ptr build(array_view<IMeshPrimitive::Ptr> primitives) = 0;

    /// Convenience: build a single-primitive mesh in one call.
    /// Equivalent to `build({ build_primitive(...) })`.
    virtual IMesh::Ptr build(array_view<VertexAttribute> attributes,
                             uint32_t vertex_stride,
                             const void* vertex_data, uint32_t vertex_count,
                             const uint32_t* indices, uint32_t index_count,
                             MeshTopology topology,
                             const aabb& bounds) = 0;

    /// Returns the shared unit-quad mesh used by every 2D visual.
    /// The same IMesh instance is returned across calls; the single
    /// primitive is immutable and carries no material.
    virtual IMesh::Ptr get_unit_quad() = 0;

    /// Returns a procedural cube mesh. Each call produces a fresh
    /// IMesh + IMeshPrimitive so the caller may set a per-instance
    /// material; the underlying IMeshBuffer is cached by `subdivisions`
    /// across calls.
    ///
    /// subdivisions: 0 or 1 = 24 verts / 12 tris (one quad per face,
    /// unique vertex per face corner so normals/uvs are per-face).
    /// N > 1 = N×N grid per face.
    virtual IMesh::Ptr get_cube(uint32_t subdivisions = 0) = 0;

    /// Returns a procedural UV sphere mesh. Each call produces a fresh
    /// IMesh + IMeshPrimitive; the underlying IMeshBuffer is cached.
    /// subdivisions: 0 = engine default (16 segments). N = N segments
    /// × N/2 rings.
    virtual IMesh::Ptr get_sphere(uint32_t subdivisions = 0) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_MESH_H
