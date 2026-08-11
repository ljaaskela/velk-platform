#ifndef VELK_RENDER_SHADER_MANAGER_H
#define VELK_RENDER_SHADER_MANAGER_H

#include "shader/shader_cache.h"

#include <velk/ext/object.h>

#include <velk-render/interface/intf_shader_compiler.h>
#include <velk-render/interface/intf_shader_manager.h>
#include <velk-render/plugin.h>

namespace velk::impl {

class ShaderManager : public ext::ObjectCore<ShaderManager, IShaderManager>
{
public:
    VELK_CLASS_UID(ClassId::ShaderManager, "ShaderManager");

    bool init() override;

    IShader::Ptr compile(string_view source, ShaderStage stage, uint64_t key = 0) override;

    void register_include(string_view name, string_view content) override;

    void set_default_vertex_shader(const IShader::Ptr& shader) override { default_vertex_ = shader; }
    IShader::Ptr default_vertex_shader() const override { return default_vertex_; }

    void set_default_fragment_shader(const IShader::Ptr& shader) override { default_fragment_ = shader; }
    IShader::Ptr default_fragment_shader() const override { return default_fragment_; }

private:
    IShaderCompiler::Ptr compiler_;
    mutable ShaderCache cache_;
    IShader::Ptr default_vertex_;
    IShader::Ptr default_fragment_;
};

} // namespace velk::impl

#endif // VELK_RENDER_SHADER_MANAGER_H
