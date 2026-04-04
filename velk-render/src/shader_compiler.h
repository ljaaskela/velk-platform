#ifndef VELK_RENDER_SHADER_COMPILER_H
#define VELK_RENDER_SHADER_COMPILER_H

#include <velk/vector.h>

#include <cstdint>

namespace velk {

enum class ShaderStage : uint8_t
{
    Vertex,
    Fragment,
};

vector<uint32_t> compile_glsl_to_spirv(const char* source, ShaderStage stage);

} // namespace velk

#endif // VELK_RENDER_SHADER_COMPILER_H
