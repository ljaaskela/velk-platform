#ifndef VELK_SCENE_DEFAULT_BATCH_H
#define VELK_SCENE_DEFAULT_BATCH_H

#include <velk/ext/core_object.h>
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
 * Also implements `IBuffer` so the renderer's standard persistent GPU
 * buffer upload path manages this batch's instance bytes — once
 * uploaded the GPU handle stays stable across frames, so per-frame
 * staging copies go away for unchanged batches.
 * `instance_data_dirty_` is set on every mutation
 * (mutable_instance_data / update_instance_at) and cleared by the
 * renderer after upload via `clear_dirty()`.
 *
 * Hive-pooled via `velk::instance().create<IBatch>(ClassId::DefaultBatch)`.
 * `ext::GpuResource` provides the IGpuResource observer plumbing so the
 * resource manager fence-tracks this batch's GPU handle for deferred
 * destroy on Ptr release.
 */
class DefaultBatch : public ext::GpuResource<DefaultBatch, IBatch, IBuffer>
{
public:
    VELK_CLASS_UID(ClassId::DefaultBatch, "DefaultBatch");

    GpuResourceType get_type() const override { return GpuResourceType::Buffer; }

    void set_pipeline_key(uint64_t v) { pipeline_key_ = v; }
    void set_texture_key(uint64_t v) { texture_key_ = v; }
    vector<uint8_t>& mutable_instance_data() { instance_data_dirty_ = true; return instance_data_; }
    void set_instance_stride(uint32_t v) { instance_stride_ = v; }
    void set_instance_count(uint32_t v) { instance_count_ = v; }
    void set_world_aabb(const aabb& v) { world_aabb_ = v; }
    void set_material(IProgram::Ptr v) { material_ = std::move(v); }
    void set_primitive(IMeshPrimitive::Ptr v) { primitive_ = std::move(v); }
    void set_shader_source(IShaderSource::Ptr v) { shader_source_ = std::move(v); }
    void set_pipeline_options(const PipelineOptions& v) { pipeline_options_ = v; }

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
        instance_data_dirty_ = true;
    }

    // IBuffer
    size_t get_data_size() const override { return instance_data_.size(); }
    const uint8_t* get_data() const override { return instance_data_.data(); }
    bool is_dirty() const override { return instance_data_dirty_; }
    void clear_dirty() override { instance_data_dirty_ = false; }

private:
    uint64_t pipeline_key_ = 0;
    uint64_t texture_key_ = 0;
    vector<uint8_t> instance_data_;
    uint32_t instance_stride_ = 0;
    uint32_t instance_count_ = 0;
    aabb world_aabb_ = aabb::empty();
    IProgram::Ptr material_;
    IMeshPrimitive::Ptr primitive_;
    IShaderSource::Ptr shader_source_;
    PipelineOptions pipeline_options_{};
    bool instance_data_dirty_ = false;
};

} // namespace velk::impl

#endif // VELK_SCENE_DEFAULT_BATCH_H
