#ifndef VELK_SCENE_DEFAULT_BATCH_H
#define VELK_SCENE_DEFAULT_BATCH_H

#include <velk/ext/core_object.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_batch.h>

#include <velk-scene/plugin.h>

#include <cstdint>

namespace velk::impl {

/**
 * @brief Default IBatch implementation: a renderer-facing data record
 *        that BatchBuilder fills once per frame.
 *
 * Hive-pooled via `velk::instance().create<IBatch>(ClassId::DefaultBatch)`.
 * Setters are public to keep the producer side ergonomic; consumers
 * always go through the IBatch interface.
 */
class DefaultBatch : public ext::ObjectCore<DefaultBatch, IBatch>
{
public:
    VELK_CLASS_UID(ClassId::DefaultBatch, "DefaultBatch");

    void set_pipeline_key(uint64_t v) { pipeline_key_ = v; }
    void set_texture_key(uint64_t v) { texture_key_ = v; }
    vector<uint8_t>& mutable_instance_data() { return instance_data_; }
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
};

} // namespace velk::impl

#endif // VELK_SCENE_DEFAULT_BATCH_H
