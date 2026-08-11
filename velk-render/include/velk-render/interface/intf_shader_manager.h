#ifndef VELK_RENDER_INTF_SHADER_MANAGER_H
#define VELK_RENDER_INTF_SHADER_MANAGER_H

#include <velk/interface/intf_metadata.h>
#include <velk/string_view.h>

#include <velk-render/interface/intf_shader.h>

#include <cstdint>

namespace velk {

/**
 * @brief Compiles shaders and caches the results.
 *
 * Owned by the render context and reached through
 * IRenderContext::shaders(). Holds the active IShaderCompiler, the set of
 * named dependencies shaders can pull in, and the on-disk SPIR-V cache, so the
 * render context itself does not deal in shaders.
 */
class IShaderManager : public Interface<IShaderManager>
{
public:
    /**
     * @brief Resolves the shader compiler and registers the framework-level
     *        shader dependencies.
     * @return false if no compiler is available, which is fatal for the
     *         render context.
     */
    virtual bool init() = 0;

    /**
     * @brief Compiles @p source to a reusable shader handle.
     *
     * @param source Shader source code.
     * @param stage  Stage to compile for.
     * @param key    Optional cache key. When non-zero, the SPIR-V is read from
     *               and written to the shader cache under this key. When zero,
     *               a hash of @p source is computed at runtime. Built-in
     *               shaders should pass a constexpr `make_hash64(source)` to
     *               avoid the runtime hash; user shaders pass 0.
     * @return The compiled shader, or nullptr on failure.
     */
    virtual IShader::Ptr compile(string_view source, ShaderStage stage, uint64_t key = 0) = 0;

    /**
     * @brief Registers a named dependency shaders can pull in.
     *
     * Forwards to the active compiler. For GLSL these are virtual include
     * files, reached with `#include "name"`. Registering the same name again
     * replaces the previous content. Works at any time, not only during
     * startup: composed material snippets register while the app runs.
     */
    virtual void register_include(string_view name, string_view content) = 0;

    /** @brief Shader used when a pipeline supplies no vertex stage. */
    virtual void set_default_vertex_shader(const IShader::Ptr& shader) = 0;
    virtual IShader::Ptr default_vertex_shader() const = 0;

    /** @brief Shader used when a pipeline supplies no fragment stage. */
    virtual void set_default_fragment_shader(const IShader::Ptr& shader) = 0;
    virtual IShader::Ptr default_fragment_shader() const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_SHADER_MANAGER_H
