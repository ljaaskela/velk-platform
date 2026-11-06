#include "layout_solver.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>

#include <algorithm>
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

namespace {

IElement::Ptr to_ptr(IElement* element)
{
    auto* object = interface_cast<IObject>(element);
    return object ? object->get_self<IElement>() : IElement::Ptr{};
}

void sort_unique(vector<IElement*>& elements)
{
    std::sort(elements.begin(), elements.end());
    elements.resize(static_cast<size_t>(std::unique(elements.begin(), elements.end()) - elements.begin()));
}

bool has_layout_phase_trait(const Element& element)
{
    for (auto&& trait : element.find_attachments<ILayoutTrait>()) {
        if (has_phase(trait.get(), TraitPhase::Layout)) {
            return true;
        }
    }
    return false;
}

/// A size change under a parent with a Layout-phase trait can affect that
/// parent and its other children, so solving starts from the topmost such
/// parent. A parent without one is a boundary: its children fill its bounds and
/// do not affect its size.
IElement::Ptr find_layout_root(IHierarchy& hierarchy, IElement::Ptr element)
{
    while (auto parent = interface_pointer_cast<IElement>(hierarchy.parent_of(as_object(element)))) {
        if (!has_layout_phase_trait(Element(parent))) {
            break;
        }
        element = parent;
    }
    return element;
}

/// Whether any ancestor of @p element is in the sorted list @p elements.
bool has_ancestor_in(IHierarchy& hierarchy, IElement::Ptr element, const vector<IElement*>& elements)
{
    if (elements.empty()) {
        return false;
    }
    while (auto parent = interface_pointer_cast<IElement>(hierarchy.parent_of(as_object(element)))) {
        if (std::binary_search(elements.begin(), elements.end(), parent.get())) {
            return true;
        }
        element = parent;
    }
    return false;
}

mat4 parent_world_of(IHierarchy& hierarchy, const IElement::Ptr& element)
{
    auto parent = interface_pointer_cast<IElement>(hierarchy.parent_of(as_object(element)));
    auto state = read_state<IElement>(parent);
    return state ? state->world_matrix : mat4::identity();
}

mat4 compute_world(const Element& traits, IElement& element, const vec3& position, const mat4& parent_world)
{
    mat4 world = parent_world * mat4::translate(position);
    for (auto&& trait : traits.find_attachments<ITransformTrait>()) {
        world = trait->transform(element, world);
    }
    return world;
}

/// Bounds of the element's own box and visuals. Visuals can extend past the
/// layout box (text overflow, shadows, outlines).
void update_bounds(const Element& traits, const IElement::Ptr& element)
{
    auto reader = read_state<IElement>(element);
    if (!reader) {
        return;
    }
    const mat4& world = reader->world_matrix;
    aabb bounds = aabb::from_size(reader->size).transformed(world);
    for (auto&& vis : traits.find_attachments<IVisual>()) {
        if (auto* v = vis.get()) {
            bounds = aabb::merge(bounds, v->get_local_bounds(reader->size).transformed(world));
        }
    }
    if (reader->world_aabb != bounds) {
        write_state<IElement>(element, [&](IElement::State& s) { s.world_aabb = bounds; });
    }
}

/// Returns whether the world matrix changed.
bool write_world(const IElement::Ptr& element, const mat4& world)
{
    auto reader = read_state<IElement>(element);
    if (!reader || reader->world_matrix == world) {
        return false;
    }
    write_state<IElement>(element, [&](IElement::State& s) { s.world_matrix = world; });
    element->mark_dirty(DirtyFlags::Transform);
    return true;
}

} // namespace

void LayoutSolver::solve(IHierarchy& hierarchy, const aabb& viewport, const PendingLayout& pending)
{
    if (pending.empty()) {
        return;
    }
    VELK_PERF_SCOPE("layout.solve");

    if (pending.full) {
        if (auto root = interface_pointer_cast<IElement>(hierarchy.root())) {
            solve_element(hierarchy, root, viewport, mat4::identity());
        }
    } else {
        vector<IElement*> layout_roots;
        layout_roots.reserve(pending.layout.size());
        for (auto* element : pending.layout) {
            if (auto ptr = to_ptr(element)) {
                layout_roots.push_back(find_layout_root(hierarchy, ptr).get());
            }
        }
        sort_unique(layout_roots);

        // A subtree solve covers everything below it.
        for (auto* element : layout_roots) {
            auto ptr = to_ptr(element);
            if (ptr && !has_ancestor_in(hierarchy, ptr, layout_roots)) {
                solve_from_parent(hierarchy, ptr, viewport);
            }
        }

        vector<IElement*> transforms = pending.transform;
        sort_unique(transforms);
        for (auto* element : transforms) {
            auto ptr = to_ptr(element);
            if (!ptr || std::binary_search(layout_roots.begin(), layout_roots.end(), element)
                || has_ancestor_in(hierarchy, ptr, layout_roots)) {
                continue;
            }
            update_transform(hierarchy, ptr, parent_world_of(hierarchy, ptr));
        }

        for (auto* element : pending.visual) {
            if (auto ptr = to_ptr(element)) {
                update_bounds(Element(ptr), ptr);
            }
        }
    }

    // After everything else, since these read other elements' matrices.
    for (auto* element : pending.dependents) {
        if (auto ptr = to_ptr(element)) {
            update_transform(hierarchy, ptr, parent_world_of(hierarchy, ptr));
        }
    }
}

void LayoutSolver::solve_from_parent(IHierarchy& hierarchy, const IElement::Ptr& element, const aabb& viewport)
{
    auto parent = interface_pointer_cast<IElement>(hierarchy.parent_of(as_object(element)));
    auto parent_state = read_state<IElement>(parent);
    if (!parent_state) {
        solve_element(hierarchy, element, viewport, mat4::identity());
        return;
    }

    // find_layout_root stops at a parent with no Layout-phase trait, so the
    // child fills the parent's bounds.
    aabb bounds;
    bounds.extent.width = parent_state->size.width;
    bounds.extent.height = parent_state->size.height;
    solve_element(hierarchy, element, bounds, parent_state->world_matrix);
}

void LayoutSolver::update_transform(IHierarchy& hierarchy, const IElement::Ptr& element, const mat4& parent_world)
{
    auto reader = read_state<IElement>(element);
    if (!reader) {
        return;
    }

    Element e(element);
    const mat4 world = compute_world(e, *element, reader->position, parent_world);
    if (!write_world(element, world)) {
        return;
    }
    update_bounds(e, element);

    for (auto& child : hierarchy.children_of(as_object(element))) {
        if (auto child_elem = interface_pointer_cast<IElement>(child)) {
            update_transform(hierarchy, child_elem, world);
        }
    }
}

void LayoutSolver::solve_element(IHierarchy& hierarchy, const IElement::Ptr& element,
                                 const aabb& parent_bounds, const mat4& parent_world)
{
    // Collect layout traits, split by phase
    Element e(element);
    auto layouts = e.find_attachments<ILayoutTrait>();
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

    if (reader->size != c.bounds.extent) {
        write_state<IElement>(element, [&](IElement::State& s) { s.size = c.bounds.extent; });
        element->mark_dirty(DirtyFlags::Layout);
    }

    for (auto* lt : constraint_traits) {
        lt->apply(c, *element, hierarchy);
    }

    const mat4 world = compute_world(e, *element, reader->position, parent_world);
    write_world(element, world);
    update_bounds(e, element);

    for (auto& child : hierarchy.children_of(as_object(element))) {
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
