#include "layout_solver.h"

#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>

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
    auto root = interface_pointer_cast<IElement>(hierarchy.root());
    if (!root) {
        return;
    }

    solve_element(hierarchy, root, viewport, mat4::identity());
}

void LayoutSolver::solve_element(IHierarchy& hierarchy, const IElement::Ptr& element,
                                 const aabb& parent_bounds, const mat4& parent_world)
{
    // Collect layout and transform traits
    vector<ILayoutTrait*> layout_traits;
    vector<ITransformTrait*> transform_traits;
    auto* storage = interface_cast<IObjectStorage>(element);
    if (storage) {
        for (size_t i = 0; i < storage->attachment_count(); ++i) {
            auto att = storage->get_attachment(i);
            auto* lt = interface_cast<ILayoutTrait>(att);
            if (lt) {
                layout_traits.push_back(lt);
            }
            auto* tt = interface_cast<ITransformTrait>(att);
            if (tt) {
                transform_traits.push_back(tt);
            }
        }
    }

    // Available space from parent
    Constraint c;
    c.bounds = parent_bounds;

    // Layout phase: measure + apply
    for (auto* lt : layout_traits) {
        if (has_phase(lt, TraitPhase::Layout)) {
            c = lt->measure(c, *element, hierarchy);
        }
    }
    for (auto* lt : layout_traits) {
        if (has_phase(lt, TraitPhase::Layout)) {
            lt->apply(c, *element, hierarchy);
        }
    }

    // Constraint phase: measure + apply
    for (auto* lt : layout_traits) {
        if (has_phase(lt, TraitPhase::Constraint)) {
            c = lt->measure(c, *element, hierarchy);
        }
    }

    // Write size from constraint bounds
    write_state<IElement>(element, [&](IElement::State& s) { s.size = c.bounds.extent; });

    for (auto* lt : layout_traits) {
        if (has_phase(lt, TraitPhase::Constraint)) {
            lt->apply(c, *element, hierarchy);
        }
    }

    // Compute base world matrix from layout position
    auto reader = read_state<IElement>(element);
    if (!reader) {
        return;
    }

    mat4 world = parent_world * mat4::translate(reader->position);
    write_state<IElement>(element, [&](IElement::State& s) { s.world_matrix = world; });

    // Run transform traits
    for (auto* tt : transform_traits) {
        tt->transform(*element);
    }

    // Re-read world matrix after transforms
    world = reader->world_matrix;

    // Recurse: each child gets its own allocated bounds (set by this element's Stack)
    auto children = hierarchy.children_of(as_object(element));
    for (auto& child : children) {
        auto child_elem = interface_pointer_cast<IElement>(child);
        auto child_state = read_state<IElement>(child_elem);
        if (child_state) {
            aabb child_bounds;
            if (child_state && (child_state->size.width > 0.f || child_state->size.height > 0.f)) {
                // Parent's Stack already allocated this child's bounds
                child_bounds.extent.width = child_state->size.width;
                child_bounds.extent.height = child_state->size.height;
            } else {
                // No Stack positioned this child, it fills the parent
                child_bounds.extent.width = reader->size.width;
                child_bounds.extent.height = reader->size.height;
            }

            solve_element(hierarchy, child_elem, child_bounds, world);
        }
    }
}

} // namespace velk::ui
