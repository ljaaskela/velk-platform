#ifndef VELK_RENDER_MESH_BUFFER_H
#define VELK_RENDER_MESH_BUFFER_H

#include <velk/vector.h>

#include <velk-render/ext/gpu_resource.h>
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
    : public ::velk::ext::GpuResource<MeshBuffer, IMeshBuffer>
{
public:
    VELK_CLASS_UID(::velk::ClassId::MeshBuffer, "MeshBuffer");

    GpuResourceType get_type() const override { return GpuResourceType::Buffer; }

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

    // IBuffer
    //
    // Size queries report the original data size even after the CPU
    // bytes have been released in `clear_dirty()`. `get_data()` returns
    // nullptr once released; the upload pass only calls it while
    // `is_dirty()` is true, so post-upload callers see the buffer as
    // "already uploaded" and skip it.
    size_t get_data_size() const override { return vbo_size_ + ibo_size_; }
    const uint8_t* get_data() const override
    {
        return bytes_.empty() ? nullptr : bytes_.data();
    }
    bool is_dirty() const override { return dirty_; }
    bool write_diff(const void* /*bytes*/, size_t /*size*/) override { return false; }
    bool write(size_t /*sz*/, WriteFn /*fn*/, void* /*ctx*/) override { return false; }
    void clear_dirty() override
    {
        dirty_ = false;
        // Mesh data is static: once on the GPU the CPU-side copy is
        // redundant. Release it to recover ~64 bytes per unit-quad mesh
        // and multi-MB per glTF mesh. `set_data` repopulates the buffer
        // if the caller needs to change contents.
        bytes_.clear();
        bytes_.shrink_to_fit();
    }

private:
    ::velk::vector<uint8_t> bytes_;
    size_t vbo_size_ = 0;
    size_t ibo_size_ = 0;
    bool dirty_ = false;
};

} // namespace velk::impl

#endif // VELK_RENDER_MESH_BUFFER_H
