#include "shader_compiler.h"

#include <velk/api/velk.h>

#include <cstring>
#include <shaderc/shaderc.hpp>
#include <string>

namespace velk {

namespace {

// Virtual include: velk.glsl
// Framework-level shader declarations.
const char* velk_glsl = R"(
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require

// Frame globals: projection matrix and viewport dimensions.
layout(buffer_reference, std430) readonly buffer Globals {
    mat4 projection;
    vec4 viewport; // width, height, 1/width, 1/height
};

// Dummy pointer type for fragment shaders that need to skip over
// 8-byte buffer_reference fields in the DrawData header.
layout(buffer_reference, std430) readonly buffer Ptr64 { uint _dummy; };

// Unit quad vertex position from a triangle strip vertex index.
// Returns (0,0), (1,0), (0,1), (1,1) for indices 0..3.
vec2 velk_unit_quad(int vertex_index)
{
    return vec2(vertex_index & 1, vertex_index >> 1);
}
)";

class VelkIncluder : public shaderc::CompileOptions::IncluderInterface
{
public:
    explicit VelkIncluder(const ShaderIncludeMap* user_includes) : user_includes_(user_includes) {}

    shaderc_include_result* GetInclude(const char* requested_source, shaderc_include_type, const char*,
                                       size_t) override
    {
        auto* result = new shaderc_include_result{};

        if (std::strcmp(requested_source, "velk.glsl") == 0) {
            result->source_name = "velk.glsl";
            result->source_name_length = 9;
            result->content = velk_glsl;
            result->content_length = std::strlen(velk_glsl);
        } else if (user_includes_) {
            auto it = user_includes_->find(requested_source);
            if (it != user_includes_->end()) {
                // Store the name so it outlives this call
                cached_name_ = it->first;
                result->source_name = cached_name_.c_str();
                result->source_name_length = cached_name_.size();
                result->content = it->second.c_str();
                result->content_length = it->second.size();
            } else {
                set_error(result, requested_source);
            }
        } else {
            set_error(result, requested_source);
        }

        return result;
    }

    void ReleaseInclude(shaderc_include_result* data) override { delete data; }

private:
    void set_error(shaderc_include_result* result, const char* name)
    {
        error_msg_ = string("unknown include: ") + name;
        result->source_name = "";
        result->source_name_length = 0;
        result->content = error_msg_.c_str();
        result->content_length = error_msg_.size();
    }

    const ShaderIncludeMap* user_includes_;
    string cached_name_;
    string error_msg_;
};

} // namespace

vector<uint32_t> compile_glsl_to_spirv(string_view source, ShaderStage stage,
                                       const ShaderIncludeMap* user_includes)
{
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;

    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    options.SetTargetSpirv(shaderc_spirv_version_1_5);
    options.SetGenerateDebugInfo();
    options.SetIncluder(std::make_unique<VelkIncluder>(user_includes));

    shaderc_shader_kind kind =
        (stage == ShaderStage::Vertex) ? shaderc_vertex_shader : shaderc_fragment_shader;

    const char* filename = (stage == ShaderStage::Vertex) ? "vertex.glsl" : "fragment.glsl";

    auto result = compiler.CompileGlslToSpv(source.data(), kind, filename, options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        VELK_LOG(E, "Shader compilation failed: %s", result.GetErrorMessage().c_str());
        return {};
    }

    return {result.cbegin(), result.cend()};
}

} // namespace velk
