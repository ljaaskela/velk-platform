#include "mesh/mesh.h"

#include <velk/api/velk.h>
#include <velk-render/gpu_data.h>

#include <cstring>

namespace velk::impl {

namespace {

// Mirrors GLSL `MeshStaticData` in shader_compiler.cpp and the C++
// mirror in velk-ui/src/renderer/scene_collector.h. Defined locally in
// this TU to avoid a velk-render -> velk-ui include cycle; the layout
// is contractual so all three definitions must stay in sync.
VELK_GPU_STRUCT MeshStaticData
{
    uint64_t buffer_addr;
    uint32_t vbo_offset;
    uint32_t ibo_offset;
    uint32_t triangle_count;
    uint32_t vertex_stride;
    uint32_t blas_root;
    uint32_t blas_node_count;
};
static_assert(sizeof(MeshStaticData) == 32, "MeshStaticData layout drift");

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

size_t MeshPrimitive::get_draw_data_size() const
{
    // RT consumes mesh primitives only when they're indexed
    // triangle-list geometry. Anything else returns 0 so the renderer
    // skips the upload entirely (and scene_collector skips the emit).
    if (topology_ != MeshTopology::TriangleList) return 0;
    if (!buffer_ || index_count_ == 0 || vertex_stride_ == 0) return 0;
    size_t total = sizeof(MeshStaticData);
    total += rt_blas_.nodes.size() * sizeof(BlasNode);
    total += rt_blas_.triangle_indices.size() * sizeof(uint32_t);
    return total;
}

ReturnValue MeshPrimitive::write_draw_data(void* out, size_t size,
                                            ::velk::ITextureResolver* /*resolver*/) const
{
    if (size < sizeof(MeshStaticData) || !out) return ReturnValue::Fail;
    if (!buffer_) return ReturnValue::Fail;

    MeshStaticData s{};
    s.buffer_addr   = get_gpu_address(buffer_);
    s.vbo_offset    = 0;  // IBO entries are global vertex indices in our
                          // gltf-imported meshes; vb base = buffer base.
    s.ibo_offset    = static_cast<uint32_t>(buffer_->get_ibo_offset()) + index_offset_;
    s.triangle_count = index_count_ / 3;
    s.vertex_stride = vertex_stride_;
    s.blas_root      = rt_blas_.root_index;
    s.blas_node_count = static_cast<uint32_t>(rt_blas_.nodes.size());

    auto* dst = static_cast<uint8_t*>(out);
    std::memcpy(dst, &s, sizeof(s));
    size_t off = sizeof(s);

    const size_t nodes_bytes = rt_blas_.nodes.size() * sizeof(BlasNode);
    if (nodes_bytes > 0) {
        if (off + nodes_bytes > size) return ReturnValue::Fail;
        std::memcpy(dst + off, rt_blas_.nodes.data(), nodes_bytes);
        off += nodes_bytes;
    }
    const size_t tri_bytes = rt_blas_.triangle_indices.size() * sizeof(uint32_t);
    if (tri_bytes > 0) {
        if (off + tri_bytes > size) return ReturnValue::Fail;
        std::memcpy(dst + off, rt_blas_.triangle_indices.data(), tri_bytes);
    }
    return ReturnValue::Success;
}

void MeshPrimitive::set_rt_blas(BlasBuild blas)
{
    rt_blas_ = std::move(blas);
    // Force the persistent buffer to re-serialise on next get_data_buffer
    // call: dropping the existing buffer makes the next call allocate a
    // fresh one with the new size and content.
    rt_data_buffer_ = nullptr;
}

IBuffer::Ptr MeshPrimitive::get_data_buffer(::velk::ITextureResolver* resolver)
{
    size_t sz = get_draw_data_size();
    if (sz == 0) return nullptr;
    if (!rt_data_buffer_) {
        rt_data_buffer_ = ::velk::instance().create<::velk::IProgramDataBuffer>(
            ::velk::ClassId::ProgramDataBuffer);
        if (!rt_data_buffer_) return nullptr;
    }
    bool ok = true;
    rt_data_buffer_->write(sz, [this, &ok, resolver](void* dst, size_t n) {
        ok = write_draw_data(dst, n, resolver) == ReturnValue::Success;
    });
    return ok ? rt_data_buffer_ : nullptr;
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
