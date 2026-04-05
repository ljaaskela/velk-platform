#ifndef VELK_RENDER_INTF_SHADER_H
#define VELK_RENDER_INTF_SHADER_H

#include <velk/array_view.h>
#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>

#include <cstdint>

namespace velk {

/** @brief Shader compilation stage. */
enum class ShaderStage : uint8_t
{
    Vertex,
    Fragment,
};

/**
 * @brief A compiled shader handle.
 *
 * Owns the compiled bytecode and destroys it automatically when the last
 * reference is released. Created via IRenderContext::compile_shader().
 */
class IShader : public Interface<IShader>
{
public:
    /**
     * @brief Initializes the shader with compiled bytecode.
     * @param bytecode Compiled shader data (moved into the shader).
     */
    virtual void init(vector<uint32_t> bytecode) = 0;

    /** @brief Returns the compiled shader bytecode. */
    virtual array_view<const uint32_t> get_data() const = 0;

    /** @brief Returns the size of the bytecode in bytes. */
    virtual size_t get_data_size() const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_SHADER_H
