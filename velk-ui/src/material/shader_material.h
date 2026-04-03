#ifndef VELK_UI_SHADER_MATERIAL_H
#define VELK_UI_SHADER_MATERIAL_H

#include <velk/ext/object.h>

#include <velk-ui/interface/intf_material.h>
#include <velk-ui/interface/intf_material_internal.h>
#include <velk-ui/plugin.h>

namespace velk_ui {

/**
 * @brief Material with a custom shader program.
 *
 * The pipeline handle is set by the render context factory after compiling
 * the shader, via the IMaterialInternal interface.
 */
class ShaderMaterial : public velk::ext::Object<ShaderMaterial, IMaterial, IMaterialInternal>
{
public:
    VELK_CLASS_UID(ClassId::Material::Shader, "ShaderMaterial");

    uint64_t get_pipeline_handle(IRenderContext&) override { return pipeline_handle_; }
    void set_pipeline_handle(uint64_t handle) override { pipeline_handle_ = handle; }

    size_t gpu_data_size() const override { return 0; }
    void write_gpu_data(void*, size_t) const override {}

private:
    uint64_t pipeline_handle_ = 0;
};

} // namespace velk_ui

#endif // VELK_UI_SHADER_MATERIAL_H
