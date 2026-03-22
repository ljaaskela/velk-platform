#include "layout_solver.h"

#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>

#include <algorithm>
#include <vector>
#include <velk-ui/interface/intf_constraint.h>
#include <velk-ui/interface/intf_element.h>

namespace velk_ui {

void LayoutSolver::solve(velk::IHierarchy& hierarchy, const velk::aabb& viewport)
{
    auto root = hierarchy.root();
    if (!root) {
        return;
    }

    solve_element(hierarchy, root, viewport, velk::mat4::identity());
}

void LayoutSolver::solve_element(velk::IHierarchy& hierarchy, const velk::IObject::Ptr& obj,
                                 const velk::aabb& parent_bounds, const velk::mat4& parent_world)
{
    auto* element = velk::interface_cast<IElement>(obj);
    if (!element) {
        return;
    }

    // Collect IConstraint attachments
    velk::vector<IConstraint*> constraints;
    auto* storage = velk::interface_cast<velk::IObjectStorage>(obj);
    if (storage) {
        for (size_t i = 0; i < storage->attachment_count(); ++i) {
            auto att = storage->get_attachment(i);
            auto* constraint = velk::interface_cast<IConstraint>(att);
            if (constraint) {
                constraints.push_back(constraint);
            }
        }
    }

    // Sort: Layout phase first, then Constraint phase
    std::sort(constraints.begin(), constraints.end(), [](IConstraint* a, IConstraint* b) {
        return static_cast<uint8_t>(a->get_phase()) < static_cast<uint8_t>(b->get_phase());
    });

    // Start with parent constraint (element fills parent by default)
    Constraint c;
    c.bounds = parent_bounds;

    // Run measure for each constraint
    for (auto* con : constraints) {
        c = con->measure(c, *element, &hierarchy);
    }

    // Run apply for each constraint
    for (auto* con : constraints) {
        con->apply(c, *element, &hierarchy);
    }

    // If no constraint wrote size, write from final constraint bounds
    {
        auto reader = velk::read_state<IElement>(element);
        if (reader && reader->size.width == 0.f && reader->size.height == 0.f) {
            velk::write_state<IElement>(element, [&](IElement::State& s) {
                s.size.width = c.bounds.extent.width;
                s.size.height = c.bounds.extent.height;
            });
        }
    }

    // Read current position and local transform
    auto reader = velk::read_state<IElement>(element);
    if (!reader) {
        return;
    }

    velk::vec3 pos = reader->position;
    velk::mat4 local = reader->local_transform;

    // Compute world_matrix = parent_world * translate(position) * local_transform
    velk::mat4 world = parent_world * velk::mat4::translate(pos) * local;

    velk::write_state<IElement>(element, [&](IElement::State& s) { s.world_matrix = world; });

    // Recurse into children with element's bounds as available space
    velk::aabb child_bounds;
    child_bounds.position = reader->position;
    child_bounds.extent.width = reader->size.width;
    child_bounds.extent.height = reader->size.height;

    auto children = hierarchy.children_of(obj);
    for (auto& child : children) {
        solve_element(hierarchy, child, child_bounds, world);
    }
}

} // namespace velk_ui
