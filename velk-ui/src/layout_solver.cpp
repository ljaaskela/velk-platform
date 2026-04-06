#include "layout_solver.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-ui/api/element.h>
#include <velk-ui/interface/intf_layout_trait.h>
#include <velk-ui/interface/intf_transform_trait.h>

#ifdef VELK_LAYOUT_DEBUG
#define LAYOUT_LOG(...) VELK_LOG(I, __VA_ARGS__)
#else
#define LAYOUT_LOG(...) ((void)0)
#endif

namespace velk::ui {

void LayoutSolver::solve(IHierarchy& hierarchy, const aabb& viewport)
{
    VELK_PERF_SCOPE("layout.solve");
    if (auto root = interface_pointer_cast<IElement>(hierarchy.root())) {
        solve_element(hierarchy, root, viewport, mat4::identity());
    }
}

void LayoutSolver::solve_element(IHierarchy& hierarchy, const IElement::Ptr& element,
                                 const aabb& parent_bounds, const mat4& parent_world)
{
    // Collect layout and transform traits, split by phase
    Element e(element);
    auto layouts = e.find_attachments<ILayoutTrait>();
    auto transform_traits = e.find_attachments<ITransformTrait>();
    vector<ILayoutTrait*> layout_traits;     // TraitPhase::Layout (e.g. Stack)
    vector<ILayoutTrait*> constraint_traits; // TraitPhase::Constraint (e.g. FixedSize)
    for (auto&& l : layouts) {
        auto* lt = l.get();
        if (has_phase(lt, TraitPhase::Layout)) {
            layout_traits.push_back(lt);
        }
        if (has_phase(lt, TraitPhase::Constraint)) {
            constraint_traits.push_back(lt);
        }
    }

    // Available space from parent
    Constraint c;
    c.bounds = parent_bounds;

    // Layout phase: measure + apply
    for (auto* lt : layout_traits) {
        c = lt->measure(c, *element, hierarchy);
    }
    for (auto* lt : layout_traits) {
        lt->apply(c, *element, hierarchy);
    }

    // Constraint phase: measure + apply
    for (auto* lt : constraint_traits) {
        c = lt->measure(c, *element, hierarchy);
    }

    // Write size from constraint bounds
    write_state<IElement>(element, [&](IElement::State& s) { s.size = c.bounds.extent; });

    for (auto* lt : constraint_traits) {
        lt->apply(c, *element, hierarchy);
    }

    // Compute base world matrix from layout position
    auto reader = read_state<IElement>(element);
    if (!reader) {
        return;
    }

    mat4 world = parent_world * mat4::translate(reader->position);
    write_state<IElement>(element, [&](IElement::State& s) { s.world_matrix = world; });

    // Run transform traits
    for (auto&& tt : transform_traits) {
        tt->transform(*element);
    }

    // Re-read world matrix after transforms
    world = reader->world_matrix;

    // Recurse into children
    auto children = hierarchy.children_of(as_object(element));
    for (auto& child : children) {
        auto child_elem = interface_pointer_cast<IElement>(child);
        if (!child_elem) {
            continue;
        }

        aabb child_bounds;
        if (!layout_traits.empty()) {
            // A Layout trait (e.g. Stack) positioned this child; use the size it assigned
            auto child_state = read_state<IElement>(child_elem);
            if (child_state) {
                child_bounds.extent.width = child_state->size.width;
                child_bounds.extent.height = child_state->size.height;
            }
        } else {
            // No layout trait on parent; child fills the parent bounds
            child_bounds.extent.width = reader->size.width;
            child_bounds.extent.height = reader->size.height;
        }

        solve_element(hierarchy, child_elem, child_bounds, world);
    }
}

} // namespace velk::ui
