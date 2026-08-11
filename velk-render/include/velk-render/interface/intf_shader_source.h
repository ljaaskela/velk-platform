#ifndef VELK_RENDER_INTF_SHADER_SOURCE_H
#define VELK_RENDER_INTF_SHADER_SOURCE_H

#include <velk/interface/intf_metadata.h>
#include <velk/string_view.h>

#include <cstdint>

namespace velk {

class IRenderContext;

/**
 * @brief Predefined role names for IShaderSource queries.
 *
 * Producers and consumers reference these constants instead of literal
 * strings so typos surface as compile errors. New role kinds (post,
 * filter, ...) extend this list as they land.
 */
namespace shader_role {

inline constexpr string_view kVertex    = "vertex";    ///< Full vertex shader (visuals + materials).
inline constexpr string_view kFragment  = "fragment";  ///< Full fragment shader (visuals + ShaderMaterial-style materials).
inline constexpr string_view kDiscard   = "discard";   ///< `void velk_visual_discard()` snippet (visuals).
inline constexpr string_view kEval      = "eval";      ///< `MaterialEval velk_eval_<name>(EvalContext)` snippet (materials).
inline constexpr string_view kShadow    = "shadow";    ///< `float velk_shadow_<name>(uint, vec3, vec3)` snippet (shadow techniques).
inline constexpr string_view kIntersect = "intersect"; ///< `bool velk_intersect_<name>(Ray, RtShape, out RayHit)` snippet (analytic shapes).

} // namespace shader_role

/**
 * @brief Unified contract for objects that contribute GLSL source to a
 *        composed pipeline.
 *
 * A single class can fill multiple roles (e.g. a visual that brings
 * both `kVertex` and `kFragment`; a material that contributes `kEval`
 * plus a custom `kVertex`). Roles a source doesn't fill return empty
 * sources / function names and the consumer falls back to its driver
 * default.
 *
 * The signature pinned for each role's source is the role's contract,
 * documented above. Consumers (paths, FrameSnippetRegistry) query by
 * role and stitch the result into their composed shader as either a
 * full stage or a `#include` plus dispatch switch.
 */
class IShaderSource : public Interface<IShaderSource>
{
public:
    /**
     * @brief Returns the GLSL body for @p role. Empty when not provided.
     *
     * For full-stage roles (`kVertex`, `kFragment`) this is a complete
     * shader; for snippet roles (`kEval`, `kShadow`, `kIntersect`,
     * `kDiscard`) it is the body of one named function.
     */
    virtual string_view get_source(string_view role) const = 0;

    /**
     * @brief Returns the function name declared by @p role's source.
     *
     * Used as both the composer's dispatch target and the include
     * filename (with ".glsl" suffix). Empty for full-stage roles, where
     * the source isn't dispatched by name.
     */
    virtual string_view get_fn_name(string_view role) const = 0;

    /**
     * @brief Stable per-class id for the pipeline cache.
     *
     * Visuals with their own raster shader return a `PipelineKey::*`
     * constant so two instances share a compiled pipeline. Sources
     * that only contribute snippets (no full stages) can leave the
     * default 0; the consuming material's auto-allocated handle drives
     * the cache key in that case.
     */
    virtual uint64_t get_pipeline_key() const = 0;

    /**
     * @brief Hook for sources that depend on additional shader includes.
     *
     * The consumer calls this once when the class is first encountered,
     * before compiling any pipeline that references it. Implementations
     * call `ctx.shaders().register_include(name, src)` for each dependency.
     * Idempotent.
     */
    virtual void register_includes(IRenderContext& ctx) const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_SHADER_SOURCE_H
