#include "shader_compiler.h"

#include <velk/api/velk.h>

#include <shaderc/shaderc.hpp>

namespace velk_ui {

velk::vector<uint32_t> compile_glsl_to_spirv(const char* source, ShaderStage stage)
{
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;

    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    options.SetTargetSpirv(shaderc_spirv_version_1_5);
    options.SetGenerateDebugInfo();

    shaderc_shader_kind kind = (stage == ShaderStage::Vertex)
        ? shaderc_vertex_shader
        : shaderc_fragment_shader;

    const char* filename = (stage == ShaderStage::Vertex) ? "vertex.glsl" : "fragment.glsl";

    auto result = compiler.CompileGlslToSpv(source, kind, filename, options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        VELK_LOG(E, "Shader compilation failed: %s", result.GetErrorMessage().c_str());
        return {};
    }

    return {result.cbegin(), result.cend()};
}

} // namespace velk_ui
