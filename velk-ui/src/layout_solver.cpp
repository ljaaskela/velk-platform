#include "layout_solver.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>

#include <algorithm>
#include <velk-scene/api/element.h>
#include <velk-scene/interface/intf_transform_trait.h>
#include <velk-scene/interface/intf_visual.h>
#include <velk-ui/interface/intf_content_size.h>

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

/// An element's layout traits: its box and the trait that places its children, if any.
struct Traits
{
    vector<ILayoutTrait::Ptr> attached;
    LayoutBox box;
    ILayoutTrait* layout = nullptr;
};

Traits collect(const Element& element)
{
    Traits t;
    t.attached = element.find_attachments<ILayoutTrait>();
    for (auto& trait : t.attached) {
        auto* lt = trait.get();
        if (has_phase(lt, TraitPhase::Constraint)) {
            lt->describe(t.box);
        }
        if (!t.layout && has_phase(lt, TraitPhase::Layout)) {
            t.layout = lt;
        }
    }
    return t;
}

bool is_auto(const LayoutBox& box)
{
    return box.width.unit == DimUnit::Auto || box.height.unit == DimUnit::Auto;
}

/// True when @p element's size or its children's placement follows its children.
bool depends_on_children(const Element& element)
{
    const auto t = collect(element);
    return t.layout || is_auto(t.box);
}

struct Edges
{
    float left = 0.f;
    float top = 0.f;
    float right = 0.f;
    float bottom = 0.f;
};

float resolve_inset(dim d, float space)
{
    switch (d.unit) {
    case DimUnit::Px:
        return d.value;
    case DimUnit::Pct:
        return d.value * space;
    default:
        return 0.f;
    }
}

Edges resolve(const Insets& insets, const size& space)
{
    return {resolve_inset(insets.left, space.width), resolve_inset(insets.top, space.height),
            resolve_inset(insets.right, space.width), resolve_inset(insets.bottom, space.height)};
}

size shrink(const size& s, const Edges& e)
{
    return {std::max(0.f, s.width - e.left - e.right), std::max(0.f, s.height - e.top - e.bottom), s.depth};
}

/// The size on one axis: the space offered, fixed, or the content plus padding.
float axis_size(dim d, float offered, float content)
{
    switch (d.unit) {
    case DimUnit::Px:
    case DimUnit::Pct:
        return resolve_dim(d, offered);
    case DimUnit::Auto:
        return content;
    default:
        return offered;
    }
}

float align_factor(Alignment a)
{
    switch (a) {
    case Alignment::Center:
        return 0.5f;
    case Alignment::End:
        return 1.f;
    default:
        return 0.f;
    }
}

mat4 compute_world(const Element& traits, IElement& element, const vec3& position, const mat4& parent_world)
{
    mat4 world = parent_world * mat4::translate(position);
    for (auto&& trait : traits.find_attachments<ITransformTrait>()) {
        world = trait->transform(element, world);
    }
    return world;
}

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

mat4 world_of(const IElement::Ptr& element)
{
    auto state = read_state<IElement>(element);
    return state ? state->world_matrix : mat4::identity();
}

} // namespace

void LayoutSolver::solve(IHierarchy& hierarchy, const aabb& viewport, const PendingLayout& pending)
{
    if (pending.empty()) {
        return;
    }
    VELK_PERF_SCOPE("layout.solve");
    hierarchy_ = &hierarchy;

    const auto parent_of = [&](const IElement::Ptr& e) {
        return interface_pointer_cast<IElement>(hierarchy.parent_of(as_object(e)));
    };
    const auto has_ancestor_in = [&](IElement::Ptr e, const vector<IElement*>& set) {
        while (auto parent = parent_of(e)) {
            if (std::binary_search(set.begin(), set.end(), parent.get())) {
                return true;
            }
            e = parent;
        }
        return false;
    };

    vector<IElement*> layout_roots;
    if (pending.full) {
        if (auto root = interface_pointer_cast<IElement>(hierarchy.root())) {
            arrange_from_parent(root, viewport);
        }
    } else {
        // An element whose content size changed lays out again when its size follows its content.
        vector<IElement*> changed = pending.layout;
        for (auto* element : pending.visual) {
            if (auto ptr = to_ptr(element); ptr && is_auto(collect(Element(ptr)).box)) {
                changed.push_back(element);
            }
        }

        // Climb to the highest ancestor whose layout depends on the changed element.
        layout_roots.reserve(changed.size());
        for (auto* element : changed) {
            auto ptr = to_ptr(element);
            if (!ptr) {
                continue;
            }
            while (auto parent = parent_of(ptr)) {
                if (!depends_on_children(Element(parent))) {
                    break;
                }
                ptr = parent;
            }
            layout_roots.push_back(ptr.get());
        }
        sort_unique(layout_roots);

        for (auto* element : layout_roots) {
            auto ptr = to_ptr(element);
            if (ptr && !has_ancestor_in(ptr, layout_roots)) {
                arrange_from_parent(ptr, viewport);
            }
        }

        vector<IElement*> transforms = pending.transform;
        sort_unique(transforms);
        for (auto* element : transforms) {
            auto ptr = to_ptr(element);
            if (!ptr || std::binary_search(layout_roots.begin(), layout_roots.end(), element)
                || has_ancestor_in(ptr, layout_roots)) {
                continue;
            }
            update_transform(ptr, world_of(parent_of(ptr)));
        }

        for (auto* element : pending.visual) {
            if (auto ptr = to_ptr(element)) {
                update_bounds(Element(ptr), ptr);
            }
        }
    }

    for (auto* element : pending.dependents) {
        if (auto ptr = to_ptr(element)) {
            update_transform(ptr, world_of(parent_of(ptr)));
        }
    }
    hierarchy_ = nullptr;
}

void LayoutSolver::arrange_from_parent(const IElement::Ptr& element, const aabb& viewport)
{
    auto parent = interface_pointer_cast<IElement>(hierarchy_->parent_of(as_object(element)));
    auto parent_state = read_state<IElement>(parent);
    if (!parent_state) {
        parent_world_ = mat4::identity();
        arrange(*element, viewport, Alignment::Start, Alignment::Start);
        return;
    }

    // The parent places no children itself (or the climb would have gone past it), so the
    // element sits in the parent's content rectangle.
    const auto traits = collect(Element(parent));
    const Edges pad = resolve(traits.box.padding, parent_state->size);
    aabb content;
    content.position = {pad.left, pad.top, 0.f};
    content.extent = shrink(parent_state->size, pad);
    parent_world_ = parent_state->world_matrix;
    arrange(*element, content, Alignment::Start, Alignment::Start);
}

size LayoutSolver::measure_content(IElement& element, ILayoutTrait* layout, const size& available)
{
    size content{};
    if (layout) {
        content = layout->measure(available, element, *this);
    } else if (auto* obj = interface_cast<IObject>(&element)) {
        for (auto& child : hierarchy_->children_of(obj->get_self())) {
            if (auto e = interface_pointer_cast<IElement>(child)) {
                const auto m = measure(*e, available);
                content.width = std::max(content.width, m.desired.width);
                content.height = std::max(content.height, m.desired.height);
            }
        }
    }
    for (auto&& visual : Element(to_ptr(&element)).find_attachments<IContentSize>()) {
        const size s = visual->content_size(available);
        content.width = std::max(content.width, s.width);
        content.height = std::max(content.height, s.height);
    }
    return content;
}

LayoutMeasure LayoutSolver::measure(IElement& element, const size& available)
{
    const auto traits = collect(Element(to_ptr(&element)));
    const LayoutBox& box = traits.box;
    const Edges margin = resolve(box.margin, available);
    const size outer = shrink(available, margin);
    const Edges pad = resolve(box.padding, outer);

    size content{};
    if (is_auto(box)) {
        // Fixed axes offer their own size to the content; the others offer what they are offered.
        size space = outer;
        if (box.width.unit == DimUnit::Px || box.width.unit == DimUnit::Pct) {
            space.width = resolve_dim(box.width, outer.width);
        }
        if (box.height.unit == DimUnit::Px || box.height.unit == DimUnit::Pct) {
            space.height = resolve_dim(box.height, outer.height);
        }
        content = measure_content(element, traits.layout, shrink(space, pad));
    }

    LayoutMeasure out;
    out.fill_x = box.width.unit == DimUnit::None;
    out.fill_y = box.height.unit == DimUnit::None;
    out.desired.width = axis_size(box.width, outer.width, content.width + pad.left + pad.right) + margin.left
                        + margin.right;
    out.desired.height = axis_size(box.height, outer.height, content.height + pad.top + pad.bottom) + margin.top
                         + margin.bottom;
    out.desired.depth = box.depth.unit == DimUnit::None ? 0.f : resolve_dim(box.depth, available.depth);
    return out;
}

void LayoutSolver::arrange(IElement& child, const aabb& slot, Alignment default_x, Alignment default_y)
{
    auto element = to_ptr(&child);
    auto reader = read_state<IElement>(element);
    if (!reader) {
        return;
    }
    Element e(element);
    const auto traits = collect(e);
    const LayoutBox& box = traits.box;

    const Edges margin = resolve(box.margin, slot.extent);
    const size outer = shrink(slot.extent, margin);
    const Edges pad = resolve(box.padding, outer);
    const Alignment ax = box.align_x != Alignment::Unset ? box.align_x : default_x;
    const Alignment ay = box.align_y != Alignment::Unset ? box.align_y : default_y;

    // Content is measured only for auto axes that are not stretched by Fill.
    const bool auto_x = box.width.unit == DimUnit::Auto && ax != Alignment::Fill;
    const bool auto_y = box.height.unit == DimUnit::Auto && ay != Alignment::Fill;
    size content{};
    if (auto_x || auto_y) {
        size space = outer;
        if (box.width.unit == DimUnit::Px || box.width.unit == DimUnit::Pct) {
            space.width = resolve_dim(box.width, outer.width);
        }
        if (box.height.unit == DimUnit::Px || box.height.unit == DimUnit::Pct) {
            space.height = resolve_dim(box.height, outer.height);
        }
        content = measure_content(child, traits.layout, shrink(space, pad));
    }

    size s;
    s.width = auto_x ? content.width + pad.left + pad.right
                     : axis_size(box.width.unit == DimUnit::Auto ? dim::none() : box.width, outer.width, 0.f);
    s.height = auto_y ? content.height + pad.top + pad.bottom
                      : axis_size(box.height.unit == DimUnit::Auto ? dim::none() : box.height, outer.height, 0.f);
    s.depth = box.depth.unit == DimUnit::None ? 0.f : resolve_dim(box.depth, slot.extent.depth);

    vec3 position;
    position.x = slot.position.x + margin.left + (outer.width - s.width) * align_factor(ax);
    position.y = slot.position.y + margin.top + (outer.height - s.height) * align_factor(ay);
    position.z = slot.position.z;

    const bool size_changed = reader->size != s;
    if (size_changed || reader->position != position) {
        write_state<IElement>(element, [&](IElement::State& st) {
            st.position = position;
            st.size = s;
        });
    }
    if (size_changed) {
        element->mark_dirty(DirtyFlags::Layout);
    }

    const mat4 world = compute_world(e, child, position, parent_world_);
    write_world(element, world);
    update_bounds(e, element);

    aabb inner;
    inner.position = {pad.left, pad.top, 0.f};
    inner.extent = shrink(s, pad);

    const mat4 saved = parent_world_;
    parent_world_ = world;
    if (traits.layout) {
        traits.layout->arrange(inner, child, *this);
    } else {
        for (auto& c : hierarchy_->children_of(as_object(element))) {
            if (auto ce = interface_pointer_cast<IElement>(c)) {
                arrange(*ce, inner, Alignment::Start, Alignment::Start);
            }
        }
    }
    parent_world_ = saved;
}

void LayoutSolver::update_transform(const IElement::Ptr& element, const mat4& parent_world)
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

    for (auto& child : hierarchy_->children_of(as_object(element))) {
        if (auto child_elem = interface_pointer_cast<IElement>(child)) {
            update_transform(child_elem, world);
        }
    }
}

} // namespace velk::ui
