#include "stack.h"

#include <velk/api/state.h>
#include <velk/interface/intf_hierarchy.h>

#include <algorithm>

namespace velk::ui {

namespace {

float& main_of(size& s, bool vertical) { return vertical ? s.height : s.width; }
float main_of(const size& s, bool vertical) { return vertical ? s.height : s.width; }
float cross_of(const size& s, bool vertical) { return vertical ? s.width : s.height; }
bool fills_main(const LayoutMeasure& m, bool vertical) { return vertical ? m.fill_y : m.fill_x; }

struct Child
{
    IElement::Ptr element;
    LayoutMeasure measure;
};

vector<Child> measure_children(IElement& element, const size& available, ILayoutContext& ctx)
{
    vector<Child> out;
    auto* obj = interface_cast<IObject>(&element);
    if (!obj) {
        return out;
    }
    for (auto& child : ctx.hierarchy().children_of(obj->get_self())) {
        if (auto e = interface_pointer_cast<IElement>(child)) {
            out.push_back({e, ctx.measure(*e, available)});
        }
    }
    return out;
}

} // namespace

size Stack::measure(const size& available, IElement& element, ILayoutContext& ctx)
{
    auto state = read_state<IStack>(this);
    if (!state) {
        return {};
    }
    const bool vertical = state->axis == 1;
    const auto children = measure_children(element, available, ctx);

    // A child that fills along the axis takes what is left, so the stack then wants all of it.
    float main = 0.f;
    float cross = 0.f;
    bool any_fill = false;
    for (auto& c : children) {
        if (fills_main(c.measure, vertical)) {
            any_fill = true;
        } else {
            main += main_of(c.measure.desired, vertical);
        }
        cross = std::max(cross, cross_of(c.measure.desired, vertical));
    }
    if (children.size() > 1) {
        main += state->spacing * static_cast<float>(children.size() - 1);
    }
    if (any_fill) {
        main = std::max(main, main_of(available, vertical));
    }

    size out{};
    main_of(out, vertical) = main;
    (vertical ? out.width : out.height) = cross;
    return out;
}

void Stack::arrange(const aabb& content, IElement& element, ILayoutContext& ctx)
{
    auto state = read_state<IStack>(this);
    if (!state) {
        return;
    }
    const bool vertical = state->axis == 1;
    const auto children = measure_children(element, content.extent, ctx);
    if (children.empty()) {
        return;
    }

    float fixed = state->spacing * static_cast<float>(children.size() - 1);
    size_t fill_count = 0;
    for (auto& c : children) {
        if (fills_main(c.measure, vertical)) {
            ++fill_count;
        } else {
            fixed += main_of(c.measure.desired, vertical);
        }
    }
    const float left = std::max(0.f, main_of(content.extent, vertical) - fixed);
    const float share = fill_count ? left / static_cast<float>(fill_count) : 0.f;

    // Along the axis every child gets exactly its size, so only the cross axis aligns.
    const Alignment cross_align = state->align == Alignment::Unset ? Alignment::Fill : state->align;
    float cursor = vertical ? content.position.y : content.position.x;
    for (auto& c : children) {
        const float main = fills_main(c.measure, vertical) ? share : main_of(c.measure.desired, vertical);
        aabb slot = content;
        if (vertical) {
            slot.position.y = cursor;
            slot.extent.height = main;
        } else {
            slot.position.x = cursor;
            slot.extent.width = main;
        }
        ctx.arrange(*c.element, slot, vertical ? cross_align : Alignment::Start,
                    vertical ? Alignment::Start : cross_align);
        cursor += main + state->spacing;
    }
}

} // namespace velk::ui
