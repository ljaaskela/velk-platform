#include "scroll_view.h"

#include <velk/api/state.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-scene/instance_types.h>

#include <algorithm>

namespace velk::ui {

namespace {

constexpr float kMinThumb = 16.f;

DrawEntry quad_entry(::velk::IRenderContext& ctx, float x, float y, float w, float h, ::velk::color col)
{
    DrawEntry entry{};
    entry.pipeline_key = PipelineKey::Default;
    if (auto quad = ctx.get_mesh_builder().get_rect_quad(); quad && !quad->get_primitives().empty()) {
        entry.primitive = quad->get_primitives()[0];
    }
    entry.bounds = {x, y, w, h};
    ElementInstance inst{};
    inst.offset = {x, y, 0.f, 0.f};
    inst.size = {w, h, 0.f, 0.f};
    inst.col = col;
    entry.set_instance(inst);
    return entry;
}

} // namespace

void ScrollView::on_state_changed(string_view, IMetadata&, Uid)
{
    invoke_trait_dirty(DirtyFlags::Layout | DirtyFlags::Visual);
}

size ScrollView::measure(const size& available, IElement&, ILayoutContext&)
{
    // The view takes the space it is given; its content scrolls inside it.
    return available;
}

void ScrollView::arrange(const aabb& content, IElement& element, ILayoutContext& ctx)
{
    auto state = read_state<IScrollView>(this);
    if (!state) {
        return;
    }
    vector<IElement::Ptr> children;
    if (auto* obj = interface_cast<IObject>(&element)) {
        for (auto& child : ctx.hierarchy().children_of(obj->get_self())) {
            if (auto e = interface_pointer_cast<IElement>(child)) {
                children.push_back(e);
            }
        }
    }
    auto measure_all = [&](float width, float height) {
        size need{};
        for (auto& c : children) {
            auto m = ctx.measure(*c, {width, height, content.extent.depth});
            need.width = std::max(need.width, m.desired.width);
            need.height = std::max(need.height, m.desired.height);
        }
        return need;
    };

    // A scroll alone moves the content: the last measure holds while the space, the bar width
    // and the children are the same and none of the children changed.
    const float bw = std::max(0.f, state->bar_width);
    vector<IElement*> ids;
    ids.reserve(children.size());
    bool stale = false;
    for (auto& c : children) {
        ids.push_back(c.get());
        stale = stale || ctx.changed(*c);
    }
    const bool reuse = !stale && measured_.valid && measured_.space == content.extent && measured_.bar_width == bw
                       && measured_.children == ids;

    float vw = content.extent.width;
    float vh = content.extent.height;
    size need{};
    bool v_bar = false;
    bool h_bar = false;
    if (reuse) {
        vw = measured_.vw;
        vh = measured_.vh;
        need = measured_.need;
        v_bar = measured_.v_bar;
        h_bar = measured_.h_bar;
    } else {
        // A bar takes its width from the content's space, which can make the other axis need one.
        need = measure_all(vw, vh);
        v_bar = need.height > vh;
        if (v_bar) {
            vw = std::max(0.f, vw - bw);
            need = measure_all(vw, vh);
        }
        h_bar = need.width > vw;
        if (h_bar) {
            vh = std::max(0.f, vh - bw);
            if (!v_bar && need.height > vh) {
                v_bar = true;
                vw = std::max(0.f, vw - bw);
                need = measure_all(vw, vh);
            }
        }
        measured_ = {true, content.extent, bw, std::move(ids), vw, vh, need, v_bar, h_bar};
    }

    const Axis old_x = x_;
    const Axis old_y = y_;
    x_ = {vw, std::max(need.width, vw), 0.f, content.position.x, h_bar};
    y_ = {vh, std::max(need.height, vh), 0.f, content.position.y, v_bar};
    x_.offset = std::clamp(state->scroll_x, 0.f, x_.max());
    y_.offset = std::clamp(state->scroll_y, 0.f, y_.max());
    v_bar_x_ = content.position.x + vw;
    h_bar_y_ = content.position.y + vh;

    for (auto& c : children) {
        aabb slot;
        slot.position = {content.position.x - x_.offset, content.position.y - y_.offset, content.position.z};
        slot.extent = {x_.content, y_.content, content.extent.depth};
        ctx.arrange(*c, slot, Alignment::Start, Alignment::Start);
    }

    // The bars are drawn from what arrange found.
    auto same = [](const Axis& a, const Axis& b) {
        return a.view == b.view && a.content == b.content && a.offset == b.offset && a.origin == b.origin
               && a.bar == b.bar;
    };
    if (!same(old_x, x_) || !same(old_y, y_)) {
        invoke_trait_dirty(DirtyFlags::Visual);
    }
}

rect ScrollView::clip_rect(const size& box) const
{
    return {0.f, 0.f, y_.bar ? v_bar_x_ : box.width, x_.bar ? h_bar_y_ : box.height};
}

ScrollView::Bar ScrollView::bar_of(const Axis& axis) const
{
    Bar bar;
    bar.track_start = axis.origin;
    bar.track_length = axis.view;
    const float ratio = axis.content > 0.f ? axis.view / axis.content : 1.f;
    bar.thumb_length = std::min(bar.track_length, std::max(kMinThumb, bar.track_length * ratio));
    const float travel = bar.track_length - bar.thumb_length;
    const float max = axis.max();
    bar.thumb_start = bar.track_start + (max > 0.f ? travel * axis.offset / max : 0.f);
    return bar;
}

vector<DrawEntry> ScrollView::get_draw_entries(::velk::IRenderContext& ctx, const ::velk::size&)
{
    vector<DrawEntry> out;
    auto state = read_state<IScrollView>(this);
    auto visual = read_state<IVisual2D>(this);
    if (!state || !visual) {
        return out;
    }
    const float bw = state->bar_width;
    const ::velk::color thumb = visual->color;
    const ::velk::color track{thumb.r, thumb.g, thumb.b, thumb.a * 0.25f};
    if (y_.bar) {
        auto b = bar_of(y_);
        out.push_back(quad_entry(ctx, v_bar_x_, b.track_start, bw, b.track_length, track));
        out.push_back(quad_entry(ctx, v_bar_x_, b.thumb_start, bw, b.thumb_length, thumb));
    }
    if (x_.bar) {
        auto b = bar_of(x_);
        out.push_back(quad_entry(ctx, b.track_start, h_bar_y_, b.track_length, bw, track));
        out.push_back(quad_entry(ctx, b.thumb_start, h_bar_y_, b.thumb_length, bw, thumb));
    }
    return out;
}

bool ScrollView::scroll_to(bool vertical, float offset)
{
    Axis& axis = vertical ? y_ : x_;
    const float value = std::clamp(offset, 0.f, axis.max());
    if (value == axis.offset) {
        return false;
    }
    axis.offset = value;
    write_state<IScrollView>(this, [&](IScrollView::State& s) { (vertical ? s.scroll_y : s.scroll_x) = value; });
    return true;
}

InputResult ScrollView::on_scroll_event(ScrollEvent& event)
{
    auto state = read_state<IScrollView>(this);
    const float step = event.unit == ScrollUnit::Lines ? (state ? state->wheel_step : 40.f) : 1.f;
    float dx = -event.delta.x * step;
    float dy = -event.delta.y * step;
    if (!!(event.modifiers & Modifier::Shift) && dx == 0.f) {
        dx = dy;
        dy = 0.f;
    }
    const bool moved_y = dy != 0.f && scroll_to(true, y_.offset + dy);
    const bool moved_x = dx != 0.f && scroll_to(false, x_.offset + dx);
    // Not moving lets an enclosing view scroll instead.
    return moved_x || moved_y ? InputResult::Consumed : InputResult::Ignored;
}

InputResult ScrollView::on_pointer_event(PointerEvent& event)
{
    const vec2 p = event.local_position;
    if (dragging_) {
        Axis& axis = drag_vertical_ ? y_ : x_;
        if (event.action == PointerAction::Move) {
            const Bar bar = bar_of(axis);
            const float travel = bar.track_length - bar.thumb_length;
            const float moved = (drag_vertical_ ? p.y : p.x) - drag_start_pointer_;
            if (travel > 0.f) {
                scroll_to(drag_vertical_, drag_start_offset_ + moved * axis.max() / travel);
            }
            return InputResult::Consumed;
        }
        if (event.action == PointerAction::Up || event.action == PointerAction::Cancel) {
            dragging_ = false;
            return InputResult::Consumed;
        }
    }
    if (event.action != PointerAction::Down || event.button != PointerButton::Left) {
        return InputResult::Ignored;
    }
    auto state = read_state<IScrollView>(this);
    const float bw = state ? state->bar_width : 0.f;

    auto press_bar = [&](bool vertical) {
        Axis& axis = vertical ? y_ : x_;
        const Bar bar = bar_of(axis);
        const float along = vertical ? p.y : p.x;
        if (along >= bar.thumb_start && along < bar.thumb_start + bar.thumb_length) {
            dragging_ = true;
            drag_vertical_ = vertical;
            drag_start_pointer_ = along;
            drag_start_offset_ = axis.offset;
            return InputResult::Captured;
        }
        // Outside the thumb: a page toward the pointer.
        scroll_to(vertical, axis.offset + (along < bar.thumb_start ? -axis.view : axis.view));
        return InputResult::Consumed;
    };
    if (y_.bar && p.x >= v_bar_x_ && p.x < v_bar_x_ + bw && p.y >= y_.origin && p.y < y_.origin + y_.view) {
        return press_bar(true);
    }
    if (x_.bar && p.y >= h_bar_y_ && p.y < h_bar_y_ + bw && p.x >= x_.origin && p.x < x_.origin + x_.view) {
        return press_bar(false);
    }
    return InputResult::Ignored;
}

} // namespace velk::ui
