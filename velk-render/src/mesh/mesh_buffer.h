#ifndef VELK_RENDER_MESH_BUFFER_H
#define VELK_RENDER_MESH_BUFFER_H

#include <velk-render/ext/gpu_buffer.h>
#include <velk-render/interface/intf_gpu_buffer.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Concrete IMeshBuffer.
 *
 * One owned byte vector holds both VBO and IBO contents: VBO bytes at
 * offset 0, IBO bytes at offset `vbo_size_` (== `get_ibo_offset()`).
 * The whole vector is uploaded as a single VkBuffer with
 * `SHADER_DEVICE_ADDRESS | INDEX_BUFFER` usage, so both the bindless
 * VBO reads and `vkCmdBindIndexBuffer(..., ibo_offset, UINT32)` target
 * the same allocation.
 *
 * Meshes without an IBO (e.g. the TriangleStrip unit quad) pass
 * `ibo_size == 0` to `set_data`; the renderer's upload pass sees the
 * zero size and skips the `INDEX_BUFFER` usage bit.
 */
class MeshBuffer
    : public ::velk::ext::GpuBuffer<MeshBuffer,
                                    ::velk::IMeshBuffer,
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
};

} // namespace velk::impl

#endif // VELK_RENDER_MESH_BUFFER_H
