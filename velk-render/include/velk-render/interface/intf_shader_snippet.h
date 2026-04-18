#ifndef VELK_RENDER_INTF_SHADER_SNIPPET_H
#define VELK_RENDER_INTF_SHADER_SNIPPET_H

#include <velk/interface/intf_metadata.h>
#include <velk/string_view.h>

namespace velk {

class IRenderContext;

/**
 * @brief Contract for objects that contribute a GLSL function to a
 *        composed shader.
 *
 * Shared by every snippet-style contributor in the pipeline:
 *   - `IRenderTechnique` sub-interfaces (shadow, reflection, AO, GI, ...),
 *   - material RT fill contributions,
 *   - future post-process composer fragments.
 *
 * The composer stitches the snippet into the target shader as a shader
 * `#include`, then emits a dispatch switch on class UID that calls
 * `get_snippet_fn_name()` for each active contributor. Every interface
 * consuming composer output speaks to the base type; no parallel
 * naming per effect kind.
 *
 * The include filename is derived internally as
 * `get_snippet_fn_name() + ".glsl"` — no user-visible include_name.
 */
class IShaderSnippet : public Interface<IShaderSnippet>
{
public:
    /**
     * @brief Returns the unique function name declared by the snippet.
     *
     * Used as the dispatch target in the composed shader and — with a
     * ".glsl" suffix — as the shader-include filename. Must be unique
     * across all snippet contributors the composer might see together.
     * By convention prefixed with `velk_` + domain, e.g.
     * `"velk_shadow_rt"`, `"velk_fill_standard"`.
     */
    virtual string_view get_snippet_fn_name() const = 0;

    /**
     * @brief Returns the GLSL body declaring the function named by
     *        get_snippet_fn_name().
     *
     * Self-contained: declares any buffer_reference structs, sampler2D
     * resources, and helper functions it needs, plus exactly one entry
     * function with the signature fixed by the consuming sub-interface
     * (e.g. `float velk_shadow_rt(uint, vec3, vec3)` for shadows).
     */
    virtual string_view get_snippet_source() const = 0;

    /**
     * @brief Hook for snippets whose source depends on additional
     *        shader includes.
     *
     * The composer calls this once when the class is first encountered,
     * before compiling any pipeline that references it. Implementations
     * call `ctx.register_shader_include(name, src)` for each dependency.
     * Idempotent; default empty for snippets with no transitive deps.
     */
    virtual void register_snippet_includes(IRenderContext& /*ctx*/) const {}
};

} // namespace velk

#endif // VELK_RENDER_INTF_SHADER_SNIPPET_H
