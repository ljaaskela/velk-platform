#include "mesh/mesh.h"

#include "mesh/mesh_deform_shaders.h"
#include "mesh/mesh_lod.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_gpu_command_buffer.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/render_path/frame_context.h>
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
    lod_table_region_ = {};
    lod_index_region_ = {};
}

bool MeshPrimitive::ensure_lod_data(IGpuResourceManager& resources, MeshLodInfo& out)
{
    if (!lod_table_region_.valid()) {
        // A single cluster has nothing to choose between.
        if (deforming_ || rt_blas_.clusters.size() < 2) {
            return false;
        }
        auto culling = resources.shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
        auto mesh_words = resources.shared_arena(IRenderBackend::kGlobalMeshWords, sizeof(uint32_t), true);
        if (!culling || !mesh_words) {
            return false;
        }
        vector<uint8_t> geometry;
        auto* buffer = interface_cast<IMeshBufferInternal>(buffer_.get());
        const VertexAttribute* normal = nullptr;
        for (const auto& attribute : attributes_) {
            if (attribute.semantic == VertexAttributeSemantic::Normal && attribute.format == VertexAttributeFormat::Float3) {
                normal = &attribute;
            }
        }
        const bool has_normals = normal && buffer && buffer->read_geometry(geometry);
        const MeshLod lod = build_mesh_lod(rt_blas_, has_normals ? geometry.data() : nullptr, vertex_stride_,
                                           has_normals ? normal->offset : 0u);
        const size_t clusters_size = lod.clusters.size() * sizeof(MeshLodCluster);
        const size_t levels_size = lod.levels.size() * sizeof(MeshLodLevel);
        lod_table_region_ = culling->alloc(clusters_size + levels_size);
        lod_index_region_ = mesh_words->alloc(lod.indices.size() * sizeof(uint32_t));
        if (!lod_table_region_.valid() || !lod_index_region_.valid()) {
            lod_table_region_ = {};
            lod_index_region_ = {};
            return false;
        }
        culling->write_at(lod_table_region_.offset(), lod.clusters.data(), clusters_size);
        culling->write_at(lod_table_region_.offset() + clusters_size, lod.levels.data(), levels_size);
        mesh_words->write_at(lod_index_region_.offset(), lod.indices.data(), lod.indices.size() * sizeof(uint32_t));

        lod_info_.cluster_word = static_cast<uint32_t>(lod_table_region_.offset() / sizeof(uint32_t));
        lod_info_.cluster_count = static_cast<uint32_t>(lod.clusters.size());
        lod_info_.level0_clusters = lod.levels[0].cluster_count;
        lod_info_.level_word = lod_info_.cluster_word + lod_info_.cluster_count * kLodClusterWords;
        lod_info_.level_count = static_cast<uint32_t>(lod.levels.size());
        lod_info_.index_word = static_cast<uint32_t>(lod_index_region_.offset() / sizeof(uint32_t));
    }
    out = lod_info_;
    return true;
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
        if (auto* copy_internal = interface_cast<IMeshPrimitiveInternal>(copy.get())) {
            copy_internal->set_deforming();
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
        if (stream.gpu_rest.valid()) {
            VELK_LOG(E, "DeformingMesh: a mesh deformed on the GPU cannot go back to the CPU");
            return;
        }
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

void MeshPrimitive::set_gpu_displacement(float max_displacement)
{
    if (!has_rest_bounds_) {
        rest_bounds_ = bounds_;
        has_rest_bounds_ = true;
    }
    bounds_.position = {rest_bounds_.position.x - max_displacement, rest_bounds_.position.y - max_displacement,
                        rest_bounds_.position.z - max_displacement};
    bounds_.extent = {rest_bounds_.extent.width + max_displacement * 2.f,
                      rest_bounds_.extent.height + max_displacement * 2.f,
                      rest_bounds_.extent.depth + max_displacement * 2.f};
}

bool MeshPrimitive::prepare_gpu_refit(IGpuResourceManager& resources, MeshRefitInfo& out)
{
    if (!rt_static_region_.valid()) {
        return false;
    }
    if (!gpu_refit_region_.valid()) {
        auto arena = resources.shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
        if (!arena) {
            return false;
        }
        const auto node_count = static_cast<uint32_t>(rt_blas_.nodes.size());
        vector<uint32_t> leaves;
        vector<uint32_t> parents;
        parents.resize(node_count, 0xFFFFFFFFu);
        for (uint32_t n = 0; n < node_count; ++n) {
            const BlasNode& node = rt_blas_.nodes[n];
            if (node.child_count == 0) {
                leaves.push_back(n);
            }
            for (uint32_t c = 0; c < node.child_count; ++c) {
                parents[node.first_child + c] = n;
            }
        }
        const auto leaf_count = static_cast<uint32_t>(leaves.size());
        gpu_refit_region_ = arena->alloc(
            (uint64_t(leaf_count) + uint64_t(node_count) * 2u) * sizeof(uint32_t));
        if (!gpu_refit_region_.valid()) {
            VELK_LOG(E, "MeshPrimitive: no room for a refit on the GPU");
            return false;
        }
        const uint64_t offset = gpu_refit_region_.offset();
        arena->write_at(offset, leaves.data(), leaf_count * sizeof(uint32_t));
        arena->write_at(offset + leaf_count * sizeof(uint32_t), parents.data(), node_count * sizeof(uint32_t));

        const auto first_word = static_cast<uint32_t>(offset / sizeof(uint32_t));
        gpu_refit_.node_base = static_cast<uint32_t>(rt_blas_nodes_region_.offset() / sizeof(BlasNode));
        gpu_refit_.word_base = static_cast<uint32_t>(rt_cluster_words_region_.offset() / sizeof(uint32_t));
        gpu_refit_.vertex_base = get_gpu_ref(buffer_).get_base();
        gpu_refit_.leaf_word = first_word;
        gpu_refit_.leaf_count = leaf_count;
        gpu_refit_.parent_word = first_word + leaf_count;
        gpu_refit_.counter_word = gpu_refit_.parent_word + node_count;
        gpu_refit_.node_count = node_count;

        // From here on the tree lives on the GPU.
        rt_blas_ = BlasBuild{};
    }
    out = gpu_refit_;
    return true;
}

void DeformingMesh::deform_on_gpu(IComputeMeshDeformer& deformer, IGpuResourceManager& resources)
{
    auto arena = resources.shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) {
        return;
    }

    for (auto& stream : streams_) {
        if (stream.gpu_rest.valid()) {
            continue;
        }
        const uint64_t bytes = stream.rest.size() * sizeof(MeshVertex);
        stream.gpu_rest = arena->alloc(bytes);
        if (!stream.gpu_rest.valid()) {
            VELK_LOG(E, "DeformingMesh: no room for rest vertices on the GPU");
            return;
        }
        arena->write_at(stream.gpu_rest.offset(), stream.rest.data(), bytes);
        stream.vertex_count = static_cast<uint32_t>(stream.rest.size());
        stream.rest = {};
        stream.vertices = {};
        stream.indices = {};
    }

    constexpr uint32_t kTableWords = 8;
    if (!gpu_table_.valid()) {
        gpu_table_ = arena->alloc(kTableWords * sizeof(uint32_t));
        if (!gpu_table_.valid()) {
            return;
        }
        const uint32_t zero[kTableWords] = {};
        arena->write_at(gpu_table_.offset(), zero, sizeof(zero));
    }

    const float max_displacement = deformer.get_max_displacement();
    if (max_displacement != gpu_max_displacement_) {
        gpu_max_displacement_ = max_displacement;
        for (auto& primitive : primitives_) {
            if (auto* internal = interface_cast<IMeshPrimitiveInternal>(primitive.get())) {
                internal->set_gpu_displacement(max_displacement);
            }
        }
        arena->update_at(gpu_table_.offset() + 4u * sizeof(uint32_t), &max_displacement, sizeof(float));
    }

    const string_view source = deformer.get_deform_source();
    if (source != string_view(gpu_source_)) {
        gpu_source_ = string(source);
        gpu_deform_pipeline_ = nullptr;
        gpu_commands_dirty_ = true;
    }

    const vec4 params = deformer.get_params();
    arena->update_at(gpu_table_.offset(), &params, sizeof(params));
}

bool DeformingMesh::prepare_gpu_commands(FrameContext& ctx)
{
    if (!ctx.resources || !ctx.backend || !ctx.render_ctx || gpu_source_.empty() || !gpu_table_.valid()) {
        return false;
    }
    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) {
        return false;
    }

    // Everything the commands name must exist before they can run.
    for (const auto& stream : streams_) {
        if (!stream.gpu_rest.valid() || get_gpu_ref(stream.buffer).kind != GpuRef::Kind::Index) {
            return false;
        }
    }
    gpu_refits_.clear();
    for (auto& primitive : primitives_) {
        auto* internal = interface_cast<IMeshPrimitiveInternal>(primitive.get());
        MeshRefitInfo info{};
        if (!internal || !internal->prepare_gpu_refit(*ctx.resources, info)) {
            return false;
        }
        gpu_refits_.push_back(info);
    }

    constexpr uint32_t kCheckInterval = 120;
    if (!gpu_overrun_logged_ && ++gpu_runs_ % kCheckInterval == 0) {
        uint32_t overruns = 0;
        arena->read_at(gpu_table_.offset() + 5u * sizeof(uint32_t), &overruns, sizeof(overruns));
        if (overruns != 0) {
            VELK_LOG(E, "DeformingMesh: vertices moved past the maximum displacement of %f, %u times",
                     double(gpu_max_displacement_), overruns);
            gpu_overrun_logged_ = true;
        }
    }

    if (!gpu_deform_pipeline_) {
        uint64_t key = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < gpu_source_.size(); ++i) {
            key = (key ^ static_cast<uint8_t>(gpu_source_[i])) * 0x100000001b3ULL;
        }
        key |= 0x1000000000000000ULL;
        gpu_deform_pipeline_ = ctx.render_ctx->pipelines().find(
            PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0});
        if (!gpu_deform_pipeline_) {
            string src(mesh_deform_prefix_src);
            src += string_view(gpu_source_);
            src += mesh_deform_main_src;
            gpu_deform_pipeline_ = ctx.render_ctx->pipelines().compile_compute(string_view(src), key);
        }
    }
    if (!gpu_refit_pipeline_) {
        constexpr uint64_t refit_key = 0x1052'6566'6974'4d73ULL;
        gpu_refit_pipeline_ = ctx.render_ctx->pipelines().find(
            PipelineCacheKey{refit_key, PixelFormat::RGBA8, DepthFormat::None, 0});
        if (!gpu_refit_pipeline_) {
            gpu_refit_pipeline_ = ctx.render_ctx->pipelines().compile_compute(mesh_refit_src, refit_key);
        }
    }
    return gpu_deform_pipeline_ && gpu_refit_pipeline_;
}

bool DeformingMesh::take_gpu_commands_dirty()
{
    const bool dirty = gpu_commands_dirty_;
    gpu_commands_dirty_ = false;
    return dirty;
}

void DeformingMesh::record_gpu_clears(IGpuCommandBuffer& cmd, IGpuBuffer* culling_buffer)
{
    for (const auto& info : gpu_refits_) {
        cmd.record_clear_buffer(culling_buffer, uint64_t(info.counter_word) * sizeof(uint32_t),
                                uint64_t(info.node_count) * sizeof(uint32_t));
    }
}

void DeformingMesh::record_gpu_vertices(IGpuCommandBuffer& cmd, vector<IGpuPipeline::Ptr>& held)
{
    held.push_back(gpu_deform_pipeline_);
    for (const auto& stream : streams_) {
        struct DeformPush
        {
            uint32_t rest_word;
            uint32_t vertex_count;
            uint32_t vertex_word;
            uint32_t table_word;
        };
        const DeformPush pc{static_cast<uint32_t>(stream.gpu_rest.offset() / sizeof(uint32_t)), stream.vertex_count,
                            get_gpu_ref(stream.buffer).get_base(),
                            static_cast<uint32_t>(gpu_table_.offset() / sizeof(uint32_t))};
        DispatchCall dc{};
        dc.pipeline = gpu_deform_pipeline_.get();
        dc.groups_x = (stream.vertex_count + 63u) / 64u;
        dc.groups_y = 1;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(pc);
        std::memcpy(dc.root_constants, &pc, sizeof(pc));
        cmd.record_dispatch(dc);
    }
}

void DeformingMesh::record_gpu_refits(IGpuCommandBuffer& cmd, vector<IGpuPipeline::Ptr>& held)
{
    held.push_back(gpu_refit_pipeline_);
    for (const auto& info : gpu_refits_) {
        struct RefitPush
        {
            uint32_t node_base;
            uint32_t word_base;
            uint32_t vertex_base;
            uint32_t leaf_word;
            uint32_t leaf_count;
            uint32_t parent_word;
            uint32_t counter_word;
        };
        const RefitPush pc{info.node_base, info.word_base, info.vertex_base, info.leaf_word,
                           info.leaf_count, info.parent_word, info.counter_word};
        DispatchCall dc{};
        dc.pipeline = gpu_refit_pipeline_.get();
        dc.groups_x = (info.leaf_count + 15u) / 16u;
        dc.groups_y = 1;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(pc);
        std::memcpy(dc.root_constants, &pc, sizeof(pc));
        cmd.record_dispatch(dc);
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
