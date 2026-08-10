#ifndef VELK_RENDER_MESH_BUFFER_H
#define VELK_RENDER_MESH_BUFFER_H

#include <velk-render/ext/gpu_buffer.h>
#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/interface/intf_gpu_buffer.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Concrete IMeshBuffer.
 *
 * One owned byte vector holds both VBO and IBO contents: VBO bytes at
 * offset 0, IBO bytes at offset `vbo_size_` (== `get_ibo_offset()`).
 * Those bytes live in a region of the shared mesh-word arena (set = 1
 * slot 14), whose backing buffer carries `INDEX_BUFFER` usage.
 *
 * Both paths reach these bytes the same way, by index: `gpu_ref` gives the
 * word base RT and raster each add their own offsets to. Nothing here is
 * addressed: `vkCmdBindIndexBuffer` binds the arena's backing buffer by
 * handle, at this region's offset plus `get_ibo_offset()`.
 *
 * Offsets reported by `get_ibo_offset` stay RELATIVE to this mesh's own
 * bytes; consumers that need an absolute position in the arena add the
 * region's base themselves (via `get_gpu_ref`).
 *
 * Meshes without an IBO (e.g. the TriangleStrip unit quad) pass
 * `ibo_size == 0` to `set_data` and are simply never bound for indexed draws.
 */
class MeshBuffer
    : public ::velk::ext::GpuBuffer<MeshBuffer,
                                    ::velk::IMeshBuffer,
                                    ::velk::IMeshBufferInternal,
                                    ::velk::IArenaBuffer,
                                    ::velk::IGpuBuffer,
                                    ::velk::IGpuBufferStorageOwner>
{
public:
    VELK_CLASS_UID(::velk::ClassId::MeshBuffer, "MeshBuffer");

    // IMeshBuffer
    void set_data(const void* vbo_data, size_t vbo_size,
                  const void* ibo_data, size_t ibo_size) override;
    size_t get_vbo_size() const override { return vbo_size_; }
    size_t get_ibo_size() const override { return ibo_size_; }
    size_t get_ibo_offset() const override { return vbo_size_; }

    // IMeshBufferInternal
    bool ensure_geometry(IGpuResourceManager& resources) override;

    // IArenaBuffer: where both paths index this geometry (word base into
    // slot 14).
    GpuRef gpu_ref() const override;

    /// Stubbed pending a real use case (glTF hot-reload, morph targets,
    /// streaming LOD). API shape is committed so enabling later is an
    /// implementation-only change.
    ReturnValue update_vertex_range(size_t /*byte_offset*/,
                                    const void* /*data*/, size_t /*size*/) override
    {
        return ReturnValue::Fail;
    }
    ReturnValue update_index_range(size_t /*byte_offset*/,
                                   const void* /*data*/, size_t /*size*/) override
    {
        return ReturnValue::Fail;
    }

    // IBuffer overrides on top of ext::GpuBuffer:
    //
    // - get_data_size returns the logical vbo+ibo size, which survives
    //   `clear_dirty`'s CPU-byte release.
    // - clear_dirty also drops `data_` since mesh data is static once
    //   on the GPU; callers that need to mutate must call `set_data`
    //   again, which repopulates the buffer.
    // - write / write_diff are not exposed for arbitrary mutation;
    //   meshes go through `set_data`.
    size_t get_data_size() const override { return vbo_size_ + ibo_size_; }
    void clear_dirty() override
    {
        GpuBuffer::clear_dirty();
        auto& data = mutable_data();
        data.clear();
        data.shrink_to_fit();
    }
    bool write_diff(const void* /*bytes*/, size_t /*size*/) override { return false; }
    bool write(size_t /*sz*/, ::velk::IBuffer::WriteFn /*fn*/, void* /*ctx*/) override { return false; }

private:
    size_t vbo_size_ = 0;
    size_t ibo_size_ = 0;

    /// This mesh's bytes inside the shared mesh-word arena, held for the
    /// buffer's lifetime so its base is stable. Weak arena reference: mesh
    /// buffers are scene assets and can outlive the renderer.
    IGpuArena::WeakPtr arena_;
    ArenaRegion region_;
};

} // namespace velk::impl

#endif // VELK_RENDER_MESH_BUFFER_H
