#ifndef VELK_RENDER_INTF_RENDER_TECHNIQUE_H
#define VELK_RENDER_INTF_RENDER_TECHNIQUE_H

#include <velk-render/interface/intf_shader_snippet.h>

namespace velk {

/**
 * @brief Base interface for pluggable rendering effects (shadows,
 *        reflections, AO, GI, post-processing).
 *
 * Every technique contributes a GLSL snippet that declares an
 * evaluator function. The lighting / post composer stitches the
 * snippet in as a shader include, generates a switch keyed on
 * technique class UIDs, and dispatches to `get_snippet_fn_name()`
 * for each active technique — the same machinery used by
 * material fill snippets, which is why both extend `IShaderSnippet`.
 *
 * Sub-interfaces (`IShadowTechnique`, `IReflectionTechnique`, ...)
 * pin down the exact GLSL function signature and declare a prepare()
 * context suited to the effect.
 *
 * Techniques attach to the render-trait they configure (shadow on
 * Light, reflection/AO/GI/post on Camera) via the normal velk
 * attachment mechanism. They do not participate in any UI trait
 * machinery.
 */
class IRenderTechnique : public Interface<IRenderTechnique, IShaderSnippet>
{
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_TECHNIQUE_H
