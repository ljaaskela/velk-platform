#ifndef VELK_RENDER_MESH_H
#define VELK_RENDER_MESH_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <velk-render/blas.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/plugin.h>

#include <mutex>

namespace velk::impl {

/// Concrete IMeshPrimitive. Holds the geometry range into an
/// IMeshBuffer (may be shared with sibling primitives), the attribute
/// layout, topology, bounds, and a material ObjectRef.
///
/// Also owns this primitive's RT geometry data: persistent regions of the
/// shared mesh-static / BLAS-node / BLAS-triangle arenas, published through
/// `ensure_rt_data`. The element bases are stable for the primitive's
/// lifetime, so mesh instances cache the mesh-static index once and only the
/// per-shape transforms change per frame.
class MeshPrimitive
    : public ::velk::ext::Object<MeshPrimitive, IMeshPrimitive, IMeshPrimitiveInternal>
{
public:
    VELK_CLASS_UID(::velk::ClassId::MeshPrimitive, "MeshPrimitive");

    MeshPrimitive() = default;

    /// Internal: populates the primitive from a pre-uploaded or
    /// pre-filled IMeshBuffer. Reached via interface_cast on
    /// IMeshPrimitiveInternal so primitives stay immutable from the
    /// IMeshPrimitive consumer perspective.
    void init(const IMeshBuffer::Ptr& buffer,
              uint32_t vertex_offset, uint32_t vertex_count,
              uint32_t index_offset, uint32_t index_count,
              array_view<VertexAttribute> attributes,
              uint32_t vertex_stride,
              MeshTopology topology,
              const aabb& bounds,
              const IMeshBuffer::Ptr& uv1_buffer,
              uint32_t uv1_offset) override;

    IMeshBuffer::Ptr get_buffer() const override { return buffer_; }
    uint32_t get_vertex_offset() const override { return vertex_offset_; }
    uint32_t get_vertex_count() const override { return vertex_count_; }
    uint32_t get_index_offset() const override { return index_offset_; }
    uint32_t get_index_count() const override { return index_count_; }
    array_view<VertexAttribute> get_attributes() const override
    {
        return {attributes_.data(), attributes_.size()};
    }
    uint32_t get_vertex_stride() const override { return vertex_stride_; }
    MeshTopology get_topology() const override { return topology_; }
    aabb get_bounds() const override { return bounds_; }
    IMeshBuffer::Ptr get_uv1_buffer() const override { return uv1_buffer_; }
    uint32_t get_uv1_offset() const override { return uv1_offset_; }

    // IMeshPrimitiveInternal: publishes MeshStaticData + the BLAS into the
    // shared arenas. Triangle-list primitives backed by an indexed buffer
    // with a built BLAS return a valid base; everything else returns
    // kInvalidMeshStaticBase (we don't emit RT shapes for those).
    uint32_t ensure_rt_data(IGpuResourceManager& resources) override;

    /// Stores a pre-built BLAS for this primitive and releases the current
    /// arena regions, so the next `ensure_rt_data` republishes the nodes +
    /// triangle indices and the RT path walks the acceleration structure
    /// instead of doing a linear triangle scan.
    void set_rt_blas(BlasBuild blas) override;

private:
    IMeshBuffer::Ptr buffer_;
    uint32_t vertex_offset_ = 0;
    uint32_t vertex_count_ = 0;
    uint32_t index_offset_ = 0;
    uint32_t index_count_ = 0;
    ::velk::vector<VertexAttribute> attributes_;
    uint32_t vertex_stride_ = 0;
    MeshTopology topology_ = MeshTopology::TriangleList;
    aabb bounds_{};
    IMeshBuffer::Ptr uv1_buffer_;
    uint32_t uv1_offset_ = 0;

    /// This primitive's regions in the shared RT arenas (set = 1 slots
    /// 11 / 12 / 13), allocated together on the first successful
    /// `ensure_rt_data` and held for the primitive's lifetime so
    /// `rt_static_base_` stays stable. Freed deferred past the fence when
    /// `set_rt_blas` replaces the build or the primitive is destroyed.
    ArenaRegion rt_static_region_;
    ArenaRegion rt_blas_nodes_region_;
    ArenaRegion rt_blas_tris_region_;

    /// Element index of `rt_static_region_`, or kInvalidMeshStaticBase
    /// while the RT data is not published.
    uint32_t rt_static_base_ = kInvalidMeshStaticBase;

    /// Pre-built BLAS for the RT path. Empty until `set_rt_blas` runs;
    /// `get_draw_data_size` and `write_draw_data` include the BLAS
    /// payload only when populated.
    BlasBuild rt_blas_;
};

/// Concrete IMesh container. Stores a list of primitives and a lazily
/// computed aggregate bounds.
class Mesh
    : public ::velk::ext::Object<Mesh, IMesh, IMeshInternal>
{
public:
    VELK_CLASS_UID(::velk::ClassId::Mesh, "Mesh");

    Mesh() = default;

    /// Internal: installs the primitive list. Bounds is taken as-is
    /// when `has_explicit_bounds` is true (procedural shapes know their
    /// exact extent analytically); otherwise the aggregate is computed
    /// lazily on first `get_bounds` call from the primitive bounds.
    void init(array_view<IMeshPrimitive::Ptr> primitives,
              const aabb& bounds, bool has_explicit_bounds) override;

    array_view<IMeshPrimitive::Ptr> get_primitives() const override
    {
        return {primitives_.data(), primitives_.size()};
    }
    aabb get_bounds() const override;

private:
    ::velk::vector<IMeshPrimitive::Ptr> primitives_;
    mutable aabb bounds_{};
    mutable bool bounds_known_ = false;
    mutable std::once_flag bounds_once_;
};

} // namespace velk::impl

#endif // VELK_RENDER_MESH_H
