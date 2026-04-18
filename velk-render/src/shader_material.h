#ifndef VELK_RENDER_SHADER_MATERIAL_IMPL_H
#define VELK_RENDER_SHADER_MATERIAL_IMPL_H

#include "spirv_material_reflect.h"

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/intf_material_internal.h>
#include <velk-render/interface/intf_shader_material.h>
#include <velk-render/plugin.h>

namespace velk::impl {

class ShaderMaterial : public ext::GpuResource<ShaderMaterial, IShaderMaterial, IMaterialInternal, IDrawData>
{
public:
    VELK_CLASS_UID(ClassId::ShaderMaterial, "ShaderMaterial");

    uint64_t get_pipeline_handle(IRenderContext&) override { return pipeline_handle_; }
    size_t get_draw_data_size() const override;
    ReturnValue write_draw_data(void* out, size_t size) const override;

    void set_pipeline_handle(uint64_t handle) override { pipeline_handle_ = handle; }
    ReturnValue setup_inputs(const vector<ShaderParam>& params) override;

private:
    uint64_t pipeline_handle_ = 0;
    vector<ShaderParam> params_;
    size_t gpu_data_size_ = 0;
};

} // namespace velk::impl

#endif // VELK_RENDER_SHADER_MATERIAL_IMPL_H
