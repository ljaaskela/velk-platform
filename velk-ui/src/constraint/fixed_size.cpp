#include "fixed_size.h"

#include <velk/api/state.h>

#include <velk-ui/interface/intf_element.h>

namespace velk::ui {

Constraint FixedSize::measure(const Constraint& c, IElement& element, IHierarchy&)
{
    auto state = read_state<IFixedSize>(this);
    if (!state) {
        return c;
    }

    Constraint result = c;
    float avail_w = c.bounds.extent.width;
    float avail_h = c.bounds.extent.height;

    if (state->width.unit != DimUnit::None) {
        result.bounds.extent.width = resolve_dim(state->width, avail_w);
    }
    if (state->height.unit != DimUnit::None) {
        result.bounds.extent.height = resolve_dim(state->height, avail_h);
    }
    // Depth resolves against 0 (pct would be meaningless for an axis the
    // parent hierarchy doesn't supply). px values pass through unchanged.
    if (state->depth.unit != DimUnit::None) {
        result.bounds.extent.depth = resolve_dim(state->depth, 0.f);
    }

    return result;
}

void FixedSize::apply(const Constraint& c, IElement& element, IHierarchy&)
{
    write_state<IElement>(&element, [&](IElement::State& s) {
        s.size.width = c.bounds.extent.width;
        s.size.height = c.bounds.extent.height;
        s.size.depth = c.bounds.extent.depth;
    });
}

} // namespace velk::ui
