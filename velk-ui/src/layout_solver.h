#ifndef VELK_UI_LAYOUT_SOLVER_H
#define VELK_UI_LAYOUT_SOLVER_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_hierarchy.h>

#include <velk-scene/interface/intf_element.h>
#include <velk-scene/interface/intf_scene.h>

namespace velk::ui {

/**
 * @brief Resolves element sizes, world matrices and bounds.
 *
 * Solves only what @p pending says changed, so the cost follows the changed
 * elements and their descendants rather than the size of the scene. Every
 * element whose world matrix or size changes is marked dirty, which is how the
 * renderer learns about descendants moved by a parent.
 */
class LayoutSolver
{
public:
    void solve(IHierarchy& hierarchy, const aabb& viewport, const PendingLayout& pending);

private:
    /** @brief Runs layout and transforms for an element and all its descendants. */
    void solve_element(IHierarchy& hierarchy, const IElement::Ptr& element, const aabb& parent_bounds,
                       const mat4& parent_world);

    /** @brief Solves @p element given its parent's current state. */
    void solve_from_parent(IHierarchy& hierarchy, const IElement::Ptr& element, const aabb& viewport);

    /** @brief Updates a world matrix, descending only while the result changes. */
    void update_transform(IHierarchy& hierarchy, const IElement::Ptr& element, const mat4& parent_world);
};

} // namespace velk::ui

#endif // VELK_UI_LAYOUT_SOLVER_H
