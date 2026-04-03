#ifndef VELK_UI_SHADER_COMPILER_H
#define VELK_UI_SHADER_COMPILER_H

#include <velk/vector.h>

#include <cstdint>

namespace velk_ui {

enum class ShaderStage : uint8_t
{
    Vertex,
    Fragment,
};

/**
 * @brief Compiles GLSL source to SPIR-V bytecode at runtime via shaderc.
 *
 * Targets Vulkan 1.2 / SPIR-V 1.5. Enables GL_EXT_buffer_reference and
 * GL_EXT_buffer_reference2 for pointer-based data access.
 *
 * @param source  Null-terminated GLSL source string.
 * @param stage   Vertex or fragment.
 * @return SPIR-V words. Empty on failure (errors logged via VELK_LOG).
 */
velk::vector<uint32_t> compile_glsl_to_spirv(const char* source, ShaderStage stage);

} // namespace velk_ui

#endif // VELK_UI_SHADER_COMPILER_H
