#ifndef VELK_RENDER_GLSL_SHADER_COMPILER_H
#define VELK_RENDER_GLSL_SHADER_COMPILER_H

#include <velk/ext/object.h>

#include <velk-render/interface/intf_shader_compiler.h>
#include <velk-render/plugin.h>
#include <velk-render/plugins/glsl/plugin.h>

#include <unordered_map>

namespace velk::impl {

/**
 * @brief Compiles GLSL to SPIR-V with shaderc.
 *
 * Dependencies are virtual include files: shaders reach them with
 * `#include "name"`, resolved against the registered set rather than the
 * filesystem.
 */
class GlslShaderCompiler : public ext::ObjectCore<GlslShaderCompiler, IShaderCompiler>
{
public:
    VELK_CLASS_UID(ClassId::GlslShaderCompiler, "GlslShaderCompiler");

    vector<uint32_t> compile(string_view source, ShaderStage stage,
                             string* out_error = nullptr) override;

    void register_include(string_view name, string_view content) override;

    uint64_t dependency_hash() const override;

private:
    /// Name to content. velk core has no map type, and this never crosses a
    /// public header.
    std::unordered_map<string, string> includes_;
};

} // namespace velk::impl

#endif // VELK_RENDER_GLSL_SHADER_COMPILER_H
