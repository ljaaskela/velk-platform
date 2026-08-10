#include "mesh/mesh.h"

#include <velk/api/velk.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_backend.h>

#include <cstring>

namespace velk::impl {

void MeshPrimitive::init(const IMeshBuffer::Ptr& buffer,
                          uint32_t vertex_offset, uint32_t vertex_count,
                          uint32_t index_offset, uint32_t index_count,
                          array_view<VertexAttribute> attributes,
                          uint32_t vertex_stride,
                          MeshTopology topology,
                          const aabb& bounds,
                          const IMeshBuffer::Ptr& uv1_buffer,
                          uint32_t uv1_offset)
{
    buffer_ = buffer;
    vertex_offset_ = vertex_offset;
    vertex_count_ = vertex_count;
    index_offset_ = index_offset;
    index_count_ = index_count;
    attributes_.assign(attributes.begin(), attributes.end());
    vertex_stride_ = vertex_stride;
    topology_ = topology;
    bounds_ = bounds;
    uv1_buffer_ = uv1_buffer;
    uv1_offset_ = uv1_offset;
}

void Mesh::init(array_view<IMeshPrimitive::Ptr> primitives,
                const aabb& bounds, bool has_explicit_bounds)
{
    primitives_.assign(primitives.begin(), primitives.end());
    if (has_explicit_bounds) {
        bounds_ = bounds;
        bounds_known_ = true;
    }
}

void MeshPrimitive::set_rt_blas(BlasBuild blas)
{
    rt_blas_ = std::move(blas);
    // Drop the published regions so the next ensure_rt_data republishes the
    // new build. The old regions are freed deferred past the in-flight
    // frame's fence, so a frame still tracing the previous BLAS is safe.
    rt_static_region_ = {};
    rt_blas_nodes_region_ = {};
    rt_blas_tris_region_ = {};
    rt_static_base_ = kInvalidMeshStaticBase;
}

uint32_t MeshPrimitive::ensure_rt_data(IGpuResourceManager& resources)
{
    if (rt_static_region_.valid()) return rt_static_base_;

    // RT consumes mesh primitives only when they're indexed triangle-list
    // geometry with a built BLAS. Anything else is skipped entirely.
    if (topology_ != MeshTopology::TriangleList) return kInvalidMeshStaticBase;
    if (!buffer_ || index_count_ == 0 || vertex_stride_ == 0) return kInvalidMeshStaticBase;
    if (rt_blas_.nodes.empty() || rt_blas_.triangle_indices.empty()) {
        return kInvalidMeshStaticBase;
    }

    // The geometry may not be in the mesh-word arena yet. Publish nothing and
    // leave the regions unallocated so a later frame retries, rather than
    // baking an unresolved base into a record we would then cache forever.
    // Asking through GpuRef means a buffer that is NOT arena-backed reports
    // the model mismatch instead of yielding a plausible-looking number.
    const GpuRef geometry = get_gpu_ref(buffer_);
    if (geometry.kind != GpuRef::Kind::Index) return kInvalidMeshStaticBase;
    const uint32_t geometry_base = geometry.get_base();

    auto static_arena = resources.shared_arena(IRenderBackend::kGlobalMeshStatic,
                                               sizeof(MeshStaticData));
    auto nodes_arena  = resources.shared_arena(IRenderBackend::kGlobalBlasNodes,
                                               sizeof(BlasNode));
    auto tris_arena   = resources.shared_arena(IRenderBackend::kGlobalBlasTris,
                                               sizeof(uint32_t));
    if (!static_arena || !nodes_arena || !tris_arena) return kInvalidMeshStaticBase;

    const uint64_t nodes_bytes = rt_blas_.nodes.size() * sizeof(BlasNode);
    const uint64_t tris_bytes = rt_blas_.triangle_indices.size() * sizeof(uint32_t);

    auto nodes_region = nodes_arena->alloc(nodes_bytes);
    auto tris_region = tris_arena->alloc(tris_bytes);
    auto static_region = static_arena->alloc(sizeof(MeshStaticData));
    if (!nodes_region.valid() || !tris_region.valid() || !static_region.valid()) {
        return kInvalidMeshStaticBase;
    }

    nodes_arena->write_at(nodes_region.offset(), rt_blas_.nodes.data(), nodes_bytes);
    tris_arena->write_at(tris_region.offset(), rt_blas_.triangle_indices.data(), tris_bytes);

    MeshStaticData s{};
    // Bases are word indices into the shared mesh-word arena: this mesh's
    // region base, plus the primitive's own byte offset within it / 4.
    // IBO entries are global vertex indices in our gltf-imported meshes, so
    // the vertex run starts at the region base itself.
    s.vbo_base      = geometry_base;
    s.ibo_base      = geometry_base
                    + (static_cast<uint32_t>(buffer_->get_ibo_offset()) + index_offset_) / 4u;
    s.triangle_count = index_count_ / 3;
    s.vertex_stride = vertex_stride_;
    s.blas_root      = rt_blas_.root_index;
    s.blas_node_count = static_cast<uint32_t>(rt_blas_.nodes.size());
    s.blas_node_base = static_cast<uint32_t>(nodes_region.offset() / sizeof(BlasNode));
    s.blas_tri_base  = static_cast<uint32_t>(tris_region.offset() / sizeof(uint32_t));
    static_arena->write_at(static_region.offset(), &s, sizeof(s));

    rt_static_base_ = static_cast<uint32_t>(
        static_region.offset() / sizeof(MeshStaticData));
    rt_blas_nodes_region_ = std::move(nodes_region);
    rt_blas_tris_region_ = std::move(tris_region);
    rt_static_region_ = std::move(static_region);
    return rt_static_base_;
}

aabb Mesh::get_bounds() const
{
    std::call_once(bounds_once_, [&]() {
        if (bounds_known_) return;
        if (primitives_.empty()) {
            bounds_ = aabb{};
        } else {
            bounds_ = primitives_.front()->get_bounds();
            for (size_t i = 1; i < primitives_.size(); ++i) {
                bounds_ = aabb::merge(bounds_, primitives_[i]->get_bounds());
            }
        }
        bounds_known_ = true;
    });
    return bounds_;
}

} // namespace velk::impl
