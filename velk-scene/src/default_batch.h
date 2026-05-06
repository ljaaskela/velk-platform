#ifndef VELK_SCENE_DEFAULT_BATCH_H
#define VELK_SCENE_DEFAULT_BATCH_H

#include <velk/vector.h>

#include <velk-render/ext/render_state.h>
#include <velk-render/interface/intf_batch.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-scene/plugin.h>

#include <cstdint>

namespace velk::impl {

/**
 * @brief Default IBatch implementation: a renderer-facing data record
 *        that BatchBuilder fills once per frame.
 *
 * Hive-pooled via `velk::instance().create<IBatch>(ClassId::DefaultBatch)`.
 * Each batch composes an `IBuffer` (an `impl::GpuBuffer` instance) holding
 * the `BatchBufferLayout` blob `[args(32)][count(16)][instance_data]`. The
 * buffer is allocated and uploaded by the renderer's standard pipeline:
 * `IGpuResourceManager::ensure_buffer_storage` allocates the backing
 * VkBuffer for the IBuffer; the upload pass `map`s it and copies
 * `get_data()` into it; `clear_dirty()` flips the dirty bit. Lifetime: the
 * IBatch::Ptr's destructor drops the inner `IBuffer::Ptr`, whose
 * `ext::GpuResource` destructor cascades through the resource manager
 * observer to defer the underlying `GpuBufferHandle` for destruction.
 *
 * `update_instance_at` writes through the cached mapped pointer (stamped
 * by the upload pass) directly into the same backing memory, so
 * transform-only updates don't need a re-upload.
 */
class DefaultBatch : public ::velk::ext::RenderState<DefaultBatch, IBatch>
{
public:
    VELK_CLASS_UID(ClassId::DefaultBatch, "DefaultBatch");

    using Layout = ::velk::BatchBufferLayout;

    void set_pipeline_key(uint64_t v) { pipeline_key_ = v; }
    void set_texture_key(uint64_t v) { texture_key_ = v; }
    /// Producer-side scratch the BatchBuilder appends instance bytes
    /// into during scene walk. After the batch is fully populated,
    /// `finalize_storage` packs args/count/instance_data into the
    /// composed storage buffer for the upload pipeline to consume.
    vector<uint8_t>& mutable_instance_data() { return instance_data_; }
    void set_instance_stride(uint32_t v) { instance_stride_ = v; }
    void set_instance_count(uint32_t v) { instance_count_ = v; }
    void set_world_aabb(const aabb& v) { world_aabb_ = v; }
    void set_material(IProgram::Ptr v) { material_ = std::move(v); }
    void set_primitive(IMeshPrimitive::Ptr v) { primitive_ = std::move(v); }
    void set_shader_source(IShaderSource::Ptr v) { shader_source_ = std::move(v); }
    void set_pipeline_options(const PipelineOptions& v) { pipeline_options_ = v; }

    /// Producer-side: pack the args + count prefix and instance bytes
    /// into the composed storage buffer's bytes. `indexed` chooses
    /// VkDrawIndexedIndirectCommand (5×u32) vs VkDrawIndirectCommand
    /// (4×u32). count is always 1 today; a future GPU-culling pass will
    /// overwrite without changing layout.
    void finalize_storage(uint32_t prim_count, bool indexed);

    /// Renderer-side: called once after `ensure_buffer_storage` allocates
    /// the backing GpuBufferHandle and the upload pass has memcpy'd the
    /// blob. Captures the mapped pointer so `update_instance_at` can
    /// write through directly on subsequent transform-only frames.
    void set_storage_mapping(uint8_t* mapped_ptr) { storage_mapped_ = mapped_ptr; }

    uint64_t pipeline_key() const override { return pipeline_key_; }
    uint64_t texture_key() const override { return texture_key_; }

    array_view<const uint8_t> instance_data() const override
    {
        return array_view<const uint8_t>(instance_data_.data(), instance_data_.size());
    }
    uint32_t instance_stride() const override { return instance_stride_; }
    uint32_t instance_count() const override { return instance_count_; }
    aabb world_aabb() const override { return world_aabb_; }

    IProgram::Ptr material() const override { return material_; }
    IMeshPrimitive::Ptr primitive() const override { return primitive_; }
    IShaderSource::Ptr shader_source() const override { return shader_source_; }
    PipelineOptions pipeline_options() const override { return pipeline_options_; }

    void update_instance_at(uint32_t instance_index,
                            array_view<const uint8_t> bytes) override;

    IBuffer* storage_buffer() const override { return storage_.get(); }
    uint64_t storage_gpu_address() const override;
    uint8_t* storage_mapped() const override { return storage_mapped_; }

private:
    uint64_t pipeline_key_ = 0;
    uint64_t texture_key_ = 0;
    /// Producer-side scratch. BatchBuilder appends instance bytes
    /// during scene walk; finalize_storage copies these into the tail
    /// of the composed storage buffer's blob. update_instance_at keeps
    /// both in sync so instance_data() always reflects current GPU
    /// contents.
    vector<uint8_t> instance_data_;
    uint32_t instance_stride_ = 0;
    uint32_t instance_count_ = 0;
    aabb world_aabb_ = aabb::empty();
    IProgram::Ptr material_;
    IMeshPrimitive::Ptr primitive_;
    IShaderSource::Ptr shader_source_;
    PipelineOptions pipeline_options_{};

    /// Composed `[args(32)][count(16)][instance_data]` blob carrier.
    /// Created lazily on first finalize. Lifetime tied to the batch.
    IBuffer::Ptr storage_;
    /// Cached after each upload — host-visible pointer for in-place
    /// transform updates. Cleared by finalize_storage when the blob is
    /// resized (since ensure_buffer_storage will reallocate).
    uint8_t* storage_mapped_ = nullptr;
};

} // namespace velk::impl

#endif // VELK_SCENE_DEFAULT_BATCH_H
