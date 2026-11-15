#ifndef VELK_RENDER_INTF_ANALYTIC_SHAPE_H
#define VELK_RENDER_INTF_ANALYTIC_SHAPE_H

#include <velk/interface/intf_metadata.h>
#include <velk/string_view.h>

#include <cstdint>

namespace velk {

class IRenderContext;


/**
 * @brief Contract for analytic primitive shapes, which rays test a visual
 *        drawn by its own pass as (see ray_walk.h).
 *
 * Built-in primitives (rect / cube / sphere) are enumerated by
 * `get_shape_kind()` and dispatched in the shaders that trace rays.
 *
 * Visuals that ship a one-off intersect (rounded box, capsule, SDF
 * blob) implement `IShaderSource` alongside `IAnalyticShape` and
 * supply the snippet under role `shader_role::kIntersect`. The
 * composers query the registry for active intersect snippets and
 * splice each into the shape-kind dispatch switch.
 */
class IAnalyticShape : public Interface<IAnalyticShape>
{
public:
    /**
     * @brief Identifies the primitive kind rays test the visual as.
     *
     * Built-in kinds:
     *   0 = rect (planar quad)
     *   1 = cube (oriented box)
     *   2 = sphere
     *
     * Visuals that ship a custom intersect snippet return one of
     * these (usually 0 for rect-based variants like a rounded
     * rectangle or a text glyph quad), since the shape the snippet is
     * given is built from the instance the same way (RtShape). The
     * composers replace the built-in dispatch with one that also calls
     * the custom intersect for visuals that registered a snippet.
     */
    virtual uint32_t get_shape_kind() const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_ANALYTIC_SHAPE_H
