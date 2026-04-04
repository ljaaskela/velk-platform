#ifndef VELK_RENDER_SHADER_COMPILER_H
#define VELK_RENDER_SHADER_COMPILER_H

#include <velk/string.h>
#include <velk/vector.h>

#include <cstdint>
#include <unordered_map>

namespace velk {

enum class ShaderStage : uint8_t
{
    Vertex,
    Fragment,
};

/// Registry of virtual shader include files (name -> content).
using ShaderIncludeMap = std::unordered_map<string, string>;

/// Compiles GLSL to SPIR-V. Resolves #include directives against
/// the built-in velk.glsl and any user-registered includes.
vector<uint32_t> compile_glsl_to_spirv(string_view source, ShaderStage stage,
                                       const ShaderIncludeMap* user_includes = nullptr);

} // namespace velk

#endif // VELK_RENDER_SHADER_COMPILER_H
