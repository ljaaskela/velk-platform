#ifndef VELK_RENDER_INTF_SHADER_COMPILER_H
#define VELK_RENDER_INTF_SHADER_COMPILER_H

#include <velk/interface/intf_metadata.h>
#include <velk/string.h>
#include <velk/string_view.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_shader.h>

#include <cstdint>

namespace velk {

/**
 * @brief Compiles shader source to SPIR-V.
 *
 * The render context reaches every shader compilation through this interface,
 * so a build that never compiles at runtime (one shipping a fully populated
 * SPIR-V cache) does not need an implementation present at all.
 *
 * A compiler also owns its own dependency model. What a shader depends on
 * besides its own source is language-specific, so the context does not model
 * it; it forwards registrations and asks for a hash to fold into cache keys.
 */
class IShaderCompiler : public Interface<IShaderCompiler>
{
public:
    /**
     * @brief Compiles @p source for @p stage.
     *
     * @param source    Shader source code.
     * @param stage     Stage to compile for.
     * @param out_error Optional. Receives the compiler's diagnostic text when
     *                  compilation fails. May be nullptr.
     * @return The compiled SPIR-V words, or an empty vector on failure.
     */
    virtual vector<uint32_t> compile(string_view source, ShaderStage stage,
                                     string* out_error = nullptr) = 0;

    /**
     * @brief Registers a named dependency that shaders can pull in.
     *
     * Registering the same name again replaces the previous content.
     * Registration happens during operation as well as at startup: material
     * snippets are composed and registered while the app runs.
     */
    virtual void register_include(string_view name, string_view content) = 0;

    /**
     * @brief Returns a hash covering everything registered via
     * register_include().
     *
     * Folded into shader cache keys so that changing a dependency invalidates
     * the entries compiled against the old content. Must be deterministic
     * across runs for identical content.
     */
    virtual uint64_t dependency_hash() const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_SHADER_COMPILER_H
