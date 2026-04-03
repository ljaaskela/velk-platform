#include "shader_compiler.h"

#include <velk/api/velk.h>

#include <shaderc/shaderc.hpp>

#include <cstring>
#include <string>

namespace velk_ui {

namespace {

// Virtual include content for Vulkan targets.
// Push constant layout matches VkBackend offsets: projection(0), rect(64), texture_index(80).
const char* velk_common_vulkan = R"(
#define VERTEX_INDEX gl_VertexIndex

layout(push_constant) uniform PushConstants {
    mat4 u_projection;
    vec4 u_rect;
    uint u_texture_index;
};
)";

// Virtual include content for OpenGL targets.
// Regular uniforms, introspectable via glGetActiveUniform.
const char* velk_common_opengl = R"(
#define VERTEX_INDEX gl_VertexID

layout(std140, binding = 0) uniform VelkGlobals {
    mat4 u_projection;
    vec4 u_rect;
};
)";

class VelkIncluder : public shaderc::CompileOptions::IncluderInterface
{
public:
    explicit VelkIncluder(ShaderTarget target) : target_(target) {}

    shaderc_include_result* GetInclude(
        const char* requested_source,
        shaderc_include_type /*type*/,
        const char* /*requesting_source*/,
        size_t /*include_depth*/) override
    {
        auto* result = new shaderc_include_result{};

        if (std::strcmp(requested_source, "velk_common.glsl") == 0) {
            const char* content = (target_ == ShaderTarget::Vulkan)
                ? velk_common_vulkan
                : velk_common_opengl;
            result->source_name = "velk_common.glsl";
            result->source_name_length = std::strlen(result->source_name);
            result->content = content;
            result->content_length = std::strlen(content);
        } else {
            static const char* error_msg = "unknown include";
            result->source_name = "";
            result->source_name_length = 0;
            result->content = error_msg;
            result->content_length = std::strlen(error_msg);
        }

        return result;
    }

    void ReleaseInclude(shaderc_include_result* data) override
    {
        delete data;
    }

private:
    ShaderTarget target_;
};

} // namespace

velk::vector<uint32_t> compile_glsl_to_spirv(const char* source, ShaderStage stage,
                                              ShaderTarget target)
{
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;

    if (target == ShaderTarget::OpenGL) {
        options.SetTargetEnvironment(shaderc_target_env_opengl, shaderc_env_version_opengl_4_5);
    } else {
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
        options.SetTargetSpirv(shaderc_spirv_version_1_5);
        options.AddMacroDefinition("VELK_VULKAN", "1");
    }

    options.SetIncluder(std::make_unique<VelkIncluder>(target));
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

velk::string preprocess_glsl(const char* source, ShaderStage stage, ShaderTarget target)
{
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;

    options.SetIncluder(std::make_unique<VelkIncluder>(target));

    shaderc_shader_kind kind = (stage == ShaderStage::Vertex)
        ? shaderc_vertex_shader
        : shaderc_fragment_shader;

    auto result = compiler.PreprocessGlsl(source, kind, "shader.glsl", options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        VELK_LOG(E, "Shader preprocessing failed: %s", result.GetErrorMessage().c_str());
        return {};
    }

    auto begin = result.cbegin();
    auto end = result.cend();
    return velk::string(begin, static_cast<size_t>(end - begin));
}

} // namespace velk_ui
