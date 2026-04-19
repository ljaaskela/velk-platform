#ifndef VELK_RENDER_INTF_ANALYTIC_SHAPE_H
#define VELK_RENDER_INTF_ANALYTIC_SHAPE_H

#include <velk/interface/intf_metadata.h>
#include <velk/string_view.h>

#include <cstdint>

namespace velk {

class IRenderContext;


/**
 * @brief Contract for analytic primitive shapes consumed by the RT
 *        path's shape-kind dispatch.
 *
 * Built-in primitives (rect / cube / sphere) are enumerated by
 * `get_shape_kind()` and dispatched in the RT compute prelude.
 * `get_shape_intersect_source()` lets a visual contribute a one-off
 * intersect function (rounded box, capsule, SDF blob) without
 * bloating the prelude.
 */
class IAnalyticShape : public Interface<IAnalyticShape>
{
public:
    /**
     * @brief Identifies the primitive kind for the RT shape buffer.
     *
     * Built-in kinds reserved by the RT prelude:
     *   0 = rect (planar quad)
     *   1 = cube (oriented box)
     *   2 = sphere
     *
     * Visuals that ship a custom intersect snippet return one of
     * these (usually 0 for rect-based variants like a rounded
     * rectangle or a text glyph quad) so the emitted shape data fits
     * the same `RtShape` layout. The renderer's composer replaces the
     * built-in dispatch with one that also calls the custom intersect
     * for shapes whose emitting visual registered a snippet.
     */
    virtual uint32_t get_shape_kind() const = 0;

    /**
     * @brief Returns the visual's custom intersect function body in
     *        GLSL, if any.
     *
     * When non-empty, the function must have the signature
     * `bool <fn_name>(Ray ray, RtShape shape, out RayHit hit)` where
     * `<fn_name>` is `get_snippet_fn_name()` below. The composer
     * dispatches to it instead of the built-in rect/cube/sphere
     * intersect for shapes this visual emitted.
     */
    virtual string_view get_shape_intersect_source() const { return {}; }

    /**
     * @brief Name of the intersect function defined in
     *        @ref get_shape_intersect_source. Ignored when the source
     *        is empty.
     */
    virtual string_view get_shape_intersect_fn_name() const { return {}; }

    /**
     * @brief Lets the visual register any shader includes its
     *        intersect snippet depends on (e.g. text coverage helpers)
     *        into @p ctx. Called by the composer right after the
     *        snippet itself is registered.
     */
    virtual void register_shape_intersect_includes(IRenderContext& /*ctx*/) const {}
};

} // namespace velk

#endif // VELK_RENDER_INTF_ANALYTIC_SHAPE_H
