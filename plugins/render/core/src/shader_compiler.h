#ifndef VELK_UI_SHADER_COMPILER_H
#define VELK_UI_SHADER_COMPILER_H

#include <velk/string.h>
#include <velk/vector.h>

#include <cstdint>

namespace velk_ui {

enum class ShaderStage : uint8_t
{
    Vertex,
    Fragment,
};

enum class ShaderTarget : uint8_t
{
    OpenGL,
    Vulkan,
};

/**
 * @brief Compiles GLSL source to SPIR-V bytecode at runtime via shaderc.
 *
 * Shaders may `#include "velk_common.glsl"` to get backend-specific
 * declarations (uniforms vs push constants, gl_VertexID vs gl_VertexIndex,
 * texture sampling). The include content is resolved based on the target.
 *
 * @param source  Null-terminated GLSL source string.
 * @param stage   Vertex or fragment.
 * @param target  OpenGL or Vulkan.
 * @return SPIR-V words. Empty on failure (errors logged via VELK_LOG).
 */
velk::vector<uint32_t> compile_glsl_to_spirv(const char* source, ShaderStage stage,
                                              ShaderTarget target = ShaderTarget::Vulkan);

/**
 * @brief Preprocesses GLSL source, resolving #includes for the given target.
 *
 * Returns the fully expanded GLSL source string ready for direct compilation
 * by the GL backend (glCompileShader).
 */
velk::string preprocess_glsl(const char* source, ShaderStage stage, ShaderTarget target);

} // namespace velk_ui

#endif // VELK_UI_SHADER_COMPILER_H
