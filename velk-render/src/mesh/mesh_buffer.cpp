#include "mesh/mesh_buffer.h"

#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_backend.h>

#include <cstring>

namespace velk::impl {

namespace {

/// Geometry is bulk and arena growth recopies the whole buffer, so the
/// mesh-word arena starts large enough that a typical scene never grows it.
/// Growth still works (doubling), it just costs a full copy when it happens.
constexpr uint64_t kMeshArenaReserve = uint64_t(32) << 20;  // 32 MiB

/// Word granularity: bases are 32-bit word indices, which is what the RT
/// shader adds to reach a vertex float or an index uint.
constexpr uint32_t kMeshWordSize = 4;

/// Regions are 16-byte aligned, not word aligned. Raster still reads vertices
/// through a buffer_reference block whose address must satisfy the block's own
/// alignment, and a vertex struct containing a vec4 requires 16. Word
/// alignment alone would let a region land mid-vec4 and corrupt those reads.
constexpr uint64_t kMeshRegionAlign = 16;

} // namespace

void MeshBuffer::set_data(const void* vbo_data, size_t vbo_size,
                          const void* ibo_data, size_t ibo_size)
{
    auto& data = mutable_data();
    data.resize(vbo_size + ibo_size);
    if (vbo_size > 0 && vbo_data) {
        std::memcpy(data.data(), vbo_data, vbo_size);
    }
    if (ibo_size > 0 && ibo_data) {
        std::memcpy(data.data() + vbo_size, ibo_data, ibo_size);
    }
    vbo_size_ = vbo_size;
    ibo_size_ = ibo_size;
    set_dirty();
}

bool MeshBuffer::ensure_geometry(IGpuResourceManager& resources)
{
    const uint64_t need = vbo_size_ + ibo_size_;
    if (need == 0) return false;

    auto arena = resources.shared_arena(IRenderBackend::kGlobalMeshWords,
                                        kMeshWordSize, true, kMeshArenaReserve);
    if (!arena) return false;
    arena_ = arena;

    // A size change takes a fresh region; the old one retires on the fence.
    if (region_.size() != need) {
        region_ = arena->alloc(need, kMeshRegionAlign);
        if (!region_.valid()) return false;
    }

    const uint8_t* bytes = get_data();
    if (!bytes) return false;
    arena->write_at(region_.offset(), bytes, need);
    return true;
}

GpuRef MeshBuffer::gpu_ref() const
{
    auto arena = arena_.lock();
    if (!arena || !region_.valid()) return {};
    return GpuRef::from_index(arena->slot(),
                              static_cast<uint32_t>(region_.offset() / kMeshWordSize));
}

} // namespace velk::impl
