#ifndef VELK_UI_LAYOUT_SOLVER_H
#define VELK_UI_LAYOUT_SOLVER_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_hierarchy.h>

#include <velk-scene/interface/intf_element.h>

namespace velk::ui {

class LayoutSolver
{
public:
    void solve(IHierarchy& hierarchy, const aabb& viewport);

private:
    void solve_element(IHierarchy& hierarchy, const IElement::Ptr& element, const aabb& parent_bounds,
                       const mat4& parent_world);
};

} // namespace velk::ui

#endif // VELK_UI_LAYOUT_SOLVER_H
