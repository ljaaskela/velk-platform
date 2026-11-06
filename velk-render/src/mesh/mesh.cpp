#include "mesh/mesh.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_backend.h>

#include <cstring>

namespace velk::impl {

namespace {

/// A deforming primitive's tree is built again when refitting raises its cost past
/// this factor.
constexpr float kBlasRebuildDrift = 1.5f;

} // namespace

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
    rt_cluster_words_region_ = {};
    rt_static_base_ = kInvalidMeshStaticBase;
}

uint32_t MeshPrimitive::ensure_rt_data(IGpuResourceManager& resources)
{
    if (rt_static_region_.valid()) return rt_static_base_;

    // RT consumes mesh primitives only when they're indexed triangle-list
    // geometry with a built BLAS. Anything else is skipped entirely.
    if (topology_ != MeshTopology::TriangleList) return kInvalidMeshStaticBase;
    if (!buffer_ || index_count_ == 0 || vertex_stride_ == 0) return kInvalidMeshStaticBase;
    if (rt_blas_.nodes.empty() || rt_blas_.cluster_words.empty()) {
        return kInvalidMeshStaticBase;
    }

    // The geometry may not be in the mesh-word arena yet. Publish nothing and
    // leave the regions unallocated so a later frame retries, rather than
    // baking an unresolved base into a record we would then cache forever.
    // Asking through GpuRef means a buffer that is NOT arena-backed reports
    // the model mismatch instead of yielding a plausible-looking number.
    if (get_gpu_ref(buffer_).kind != GpuRef::Kind::Index) return kInvalidMeshStaticBase;

    auto static_arena = resources.shared_arena(IRenderBackend::kGlobalMeshStatic,
                                               sizeof(MeshStaticData));
    if (!static_arena) return kInvalidMeshStaticBase;
    auto static_region = static_arena->alloc(sizeof(MeshStaticData));
    if (!static_region.valid()) return kInvalidMeshStaticBase;

    MeshStaticData s{};
    if (!publish_tree(resources, s)) return kInvalidMeshStaticBase;
    static_arena->write_at(static_region.offset(), &s, sizeof(s));

    rt_static_base_ = static_cast<uint32_t>(
        static_region.offset() / sizeof(MeshStaticData));
    rt_static_region_ = std::move(static_region);
    return rt_static_base_;
}

bool MeshPrimitive::publish_tree(IGpuResourceManager& resources, MeshStaticData& out)
{
    auto nodes_arena = resources.shared_arena(IRenderBackend::kGlobalBlasNodes, sizeof(BlasNode));
    auto words_arena = resources.shared_arena(IRenderBackend::kGlobalClusterWords, sizeof(uint32_t));
    if (!nodes_arena || !words_arena) return false;

    const uint64_t nodes_bytes = rt_blas_.nodes.size() * sizeof(BlasNode);
    const uint64_t words_bytes = rt_blas_.cluster_words.size() * sizeof(uint32_t);
    auto nodes_region = nodes_arena->alloc(nodes_bytes);
    auto words_region = words_arena->alloc(words_bytes);
    if (!nodes_region.valid() || !words_region.valid()) return false;

    nodes_arena->write_at(nodes_region.offset(), rt_blas_.nodes.data(), nodes_bytes);
    words_arena->write_at(words_region.offset(), rt_blas_.cluster_words.data(), words_bytes);

    // A base is a word index into the shared mesh-word arena. IBO entries are
    // global vertex indices in our gltf-imported meshes, so the vertex run starts
    // at the region base itself.
    out.vbo_base = get_gpu_ref(buffer_).get_base();
    out.triangle_count = index_count_ / 3;
    out.vertex_stride = vertex_stride_;
    out.blas_root = rt_blas_.root_index;
    out.blas_node_count = static_cast<uint32_t>(rt_blas_.nodes.size());
    out.blas_node_base = static_cast<uint32_t>(nodes_region.offset() / sizeof(BlasNode));
    out.cluster_word_base = static_cast<uint32_t>(words_region.offset() / sizeof(uint32_t));

    rt_blas_nodes_region_ = std::move(nodes_region);
    rt_cluster_words_region_ = std::move(words_region);
    return true;
}

void MeshPrimitive::refit(array_view<const MeshVertex> vertices, array_view<const uint32_t> indices,
                          IGpuResourceManager& resources)
{
    if (rt_blas_.nodes.empty()) return;
    VELK_PERF_SCOPE("mesh.refit");

    const auto* floats = reinterpret_cast<const float*>(vertices.begin());
    if (rt_built_cost_ == 0.f) {
        rt_built_cost_ = blas_tree_cost(rt_blas_);
    }
    refit_mesh_blas(rt_blas_, floats, vertex_stride_);
    if (blas_tree_cost(rt_blas_) > rt_built_cost_ * kBlasRebuildDrift) {
        VELK_PERF_SCOPE("mesh.blas_rebuild");
        rt_blas_ = build_mesh_blas(floats, vertex_stride_, static_cast<uint32_t>(vertices.size()),
                                   indices.begin() + index_offset_ / sizeof(uint32_t), index_count_ / 3);
        rt_built_cost_ = blas_tree_cost(rt_blas_);
    }

    const BlasNode& root = rt_blas_.nodes[rt_blas_.root_index];
    bounds_.position = {root.aabb_min[0], root.aabb_min[1], root.aabb_min[2]};
    bounds_.extent = {root.aabb_max[0] - root.aabb_min[0], root.aabb_max[1] - root.aabb_min[1],
                      root.aabb_max[2] - root.aabb_min[2]};

    if (!rt_static_region_.valid()) return;

    const uint64_t nodes_bytes = rt_blas_.nodes.size() * sizeof(BlasNode);
    const uint64_t words_bytes = rt_blas_.cluster_words.size() * sizeof(uint32_t);
    if (nodes_bytes == rt_blas_nodes_region_.size() && words_bytes == rt_cluster_words_region_.size()) {
        auto nodes_arena = resources.shared_arena(IRenderBackend::kGlobalBlasNodes, sizeof(BlasNode));
        auto words_arena = resources.shared_arena(IRenderBackend::kGlobalClusterWords, sizeof(uint32_t));
        if (nodes_arena && words_arena) {
            nodes_arena->update_at(rt_blas_nodes_region_.offset(), rt_blas_.nodes.data(), nodes_bytes);
            words_arena->update_at(rt_cluster_words_region_.offset(), rt_blas_.cluster_words.data(), words_bytes);
        }
        return;
    }

    // A new tree of another size takes new regions. The old ones retire on the fence,
    // and the record pointing at them keeps its base.
    MeshStaticData s{};
    auto static_arena = resources.shared_arena(IRenderBackend::kGlobalMeshStatic, sizeof(MeshStaticData));
    if (static_arena && publish_tree(resources, s)) {
        static_arena->update_at(rt_static_region_.offset(), &s, sizeof(s));
    } else {
        VELK_LOG(E, "MeshPrimitive: no room for a rebuilt tree, rays use the previous one");
    }
}

bool DeformingMesh::init(const IMesh& source, IMeshBuilder& builder)
{
    for (const auto& primitive : source.get_primitives()) {
        auto buffer = primitive->get_buffer();
        if (!buffer || buffer->get_ibo_size() == 0 || primitive->get_vertex_stride() != sizeof(MeshVertex)
            || primitive->get_topology() != MeshTopology::TriangleList) {
            VELK_LOG(E, "build_deforming: a mesh must be an indexed triangle list of MeshVertex");
            return false;
        }

        uint32_t stream_index = 0;
        while (stream_index < streams_.size() && streams_[stream_index].source != buffer) {
            ++stream_index;
        }
        if (stream_index == streams_.size()) {
            vector<uint8_t> bytes;
            auto* internal = interface_cast<IMeshBufferInternal>(buffer.get());
            if (!internal || !internal->read_geometry(bytes)) {
                VELK_LOG(E, "build_deforming: could not read the source mesh's geometry");
                return false;
            }
            const size_t vbo_size = buffer->get_vbo_size();
            const size_t ibo_size = buffer->get_ibo_size();
            const uint8_t* index_bytes = bytes.data() + buffer->get_ibo_offset();
            const auto* first_vertex = reinterpret_cast<const MeshVertex*>(bytes.data());
            const auto* first_index = reinterpret_cast<const uint32_t*>(index_bytes);

            Stream stream;
            stream.source = buffer;
            stream.buffer = builder.build_buffer(bytes.data(), vbo_size, index_bytes, ibo_size);
            stream.rest.assign(first_vertex, first_vertex + vbo_size / sizeof(MeshVertex));
            stream.vertices = stream.rest;
            stream.indices.assign(first_index, first_index + ibo_size / sizeof(uint32_t));
            streams_.push_back(std::move(stream));
        }
        const Stream& stream = streams_[stream_index];

        auto copy = builder.build_primitive_in_buffer(
            stream.buffer, primitive->get_vertex_offset(), primitive->get_vertex_count(),
            primitive->get_index_offset(), primitive->get_index_count(), primitive->get_attributes(),
            primitive->get_vertex_stride(), primitive->get_topology(), primitive->get_bounds(),
            primitive->get_uv1_buffer(), primitive->get_uv1_offset());
        if (!copy) {
            return false;
        }
        if (auto from = read_state<IMeshPrimitive>(primitive.get())) {
            write_state<IMeshPrimitive>(copy.get(), [&](IMeshPrimitive::State& s) { s.material = from->material; });
        }
        copy->set_rt_blas(build_mesh_blas(reinterpret_cast<const float*>(stream.rest.data()), sizeof(MeshVertex),
                                          static_cast<uint32_t>(stream.rest.size()),
                                          stream.indices.data() + primitive->get_index_offset() / sizeof(uint32_t),
                                          primitive->get_index_count() / 3));
        primitives_.push_back(copy);
        primitive_streams_.push_back(stream_index);
    }
    return !primitives_.empty();
}

void DeformingMesh::deform(IMeshDeformer& deformer, IGpuResourceManager& resources)
{
    for (auto& stream : streams_) {
        deformer.deform({stream.rest.data(), stream.rest.size()}, stream.vertices.data());
        stream.buffer->update_vertex_range(0, stream.vertices.data(), stream.vertices.size() * sizeof(MeshVertex));
    }
    for (size_t i = 0; i < primitives_.size(); ++i) {
        if (auto* primitive = interface_cast<IMeshPrimitiveInternal>(primitives_[i].get())) {
            const Stream& stream = streams_[primitive_streams_[i]];
            primitive->refit({stream.vertices.data(), stream.vertices.size()},
                             {stream.indices.data(), stream.indices.size()}, resources);
        }
    }
}

aabb DeformingMesh::get_bounds() const
{
    if (primitives_.empty()) {
        return {};
    }
    aabb bounds = primitives_.front()->get_bounds();
    for (size_t i = 1; i < primitives_.size(); ++i) {
        bounds = aabb::merge(bounds, primitives_[i]->get_bounds());
    }
    return bounds;
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
