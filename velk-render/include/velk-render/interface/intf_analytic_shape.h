#ifndef VELK_RENDER_INTF_ANALYTIC_SHAPE_H
#define VELK_RENDER_INTF_ANALYTIC_SHAPE_H

#include <velk/interface/intf_metadata.h>
#include <velk/string_view.h>

#include <cstdint>

namespace velk {

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
     * @brief Returns the shape kind the RT compute shader dispatches on.
     *
     *   0 = rect (planar quad, u_axis x v_axis)
     *   1 = cube (oriented box, u_axis x v_axis x w_axis)
     *   2 = sphere (centered in the element's bounding box, radius from size)
     */
    virtual uint32_t get_shape_kind() const = 0;

    /**
     * @brief Returns an optional GLSL snippet for a custom intersect
     *        routine. Empty for visuals that use the built-in
     *        shape-kind dispatch without a custom body.
     */
    virtual string_view get_shape_intersect_source() const { return {}; }
};

} // namespace velk

#endif // VELK_RENDER_INTF_ANALYTIC_SHAPE_H
