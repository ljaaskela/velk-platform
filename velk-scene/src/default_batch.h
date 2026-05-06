#ifndef VELK_SCENE_DEFAULT_BATCH_H
#define VELK_SCENE_DEFAULT_BATCH_H

#include <velk/vector.h>

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_batch.h>
#include <velk-render/interface/intf_buffer.h>

#include <velk-scene/plugin.h>

#include <cstring>
#include <cstdint>

namespace velk::impl {

/**
 * @brief Default IBatch implementation: a renderer-facing data record
 *        that BatchBuilder fills once per frame.
 *
 * Hive-pooled via `velk::instance().create<IBatch>(ClassId::DefaultBatch)`.
 * Each batch owns its own GPU buffer with the `BatchBufferLayout` blob
 * (`[args(32)][count(16)][instance_data]`). The buffer is allocated and
 * uploaded by the renderer's standard pipeline:
 * `IGpuResourceManager::ensure_buffer_storage` allocates the backing
 * VkBuffer, the upload pass `map`s it and copies `get_data()` into it,
 * `clear_dirty()` flips the dirty bit. Lifetime: the IBatch::Ptr's
 * destructor cascades through `ext::GpuResource::~GpuResource()` to the
 * resource manager observer, which fence-tracks the underlying
 * `GpuBufferHandle` for deferred destruction.
 *
 * `update_instance_at` writes through the cached mapped pointer (stamped
 * by the upload pass) directly into the same backing memory, so
 * transform-only updates don't need a re-upload.
 */
class DefaultBatch : public ext::GpuResource<DefaultBatch, IBatch, IBuffer>
{
public:
    VELK_CLASS_UID(ClassId::DefaultBatch, "DefaultBatch");

    using Layout = ::velk::BatchBufferLayout;

    GpuResourceType get_type() const override { return GpuResourceType::Buffer; }

    void set_pipeline_key(uint64_t v) { pipeline_key_ = v; }
    void set_texture_key(uint64_t v) { texture_key_ = v; }
    /// Producer-side scratch the BatchBuilder appends instance bytes
    /// into during scene walk. After the batch is fully populated,
    /// `finalize_storage` packs args/count/instance_data into
    /// `storage_blob_` for the upload pipeline to consume.
    vector<uint8_t>& mutable_instance_data() { return instance_data_; }
    void set_instance_stride(uint32_t v) { instance_stride_ = v; }
    void set_instance_count(uint32_t v) { instance_count_ = v; }
    void set_world_aabb(const aabb& v) { world_aabb_ = v; }
    void set_material(IProgram::Ptr v) { material_ = std::move(v); }
    void set_primitive(IMeshPrimitive::Ptr v) { primitive_ = std::move(v); }
    void set_shader_source(IShaderSource::Ptr v) { shader_source_ = std::move(v); }
    void set_pipeline_options(const PipelineOptions& v) { pipeline_options_ = v; }

    /// Producer-side: pack the args + count prefix and instance bytes
    /// into `storage_blob_`. `indexed` chooses VkDrawIndexedIndirectCommand
    /// (5×u32) vs VkDrawIndirectCommand (4×u32). count is always 1 today;
    /// a future GPU-culling pass will overwrite without changing layout.
    void finalize_storage(uint32_t prim_count, bool indexed)
    {
        storage_blob_.resize(Layout::kInstanceOffset + instance_data_.size());
        // args
        if (indexed) {
            uint32_t args[5] = { prim_count, instance_count_, 0u, 0u, 0u };
            std::memcpy(storage_blob_.data() + Layout::kArgsOffset, args, sizeof(args));
        } else {
            uint32_t args[4] = { prim_count, instance_count_, 0u, 0u };
            std::memcpy(storage_blob_.data() + Layout::kArgsOffset, args, sizeof(args));
        }
        // count
        uint32_t count = 1;
        std::memcpy(storage_blob_.data() + Layout::kCountOffset, &count, sizeof(count));
        // instance data
        if (!instance_data_.empty()) {
            std::memcpy(storage_blob_.data() + Layout::kInstanceOffset,
                        instance_data_.data(), instance_data_.size());
        }
        storage_dirty_ = true;
    }

    /// Renderer-side: called once after `ensure_buffer_storage` allocates
    /// the backing GpuBufferHandle and the upload pass has memcpy'd the blob.
    /// Captures the handle + mapped pointer so `update_instance_at` can
    /// write through directly on subsequent transform-only frames.
    void set_storage_mapping(GpuBufferHandle handle, uint8_t* mapped_ptr)
    {
        storage_handle_ = handle;
        storage_mapped_ = mapped_ptr;
    }

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
                            array_view<const uint8_t> bytes) override
    {
        if (instance_stride_ == 0) return;
        if (instance_index >= instance_count_) return;
        size_t offset = static_cast<size_t>(instance_index) * instance_stride_;
        if (offset + bytes.size() > instance_data_.size()) return;
        std::memcpy(instance_data_.data() + offset, bytes.begin(), bytes.size());
        if (storage_mapped_) {
            std::memcpy(storage_mapped_ + Layout::kInstanceOffset + offset,
                        bytes.begin(), bytes.size());
        }
    }

    GpuBufferHandle storage_buffer() const override { return storage_handle_; }
    uint64_t  storage_gpu_address() const override
    {
        // ensure_buffer_storage stamps the GPU device address into our
        // ext::GpuResource handle via set_gpu_handle(Default, addr).
        return get_gpu_handle(GpuResourceKey::Default);
    }

    // IBuffer
    size_t get_data_size() const override { return storage_blob_.size(); }
    const uint8_t* get_data() const override { return storage_blob_.data(); }
    bool is_dirty() const override { return storage_dirty_; }
    void clear_dirty() override { storage_dirty_ = false; }

private:
    uint64_t pipeline_key_ = 0;
    uint64_t texture_key_ = 0;
    /// Producer-side scratch. BatchBuilder appends instance bytes
    /// during scene walk; finalize_storage copies these into the tail
    /// of storage_blob_. update_instance_at keeps both in sync so
    /// instance_data() always reflects the current GPU contents.
    vector<uint8_t> instance_data_;
    uint32_t instance_stride_ = 0;
    uint32_t instance_count_ = 0;
    aabb world_aabb_ = aabb::empty();
    IProgram::Ptr material_;
    IMeshPrimitive::Ptr primitive_;
    IShaderSource::Ptr shader_source_;
    PipelineOptions pipeline_options_{};

    /// Contiguous `[args(32)][count(16)][instance_data]` blob the
    /// renderer's IBuffer-upload pipeline copies to GPU when
    /// `is_dirty()` is set.
    vector<uint8_t> storage_blob_;
    bool storage_dirty_ = false;

    /// Cached after the first upload — `storage_handle_` is the
    /// backend's GpuBufferHandle ID for indirect-args binding;
    /// `storage_mapped_` is the host-visible pointer for in-place
    /// transform updates.
    GpuBufferHandle storage_handle_ = 0;
    uint8_t* storage_mapped_ = nullptr;
};

} // namespace velk::impl

#endif // VELK_SCENE_DEFAULT_BATCH_H
