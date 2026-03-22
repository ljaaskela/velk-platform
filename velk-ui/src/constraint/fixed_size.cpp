#include "fixed_size.h"

#include <velk/api/state.h>

#include <velk-ui/interface/intf_element.h>

namespace velk_ui {

ConstraintPhase FixedSize::get_phase() const
{
    return ConstraintPhase::Constraint;
}

Constraint FixedSize::measure(const Constraint& c, IElement& element, velk::IHierarchy*)
{
    auto state = velk::read_state<IFixedSize>(this);
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

    return result;
}

void FixedSize::apply(const Constraint& c, IElement& element, velk::IHierarchy*)
{
    velk::write_state<IElement>(&element, [&](IElement::State& s) {
        s.size.width = c.bounds.extent.width;
        s.size.height = c.bounds.extent.height;
    });
}

} // namespace velk_ui
