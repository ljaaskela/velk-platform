#include "mesh/mesh.h"

#include "mesh/mesh_deform_shaders.h"
#include "mesh/mesh_lod.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk-render/api/material/material.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_gpu_command_buffer.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_backend.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace velk::impl {

namespace {

/// A deforming primitive's tree is built again when refitting raises its cost past
/// this factor.
constexpr float kBlasRebuildDrift = 1.5f;

/// Where a vertex's normal and first uv are, kLodNoAttribute for what it has not.
void attribute_offsets(const vector<VertexAttribute>& attributes, uint32_t& normal_offset, uint32_t& uv_offset)
{
    normal_offset = kLodNoAttribute;
    uv_offset = kLodNoAttribute;
    for (const auto& attribute : attributes) {
        if (attribute.semantic == VertexAttributeSemantic::Normal && attribute.format == VertexAttributeFormat::Float3) {
            normal_offset = attribute.offset;
        }
        if (attribute.semantic == VertexAttributeSemantic::TexCoord0 && attribute.format == VertexAttributeFormat::Float2) {
            uv_offset = attribute.offset;
        }
    }
}

} // namespace

void MeshPrimitive::init(const IMeshBuffer::Ptr& buffer,
                          uint32_t vertex_offset, uint32_t vertex_count,
                          uint32_t index_offset, uint32_t index_count,
                          array_view<VertexAttribute> attributes,
                          uint32_t vertex_stride,
                          MeshTopology topology,
                          const aabb& bounds,
                          const IMeshBuffer::Ptr& uv1_buffer,
                          uint32_t uv1_offset,
                          const IMeshBuffer::Ptr& color_buffer)
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
    color_buffer_ = color_buffer;
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
    rt_surface_region_ = {};
    rt_static_base_ = kInvalidMeshStaticBase;
    release_lod();
}

void MeshPrimitive::release_lod()
{
    if (auto resources = lod_resources_.lock()) {
        for (const LodPage& page : lod_pages_) {
            if (page.loaded) {
                resources->release_geometry(page.bytes);
            }
        }
    }
    lod_resources_ = IGpuResourceManager::WeakPtr{};
    lod_pages_.clear();
    lod_page_clusters_.clear();
    lod_page_made_.clear();
    lod_cluster_link_.clear();
    lod_node_link_.clear();
    lod_records_.clear();
    lod_headers_.clear();
    lod_tree_.clear();
    lod_ = MeshLod{};
    lod_table_region_ = {};
    lod_nodes_region_ = {};
    lod_words_region_ = {};
    lod_marks_region_ = {};
    lod_group_dependents_.clear();
    lod_group_idle_.clear();
    lod_group_order_.clear();
    lod_watching_ = false;
    lod_groups_missing_ = 0;
    lod_dirty_clusters_.clear();
    lod_dirty_nodes_.clear();
    lod_info_ = {};
}

void MeshPrimitive::flush_lod(IGpuResourceManager& resources)
{
    auto culling = resources.shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    auto nodes_arena = resources.shared_arena(IRenderBackend::kGlobalBlasNodes, sizeof(BlasNode));
    auto words_arena = resources.shared_arena(IRenderBackend::kGlobalClusterWords, sizeof(uint32_t));
    if (!culling || !nodes_arena || !words_arena) {
        return;
    }
    const auto once = [](vector<uint32_t>& list) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    };
    once(lod_dirty_clusters_);
    once(lod_dirty_nodes_);
    // Frames on the GPU may be reading these, so the writes take their place in its order.
    const uint64_t header_bytes = kClusterHeaderWords * sizeof(uint32_t);
    for (uint32_t c : lod_dirty_clusters_) {
        culling->update_at(lod_table_region_.offset() + uint64_t(c) * sizeof(MeshLodCluster), &lod_records_[c],
                           sizeof(MeshLodCluster));
        if (lod_words_region_.valid()) {
            words_arena->update_at(lod_words_region_.offset() + uint64_t(c) * header_bytes,
                                   lod_headers_.data() + size_t(c) * kClusterHeaderWords, header_bytes);
        }
    }
    for (uint32_t n : lod_dirty_nodes_) {
        nodes_arena->update_at(lod_nodes_region_.offset() + (uint64_t(n) * 2 + 1) * sizeof(BlasGpuNode), &lod_tree_[n],
                               sizeof(MeshLodNode));
    }
    lod_dirty_clusters_.clear();
    lod_dirty_nodes_.clear();
}

void MeshPrimitive::unload_lod_page(uint32_t group, IGpuResourceManager& resources)
{
    LodPage& page = lod_pages_[group];
    if (!page.loaded) {
        return;
    }
    // Its clusters cannot be chosen any more and have nothing to draw or walk, and the
    // clusters made from it stand in for them wherever their own parents will not do.
    for (uint32_t c : lod_page_clusters_[group]) {
        MeshLodCluster& record = lod_records_[c];
        record.index_word = 0;
        record.triangle_count = 0;
        record.vertex_word = 0;
        record.uv1_word = kLodNoAttribute;
        record.color_word = kLodNoAttribute;
        if (!lod_headers_.empty()) {
            uint32_t* header = lod_headers_.data() + size_t(c) * kClusterHeaderWords;
            header[10] = kBlasLeafRef;
            header[12] = header[13] = 0;
            header[14] = lod_empty_leaf_word_;
        }
        set_lod_errors(c, record.error, 0.f);
    }
    for (uint32_t made : lod_page_made_[group]) {
        set_lod_errors(made, 0.f, lod_records_[made].parent_error);
        // Those are about to be used, so their own group is not to go next for having sat
        // unused behind this one.
        const uint32_t stands_in = lod_.cluster_group[made];
        if (stands_in != kLodNoGroup) {
            lod_group_idle_[stands_in] = 0;
        }
    }
    // The arenas keep freed regions until the frames that may still read them are done.
    resources.release_geometry(page.bytes);
    page = LodPage{};
    ++lod_groups_missing_;
}

void MeshPrimitive::update_lod(IGpuResourceManager& resources)
{
    if (lod_.group_count == 0 || !lod_marks_region_.valid()) {
        return;
    }
    // With everything loaded and room to spare there is nothing to ask for or to drop.
    const bool pressure = resources.geometry_pressure();
    if (lod_groups_missing_ == 0 && !pressure) {
        lod_watching_ = false;
        return;
    }
    // Reading the arena back is slow, and a group that is wanted stays wanted, so the marks
    // are looked at every few frames.
    constexpr uint32_t kFramesBetween = 4;
    if (++lod_update_count_ % kFramesBetween != 0) {
        return;
    }
    auto culling = resources.shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!culling) {
        return;
    }
    vector<uint32_t> marks;
    marks.resize(lod_.group_count, 0u);
    const uint64_t marks_bytes = marks.size() * sizeof(uint32_t);
    culling->read_at(lod_marks_region_.offset(), marks.data(), marks_bytes);

    // How long each group has gone unwanted is counted from when detail got short of room:
    // before that the marks were not looked at, and say nothing of when.
    if (pressure && !lod_watching_) {
        std::fill(lod_group_idle_.begin(), lod_group_idle_.end(), 0u);
    }
    lod_watching_ = pressure;

    bool any = false;
    bool loaded = false;
    for (uint32_t g = 0; g < lod_.group_count; ++g) {
        if (marks[g] == 0u) {
            lod_group_idle_[g] += pressure ? 1u : 0u;
            continue;
        }
        any = true;
        lod_group_idle_[g] = 0;
        if (lod_pages_[g].loaded) {
            continue;
        }
        // A mark comes from a cluster made from the group, so those are loaded; checked all
        // the same, since loading without them would leave holes.
        bool ready = true;
        for (uint32_t made : lod_page_made_[g]) {
            const uint32_t needs = lod_.cluster_group[made];
            ready = ready && (needs == kLodNoGroup || lod_pages_[needs].loaded);
        }
        if (ready && load_lod_page(g, resources, false, true)) {
            loaded = true;
            --lod_groups_missing_;
        }
    }

    // Short of room: what nothing has wanted for long enough goes, from the finest, each
    // once nothing loaded needs it.
    if (pressure) {
        const uint32_t looks = std::max(resources.geometry_idle_frames() / kFramesBetween, 1u);
        for (size_t i = lod_group_order_.size(); i-- > 0;) {
            const uint32_t g = lod_group_order_[i];
            if (!lod_pages_[g].loaded || lod_group_idle_[g] < looks) {
                continue;
            }
            bool needed = false;
            for (uint32_t dependent : lod_group_dependents_[g]) {
                needed = needed || lod_pages_[dependent].loaded;
            }
            if (!needed) {
                unload_lod_page(g, resources);
                loaded = true;
            }
        }
    }
    if (loaded) {
        flush_lod(resources);
    }
    if (any) {
        std::fill(marks.begin(), marks.end(), 0u);
        culling->update_at(lod_marks_region_.offset(), marks.data(), marks_bytes);
    }
}

void MeshPrimitive::set_lod_errors(uint32_t c, float error, float parent_error)
{
    lod_records_[c].error = error;
    lod_records_[c].parent_error = parent_error;
    lod_dirty_clusters_.push_back(c);
    if (!lod_headers_.empty()) {
        uint32_t* header = lod_headers_.data() + size_t(c) * kClusterHeaderWords;
        std::memcpy(header + 4, &error, sizeof(float));
        std::memcpy(header + 9, &parent_error, sizeof(float));
    }

    // Up the tree: each node says of a child the least error and the largest parent error
    // under it, which is what lets a walk skip it.
    uint32_t link = lod_cluster_link_[c];
    if (link == kLodNoGroup) {
        return;
    }
    float least = error;
    float largest = parent_error;
    for (;;) {
        const uint32_t n = link >> 1;
        MeshLodNode::Child& child = lod_tree_[n].children[link & 1u];
        if (child.min_error == least && child.max_parent_error == largest) {
            return;
        }
        child.min_error = least;
        child.max_parent_error = largest;
        lod_dirty_nodes_.push_back(n);
        link = lod_node_link_[n];
        if (link == kLodNoGroup) {
            return;
        }
        least = std::numeric_limits<float>::max();
        largest = 0.f;
        for (const MeshLodNode::Child& of : lod_tree_[n].children) {
            if (of.ref != kLodNoChild) {
                least = std::min(least, of.min_error);
                largest = std::max(largest, of.max_parent_error);
            }
        }
    }
}

bool MeshPrimitive::load_lod_page(uint32_t page_index, IGpuResourceManager& resources, bool always, bool paced)
{
    LodPage& page = lod_pages_[page_index];
    if (page.loaded) {
        return true;
    }
    const vector<uint32_t>& clusters = lod_page_clusters_[page_index];
    const bool rays = lod_words_region_.valid();
    const bool is_group = page_index < lod_.group_count;

    uint64_t geometry_bytes = 0;
    uint64_t node_count = 0;
    uint64_t word_count = 0;
    for (uint32_t c : clusters) {
        const MeshLodCluster& source = lod_.clusters[c];
        geometry_bytes += uint64_t(source.vertex_count) * vertex_stride_ + uint64_t(source.triangle_count) * 3u * sizeof(uint32_t);
        if (source.uv1_word != kLodNoAttribute) {
            geometry_bytes += uint64_t(source.vertex_count) * 2u * sizeof(float);
        }
        if (source.color_word != kLodNoAttribute) {
            geometry_bytes += uint64_t(source.vertex_count) * sizeof(uint32_t);
        }
        if (rays) {
            node_count += lod_.blocks[c].node_count;
            word_count += lod_.blocks[c].word_count;
        }
    }
    const uint64_t bytes = geometry_bytes + node_count * sizeof(BlasGpuNode) + word_count * sizeof(uint32_t);
    if (!resources.reserve_geometry(bytes, always)) {
        return false;
    }
    if (paced && !resources.take_geometry_frame(bytes)) {
        resources.release_geometry(bytes);
        return false;
    }

    auto mesh_words = resources.shared_arena(IRenderBackend::kGlobalMeshWords, sizeof(uint32_t), true);
    auto nodes_arena = resources.shared_arena(IRenderBackend::kGlobalBlasNodes, sizeof(BlasNode));
    auto words_arena = resources.shared_arena(IRenderBackend::kGlobalClusterWords, sizeof(uint32_t));
    if (mesh_words && geometry_bytes != 0) {
        page.geometry = mesh_words->alloc(geometry_bytes);
    }
    if (nodes_arena && node_count != 0) {
        page.nodes = nodes_arena->alloc(node_count * sizeof(BlasGpuNode), sizeof(BlasGpuNode));
    }
    if (words_arena && word_count != 0) {
        page.words = words_arena->alloc(word_count * sizeof(uint32_t));
    }
    if ((geometry_bytes != 0 && !page.geometry.valid()) || (node_count != 0 && !page.nodes.valid())
        || (word_count != 0 && !page.words.valid())) {
        page = LodPage{};
        resources.release_geometry(bytes);
        return false;
    }

    // Its clusters' data, one after another, and in each one's record and header where it went.
    vector<uint8_t> geometry;
    vector<BlasGpuNode> nodes;
    vector<uint32_t> words;
    geometry.reserve(geometry_bytes);
    nodes.reserve(node_count);
    words.reserve(word_count);
    const auto geometry_word = static_cast<uint32_t>(page.geometry.offset() / sizeof(uint32_t));
    const auto node_base = static_cast<uint32_t>(page.nodes.offset() / sizeof(BlasGpuNode));
    const auto word_base = static_cast<uint32_t>(page.words.offset() / sizeof(uint32_t));
    const auto append = [&geometry](const void* from, size_t size) {
        const auto* first = static_cast<const uint8_t*>(from);
        geometry.insert(geometry.end(), first, first + size);
    };
    for (uint32_t c : clusters) {
        const MeshLodCluster& source = lod_.clusters[c];
        MeshLodCluster& record = lod_records_[c];
        lod_dirty_clusters_.push_back(c);
        if (deforming_) {
            record.vertex_word = lod_live_vertex_word_;
            record.uv1_word = lod_live_uv1_word_;
            record.color_word = lod_live_color_word_;
        } else {
            record.vertex_word = geometry_word + static_cast<uint32_t>(geometry.size() / sizeof(uint32_t));
            append(lod_.vertices.data() + size_t(source.vertex_word) * sizeof(uint32_t), size_t(source.vertex_count) * vertex_stride_);
            if (source.uv1_word != kLodNoAttribute) {
                record.uv1_word = geometry_word + static_cast<uint32_t>(geometry.size() / sizeof(uint32_t));
                append(lod_.uv1.data() + source.uv1_word, size_t(source.vertex_count) * 2u * sizeof(float));
            }
            if (source.color_word != kLodNoAttribute) {
                record.color_word = geometry_word + static_cast<uint32_t>(geometry.size() / sizeof(uint32_t));
                append(lod_.colors.data() + source.color_word, size_t(source.vertex_count) * sizeof(uint32_t));
            }
        }
        record.index_word = geometry_word + static_cast<uint32_t>(geometry.size() / sizeof(uint32_t));
        record.triangle_count = source.triangle_count;
        append(lod_.indices.data() + source.index_word, size_t(source.triangle_count) * 3u * sizeof(uint32_t));

        if (rays) {
            const MeshLod::Block& block = lod_.blocks[c];
            uint32_t* header = lod_headers_.data() + size_t(c) * kClusterHeaderWords;
            header[10] = lod_.ray_words[size_t(c) * kClusterHeaderWords + 10u];
            header[12] = record.vertex_word;
            header[13] = node_base + static_cast<uint32_t>(nodes.size());
            header[14] = word_base + static_cast<uint32_t>(words.size());
            nodes.insert(nodes.end(), lod_.ray_nodes.begin() + block.node_first,
                         lod_.ray_nodes.begin() + block.node_first + block.node_count);
            words.insert(words.end(), lod_.ray_words.begin() + block.word_first,
                         lod_.ray_words.begin() + block.word_first + block.word_count);
        }
    }
    if (!geometry.empty()) {
        mesh_words->write_at(page.geometry.offset(), geometry.data(), geometry.size());
    }
    if (!nodes.empty()) {
        nodes_arena->write_at(page.nodes.offset(), nodes.data(), nodes.size() * sizeof(BlasGpuNode));
    }
    if (!words.empty()) {
        words_arena->write_at(page.words.offset(), words.data(), words.size() * sizeof(uint32_t));
    }

    // Its clusters can be chosen again where their parent will not do, and the clusters made
    // from it give way to them where their own error is too large.
    if (is_group) {
        for (uint32_t c : clusters) {
            set_lod_errors(c, lod_records_[c].error, lod_.clusters[c].parent_error);
        }
        for (uint32_t made : lod_page_made_[page_index]) {
            set_lod_errors(made, lod_.clusters[made].error, lod_records_[made].parent_error);
        }
    }
    page.bytes = bytes;
    page.loaded = true;
    return true;
}

bool MeshPrimitive::read_lod_input()
{
    auto* buffer = interface_cast<IMeshBufferInternal>(buffer_.get());
    lod_geometry_.clear();
    lod_uv1_bytes_.clear();
    lod_color_bytes_.clear();
    bool read = buffer && buffer->read_geometry(lod_geometry_);
    // The second uv is a stream of its own, by vertex, so clusters need runs of it too.
    if (read && uv1_buffer_) {
        auto* uv1_buffer = interface_cast<IMeshBufferInternal>(uv1_buffer_.get());
        const size_t needed = size_t(uv1_offset_) + buffer_->get_vbo_size() / vertex_stride_ * sizeof(float) * 2;
        if (!uv1_buffer || !uv1_buffer->read_geometry(lod_uv1_bytes_) || lod_uv1_bytes_.size() < needed) {
            read = false;
        }
    }
    // Colours likewise, one word for each vertex of the buffer.
    if (read && color_buffer_) {
        auto* color_buffer = interface_cast<IMeshBufferInternal>(color_buffer_.get());
        const size_t needed = buffer_->get_vbo_size() / vertex_stride_ * sizeof(uint32_t);
        if (!color_buffer || !color_buffer->read_geometry(lod_color_bytes_) || lod_color_bytes_.size() < needed) {
            read = false;
        }
    }
    lod_read_ = read;
    // A deforming mesh's clusters name its live vertices, which must be on the GPU by now.
    // Any other mesh's get copies of their own, which takes reading them.
    lod_live_vertex_word_ = 0;
    lod_live_uv1_word_ = kLodNoAttribute;
    lod_live_color_word_ = kLodNoAttribute;
    if (deforming_) {
        const GpuRef live = get_gpu_ref(buffer_);
        if (live.kind != GpuRef::Kind::Index) {
            return false;
        }
        lod_live_vertex_word_ = live.get_base();
        if (uv1_buffer_) {
            const GpuRef live_uv1 = get_gpu_ref(uv1_buffer_);
            if (live_uv1.kind != GpuRef::Kind::Index) {
                return false;
            }
            lod_live_uv1_word_ = live_uv1.get_base() + uv1_offset_ / 4u;
        }
        if (color_buffer_) {
            const GpuRef live_color = get_gpu_ref(color_buffer_);
            if (live_color.kind != GpuRef::Kind::Index) {
                return false;
            }
            lod_live_color_word_ = live_color.get_base();
        }
    } else if (!read) {
        if (!lod_unreadable_) {
            VELK_LOG(E, "MeshPrimitive: could not read the mesh's vertices, so it cannot be drawn by cluster or hit by rays");
            lod_unreadable_ = true;
        }
        return false;
    }
    return true;
}

void MeshPrimitive::build_lod()
{
    // Readied for another thread: its input read there too, as reading geometry back from
    // GPU-mapped memory is slow and runs in parallel this way. Nothing grows the arenas
    // meanwhile, as the main thread waits for the builds.
    if (lod_readied_) {
        lod_readied_ = false;
        if (!read_lod_input()) {
            return;
        }
    }
    uint32_t normal_offset;
    uint32_t uv_offset;
    attribute_offsets(attributes_, normal_offset, uv_offset);
    const float* uv1 = lod_read_ && uv1_buffer_ ? reinterpret_cast<const float*>(lod_uv1_bytes_.data() + uv1_offset_) : nullptr;
    const uint32_t* colors = lod_read_ && color_buffer_ ? reinterpret_cast<const uint32_t*>(lod_color_bytes_.data()) : nullptr;
    // A deforming mesh's coarser levels would be drawn in its rest pose, and its rays walk
    // the tree that is refit, not the hierarchy.
    lod_ = build_mesh_lod(rt_blas_, lod_read_ ? lod_geometry_.data() : nullptr, uv1, colors, vertex_stride_, normal_offset,
                          deforming_);
    lod_geometry_ = vector<uint8_t>{};
    lod_uv1_bytes_ = vector<uint8_t>{};
    lod_color_bytes_ = vector<uint8_t>{};
    lod_built_ = true;
}

bool MeshPrimitive::begin_lod_build()
{
    // Once: a primitive several batches draw is readied by the first.
    if (lod_table_region_.valid() || lod_built_ || lod_readied_ || deforming_ || rt_blas_.clusters.empty()) {
        return false;
    }
    lod_readied_ = true;
    return true;
}

bool MeshPrimitive::ensure_lod_data(IGpuResourceManager& resources, MeshLodInfo& out)
{
    if (!lod_table_region_.valid()) {
        // A single cluster has nothing to choose between, but takes the same path, so that
        // what is drawn is chosen the same way for every mesh.
        if (rt_blas_.clusters.empty()) {
            return false;
        }
        auto culling = resources.shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
        auto mesh_words = resources.shared_arena(IRenderBackend::kGlobalMeshWords, sizeof(uint32_t), true);
        if (!culling || !mesh_words) {
            return false;
        }
        // Built already where it was readied for another thread (begin_lod_build).
        if (!lod_built_) {
            if (!read_lod_input()) {
                return false;
            }
            build_lod();
        }
        lod_built_ = false;
        auto nodes_arena = resources.shared_arena(IRenderBackend::kGlobalBlasNodes, sizeof(BlasNode));
        auto words_arena = resources.shared_arena(IRenderBackend::kGlobalClusterWords, sizeof(uint32_t));
        if (!nodes_arena || !words_arena || lod_.clusters.empty() || lod_.nodes.empty()) {
            release_lod();
            return false;
        }
        const bool rays = !lod_.ray_words.empty();
        const size_t cluster_count = lod_.clusters.size();
        const size_t upper = lod_.nodes.size();
        const uint32_t group_count = lod_.group_count;

        // Always there: every cluster's record and header, and the tree over the clusters.
        const size_t clusters_size = cluster_count * sizeof(MeshLodCluster);
        const size_t nodes_size = upper * 2 * sizeof(BlasGpuNode);
        // After the headers, a leaf without triangles, which a cluster that is not loaded has
        // for a tree: should one ever be walked, the walk finds nothing there and goes on.
        const size_t headers_size = (cluster_count * kClusterHeaderWords + 4) * sizeof(uint32_t);
        lod_table_region_ = culling->alloc(clusters_size);
        lod_nodes_region_ = nodes_arena->alloc(nodes_size, sizeof(BlasGpuNode));
        if (rays) {
            // A reference to a cluster's header holds its word over four.
            lod_words_region_ = words_arena->alloc(headers_size, sizeof(uint32_t) * 4);
        }
        if (group_count != 0) {
            lod_marks_region_ = culling->alloc(size_t(group_count) * sizeof(uint32_t));
        }
        if (!lod_table_region_.valid() || !lod_nodes_region_.valid() || (rays && !lod_words_region_.valid())
            || (group_count != 0 && !lod_marks_region_.valid())) {
            release_lod();
            return false;
        }
        lod_resources_ = ::velk::get_self<IGpuResourceManager>(&resources);
        const auto marks_word = static_cast<uint32_t>(lod_marks_region_.offset() / sizeof(uint32_t));
        if (group_count != 0) {
            vector<uint32_t> no_marks;
            no_marks.resize(group_count, 0u);
            culling->write_at(lod_marks_region_.offset(), no_marks.data(), no_marks.size() * sizeof(uint32_t));
        }

        // Which clusters each group holds and which were made from it, and where each cluster
        // and node hangs in the tree.
        lod_page_clusters_.resize(size_t(group_count) + 1);
        lod_page_made_.resize(group_count);
        lod_pages_.resize(size_t(group_count) + 1);
        for (uint32_t c = 0; c < cluster_count; ++c) {
            const uint32_t group = lod_.cluster_group[c];
            lod_page_clusters_[group == kLodNoGroup ? group_count : group].push_back(c);
            if (lod_.cluster_made_from[c] != kLodNoGroup) {
                lod_page_made_[lod_.cluster_made_from[c]].push_back(c);
            }
        }
        lod_cluster_link_.resize(cluster_count, kLodNoGroup);
        lod_node_link_.resize(upper, kLodNoGroup);
        for (uint32_t n = 0; n < upper; ++n) {
            for (uint32_t k = 0; k < 2; ++k) {
                const uint32_t ref = lod_.nodes[n].children[k].ref;
                if (ref == kLodNoChild) {
                    continue;
                }
                ((ref & kLodLeaf) != 0u ? lod_cluster_link_[ref & ~kLodLeaf] : lod_node_link_[ref]) = n * 2u + k;
            }
        }

        // With nothing loaded, no cluster of a group can be chosen, and every cluster made
        // from a group is chosen wherever its own parent will not do.
        lod_records_.assign(lod_.clusters.begin(), lod_.clusters.end());
        lod_tree_.assign(lod_.nodes.begin(), lod_.nodes.end());
        if (rays) {
            lod_headers_.assign(lod_.ray_words.begin(), lod_.ray_words.begin() + cluster_count * kClusterHeaderWords);
            lod_headers_.resize(cluster_count * kClusterHeaderWords + 4, 0u);
        }
        const auto empty_leaf_word = static_cast<uint32_t>(lod_words_region_.offset() / sizeof(uint32_t)
                                                           + cluster_count * kClusterHeaderWords);
        lod_empty_leaf_word_ = empty_leaf_word;
        for (uint32_t c = 0; c < cluster_count; ++c) {
            // No triangles either, so that one drawn by mistake draws nothing.
            MeshLodCluster& record = lod_records_[c];
            record.index_word = 0;
            record.triangle_count = 0;
            record.vertex_word = 0;
            record.uv1_word = kLodNoAttribute;
            record.color_word = kLodNoAttribute;
            // Using it where its error as built is too large asks for the group it was made from.
            const uint32_t made_from = lod_.cluster_made_from[c];
            record.full_error = lod_.clusters[c].error;
            record.mark_word = made_from != kLodNoGroup ? marks_word + made_from : kLodNoAttribute;
            // Using it at all says its own group is wanted.
            const uint32_t group = lod_.cluster_group[c];
            record.own_mark_word = group != kLodNoGroup ? marks_word + group : kLodNoAttribute;
            if (rays) {
                uint32_t* header = lod_headers_.data() + size_t(c) * kClusterHeaderWords;
                header[10] = kBlasLeafRef;
                header[12] = header[13] = 0;
                header[14] = empty_leaf_word;
                std::memcpy(header + 15, &record.full_error, sizeof(float));
                header[16] = record.mark_word;
                header[17] = record.own_mark_word;
            }
            set_lod_errors(c, lod_.cluster_made_from[c] != kLodNoGroup ? 0.f : lod_.clusters[c].error,
                           lod_.cluster_group[c] != kLodNoGroup ? 0.f : lod_.clusters[c].parent_error);
        }

        // What nothing replaces is always loaded. Then groups from the coarsest, while the
        // budget has room, each once the groups of the clusters made from it are there.
        if (!load_lod_page(group_count, resources, true, false)) {
            release_lod();
            return false;
        }
        // A group needs the groups its clusters' stand-ins are in, which then cannot go first.
        lod_group_dependents_.resize(group_count);
        lod_group_idle_.resize(group_count, 0u);
        for (uint32_t g = 0; g < group_count; ++g) {
            for (uint32_t made : lod_page_made_[g]) {
                const uint32_t needs = lod_.cluster_group[made];
                if (needs != kLodNoGroup) {
                    vector<uint32_t>& dependents = lod_group_dependents_[needs];
                    if (dependents.empty() || dependents.back() != g) {
                        dependents.push_back(g);
                    }
                }
            }
        }
        vector<uint32_t>& order = lod_group_order_;
        order.reserve(group_count);
        for (uint32_t g = 0; g < group_count; ++g) {
            order.push_back(g);
        }
        const auto level_of = [this](uint32_t g) { return lod_.clusters[lod_page_clusters_[g].front()].level; };
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return level_of(a) > level_of(b); });
        for (uint32_t g : order) {
            bool ready = true;
            for (uint32_t made : lod_page_made_[g]) {
                const uint32_t needs = lod_.cluster_group[made];
                ready = ready && (needs == kLodNoGroup || lod_pages_[needs].loaded);
            }
            if (!ready || !load_lod_page(g, resources, false, false)) {
                ++lod_groups_missing_;
            }
        }
        // Everything is written whole below, so nothing is left to bring up to date.
        lod_dirty_clusters_.clear();
        lod_dirty_nodes_.clear();

        // The tree: what rays read of a node, with its references by where they lead in the
        // arenas, then what says which children to skip.
        const auto node_base = static_cast<uint32_t>(lod_nodes_region_.offset() / sizeof(BlasGpuNode));
        const auto header_base = static_cast<uint32_t>(lod_words_region_.offset() / sizeof(uint32_t));
        vector<BlasGpuNode> tree;
        tree.resize(upper * 2);
        for (size_t n = 0; n < upper; ++n) {
            BlasGpuNode& node = tree[n * 2];
            node = lod_.ray_nodes[n * 2];
            if (rays) {
                for (uint32_t* ref : {&node.left, &node.right}) {
                    if (*ref != kBlasNoChild) {
                        *ref += (*ref & kBlasClusterRef) != 0u ? header_base / 4u : node_base;
                    }
                }
            }
            std::memcpy(&tree[n * 2 + 1], &lod_tree_[n], sizeof(MeshLodNode));
        }
        culling->write_at(lod_table_region_.offset(), lod_records_.data(), clusters_size);
        nodes_arena->write_at(lod_nodes_region_.offset(), tree.data(), nodes_size);
        if (rays) {
            words_arena->write_at(lod_words_region_.offset(), lod_headers_.data(), headers_size);
        }

        if (group_count != 0) {
            resources.track_geometry(::velk::get_self<IMeshPrimitiveInternal>(this));
        }

        lod_info_.cluster_word = static_cast<uint32_t>(lod_table_region_.offset() / sizeof(uint32_t));
        lod_info_.cluster_count = static_cast<uint32_t>(cluster_count);
        lod_info_.level0_clusters = lod_.level0_clusters;
        // The tree's nodes are every second one, after the one rays read first.
        lod_info_.node_word = static_cast<uint32_t>((lod_nodes_region_.offset() + sizeof(BlasGpuNode)) / sizeof(uint32_t));
        lod_info_.ray_word = rays ? header_base : 0xFFFFFFFFu;
        lod_info_.deforming = deforming_;
    }
    out = lod_info_;
    return true;
}

uint32_t MeshPrimitive::ensure_rt_data(IGpuResourceManager& resources)
{
    if (rt_static_region_.valid()) return rt_static_base_;

    // RT consumes mesh primitives only when they're indexed triangle-list
    // geometry with a BLAS. Anything else is skipped entirely.
    if (topology_ != MeshTopology::TriangleList) return kInvalidMeshStaticBase;
    if (!buffer_ || index_count_ == 0 || vertex_stride_ == 0) return kInvalidMeshStaticBase;
    ensure_blas();
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
    if (deforming_) {
        if (!publish_tree(resources, s)) return kInvalidMeshStaticBase;
    } else {
        // Rays walk the level of detail hierarchy, from the tree over its clusters.
        MeshLodInfo lod;
        if (!ensure_lod_data(resources, lod) || !lod_words_region_.valid()) return kInvalidMeshStaticBase;
        fill_static(s);
        // The tree's first node, by where it is: each cluster's header says where the rest is.
        s.blas_root = kBlasUpperRef | static_cast<uint32_t>(lod_nodes_region_.offset() / sizeof(BlasGpuNode));
        s.blas_node_count = static_cast<uint32_t>(lod_nodes_region_.size() / sizeof(BlasGpuNode));
    }
    static_arena->write_at(static_region.offset(), &s, sizeof(s));

    rt_static_base_ = static_cast<uint32_t>(
        static_region.offset() / sizeof(MeshStaticData));
    rt_static_region_ = std::move(static_region);
    return rt_static_base_;
}

void MeshPrimitive::ensure_blas() const
{
    if (!rt_blas_.nodes.empty() || blas_build_tried_) return;
    blas_build_tried_ = true;
    if (topology_ != MeshTopology::TriangleList || !buffer_ || index_count_ == 0 || vertex_stride_ == 0) return;

    // The BLAS builder reads positions from the start of each vertex.
    bool positions_first = false;
    for (const auto& attribute : attributes_) {
        positions_first = positions_first
            || (attribute.semantic == VertexAttributeSemantic::Position && attribute.format == VertexAttributeFormat::Float3 && attribute.offset == 0);
    }
    auto* buffer = interface_cast<IMeshBufferInternal>(buffer_.get());
    vector<uint8_t> bytes;
    if (!positions_first || !buffer || !buffer->read_geometry(bytes)) return;

    const auto* vertices = reinterpret_cast<const float*>(bytes.data());
    const auto* indices = reinterpret_cast<const uint32_t*>(bytes.data() + buffer_->get_ibo_offset() + index_offset_);
    rt_blas_ = build_mesh_blas(vertices, vertex_stride_, static_cast<uint32_t>(buffer_->get_vbo_size() / vertex_stride_), indices, index_count_ / 3);
}

vector<uint32_t> MeshPrimitive::surface_table(const uint8_t* vertices) const
{
    vector<uint8_t> geometry;
    if (!vertices) {
        auto* buffer = interface_cast<IMeshBufferInternal>(buffer_.get());
        if (buffer && buffer->read_geometry(geometry)) {
            vertices = geometry.data();
        }
    }
    uint32_t normal_offset;
    uint32_t uv_offset;
    attribute_offsets(attributes_, normal_offset, uv_offset);
    return blas_surface_table(rt_blas_, vertices, vertex_stride_, normal_offset, uv_offset);
}

void MeshPrimitive::fill_static(MeshStaticData& out)
{
    // A base is a word index into the shared mesh-word arena. IBO entries are
    // global vertex indices in our gltf-imported meshes, so the vertex run starts
    // at the region base itself.
    out.vbo_base = get_gpu_ref(buffer_).get_base();
    out.flags = deforming_ ? kMeshStaticDeforming : 0u;
    if (auto state = read_state<IMeshPrimitive>(this); state && state->material) {
        Material material(state->material.get<IObject>());
        if (material && material.has_options() && material.options().get_double_sided()) {
            out.flags |= kMeshStaticDoubleSided;
        }
    }
    out.triangle_count = index_count_ / 3;
    out.vertex_stride = vertex_stride_;
}

bool MeshPrimitive::publish_tree(IGpuResourceManager& resources, MeshStaticData& out, const uint8_t* vertices)
{
    auto nodes_arena = resources.shared_arena(IRenderBackend::kGlobalBlasNodes, sizeof(BlasNode));
    auto words_arena = resources.shared_arena(IRenderBackend::kGlobalClusterWords, sizeof(uint32_t));
    if (!nodes_arena || !words_arena) return false;

    const BlasGpuTree tree = blas_gpu_tree(rt_blas_);
    const vector<uint32_t> surfaces = surface_table(vertices);
    const uint64_t nodes_bytes = tree.nodes.size() * sizeof(BlasGpuNode);
    const uint64_t words_bytes = tree.words.size() * sizeof(uint32_t);
    const uint64_t surface_bytes = surfaces.size() * sizeof(uint32_t);
    auto nodes_region = nodes_arena->alloc(std::max<uint64_t>(nodes_bytes, sizeof(BlasGpuNode)), sizeof(BlasGpuNode));
    auto words_region = words_arena->alloc(words_bytes);
    auto surface_region = words_arena->alloc(surface_bytes);
    if (!nodes_region.valid() || !words_region.valid() || !surface_region.valid()) return false;

    if (nodes_bytes != 0) {
        nodes_arena->write_at(nodes_region.offset(), tree.nodes.data(), nodes_bytes);
    }
    words_arena->write_at(words_region.offset(), tree.words.data(), words_bytes);
    words_arena->write_at(surface_region.offset(), surfaces.data(), surface_bytes);

    fill_static(out);
    out.blas_root = tree.root_ref;
    out.blas_node_count = static_cast<uint32_t>(rt_blas_.nodes.size());
    out.blas_node_base = static_cast<uint32_t>(nodes_region.offset() / sizeof(BlasGpuNode));
    out.cluster_word_base = static_cast<uint32_t>(words_region.offset() / sizeof(uint32_t));
    out.surface_word_base = static_cast<uint32_t>(surface_region.offset() / sizeof(uint32_t));

    rt_blas_nodes_region_ = std::move(nodes_region);
    rt_cluster_words_region_ = std::move(words_region);
    rt_surface_region_ = std::move(surface_region);
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
    // The surface table follows the tree's shape, which only a rebuild changes; deforming
    // otherwise does not matter at the sizes it is read for.
    bool rebuilt = false;
    if (blas_tree_cost(rt_blas_) > rt_built_cost_ * kBlasRebuildDrift) {
        VELK_PERF_SCOPE("mesh.blas_rebuild");
        rt_blas_ = build_mesh_blas(floats, vertex_stride_, static_cast<uint32_t>(vertices.size()),
                                   indices.begin() + index_offset_ / sizeof(uint32_t), index_count_ / 3);
        rt_built_cost_ = blas_tree_cost(rt_blas_);
        rebuilt = true;
    }
    const auto* vertex_bytes = reinterpret_cast<const uint8_t*>(vertices.begin());

    const BlasNode& root = rt_blas_.nodes[rt_blas_.root_index];
    bounds_.position = {root.aabb_min[0], root.aabb_min[1], root.aabb_min[2]};
    bounds_.extent = {root.aabb_max[0] - root.aabb_min[0], root.aabb_max[1] - root.aabb_min[1],
                      root.aabb_max[2] - root.aabb_min[2]};

    if (!rt_static_region_.valid()) return;

    const BlasGpuTree tree = blas_gpu_tree(rt_blas_);
    const uint64_t nodes_bytes = std::max<uint64_t>(tree.nodes.size() * sizeof(BlasGpuNode), sizeof(BlasGpuNode));
    const uint64_t words_bytes = tree.words.size() * sizeof(uint32_t);
    if (nodes_bytes == rt_blas_nodes_region_.size() && words_bytes == rt_cluster_words_region_.size()) {
        auto nodes_arena = resources.shared_arena(IRenderBackend::kGlobalBlasNodes, sizeof(BlasNode));
        auto words_arena = resources.shared_arena(IRenderBackend::kGlobalClusterWords, sizeof(uint32_t));
        if (nodes_arena && words_arena) {
            if (!tree.nodes.empty()) {
                nodes_arena->update_at(rt_blas_nodes_region_.offset(), tree.nodes.data(), tree.nodes.size() * sizeof(BlasGpuNode));
            }
            words_arena->update_at(rt_cluster_words_region_.offset(), tree.words.data(), words_bytes);
            if (rebuilt) {
                const vector<uint32_t> surfaces = surface_table(vertex_bytes);
                if (surfaces.size() * sizeof(uint32_t) == rt_surface_region_.size()) {
                    words_arena->update_at(rt_surface_region_.offset(), surfaces.data(), rt_surface_region_.size());
                }
            }
        }
        return;
    }

    // A new tree of another size takes new regions. The old ones retire on the fence,
    // and the record pointing at them keeps its base.
    MeshStaticData s{};
    auto static_arena = resources.shared_arena(IRenderBackend::kGlobalMeshStatic, sizeof(MeshStaticData));
    if (static_arena && publish_tree(resources, s, vertex_bytes)) {
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
            primitive->get_uv1_buffer(), primitive->get_uv1_offset(), primitive->get_color_buffer());
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
        const BlasGpuTree tree = blas_gpu_tree(rt_blas_);
        const auto node_count = static_cast<uint32_t>(tree.nodes.size());
        const auto leaf_count = static_cast<uint32_t>(tree.leaf_links.size() / 2u);
        const uint32_t leaf_words = leaf_count * 2u;
        gpu_refit_region_ = arena->alloc(
            (uint64_t(leaf_words) + uint64_t(node_count) * 2u) * sizeof(uint32_t));
        if (!gpu_refit_region_.valid()) {
            VELK_LOG(E, "MeshPrimitive: no room for a refit on the GPU");
            return false;
        }
        const uint64_t offset = gpu_refit_region_.offset();
        arena->write_at(offset, tree.leaf_links.data(), leaf_words * sizeof(uint32_t));
        if (node_count != 0) {
            arena->write_at(offset + leaf_words * sizeof(uint32_t), tree.parent_links.data(), node_count * sizeof(uint32_t));
        }

        const auto first_word = static_cast<uint32_t>(offset / sizeof(uint32_t));
        gpu_refit_.node_base = static_cast<uint32_t>(rt_blas_nodes_region_.offset() / sizeof(BlasGpuNode));
        gpu_refit_.word_base = static_cast<uint32_t>(rt_cluster_words_region_.offset() / sizeof(uint32_t));
        gpu_refit_.vertex_base = get_gpu_ref(buffer_).get_base();
        gpu_refit_.leaf_word = first_word;
        gpu_refit_.leaf_count = leaf_count;
        gpu_refit_.parent_word = first_word + leaf_words;
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
