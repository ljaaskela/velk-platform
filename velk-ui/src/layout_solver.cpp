#include "layout_solver.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-scene/api/element.h>
#include <velk-ui/interface/intf_layout_trait.h>
#include <velk-scene/interface/intf_transform_trait.h>
#include <velk-scene/interface/intf_visual.h>

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

    auto reader = read_state<IElement>(element);
    if (!reader) {
        return;
    }

    // Write size from constraint bounds — only if changed. A redundant
    // write would fire an on_state_changed("size") notification and
    // cascade Layout-dirty back to the scene, which would force every
    // visual into the redraw list the following frame.
    if (reader->size != c.bounds.extent) {
        write_state<IElement>(element, [&](IElement::State& s) { s.size = c.bounds.extent; });
    }

    for (auto* lt : constraint_traits) {
        lt->apply(c, *element, hierarchy);
    }

    // Compute base world matrix from layout position

    mat4 world = parent_world * mat4::translate(reader->position);
    if (!(reader->world_matrix == world)) {
        write_state<IElement>(element, [&](IElement::State& s) { s.world_matrix = world; });
    }

    // Run transform traits
    for (auto&& tt : transform_traits) {
        tt->transform(*element);
    }

    // Re-read world matrix after transforms
    world = reader->world_matrix;

    // Recurse into children. Each child writes its own world_aabb
    // during its solve; we merge them back into this element's
    // aggregate bounds afterwards. Visuals attached to this element
    // can extend past the layout box (text overflow, shadows,
    // outlines), so we query each via get_local_bounds and fold its
    // world-space bound in too.
    auto children = hierarchy.children_of(as_object(element));
    aabb combined = aabb::from_size(reader->size).transformed(world);
    for (auto&& vis : e.find_attachments<IVisual>()) {
        if (auto* v = vis.get()) {
            combined = aabb::merge(combined, v->get_local_bounds(reader->size).transformed(world));
        }
    }
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

        if (auto child_state = read_state<IElement>(child_elem)) {
            combined = aabb::merge(combined, child_state->world_aabb);
        }
    }

    if (reader->world_aabb != combined) {
        write_state<IElement>(element, [&](IElement::State& s) { s.world_aabb = combined; });
    }
}

} // namespace velk::ui
