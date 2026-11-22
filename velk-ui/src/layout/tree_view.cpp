#include "tree_view.h"

#include <velk/api/state.h>
#include <velk-render/interface/intf_render_context.h>

#include <algorithm>

namespace velk::ui {

namespace {

// The built-in row: the item's `label` as text, centered in the row.
constexpr string_view kDefaultRow = R"({
  "version": 1,
  "objects": [
    { "id": "root", "class": "velk-scene.Element" },
    { "id": "label", "class": "velk-scene.Element" }
  ],
  "hierarchies": { "scene": { "root": ["label"] } },
  "attachments": [
    { "id": "row", "targets": ["root"], "class": "velk-ui.ListRow" },
    { "targets": ["label"], "class": "velk-text.TextVisual",
      "properties": { "text": { "bind": "row.item.label" }, "font_size": 14.0, "v_align": "center",
                      "color": { "r": 0.9, "g": 0.9, "b": 0.9, "a": 1.0 } } }
  ]
})";

} // namespace

IItemSource::Ptr TreeView::source() const
{
    auto state = read_state<ITreeView>(this);
    return state ? state->source.get<IItemSource>() : nullptr;
}

float TreeView::row_height() const
{
    auto state = read_state<ITreeView>(this);
    return state ? std::max(1.f, state->row_height) : 22.f;
}

float TreeView::indent() const
{
    auto state = read_state<ITreeView>(this);
    return state ? std::max(0.f, state->indent) : 16.f;
}

void TreeView::changed()
{
    invoke_trait_dirty(DirtyFlags::Layout | DirtyFlags::Visual);
}

void TreeView::on_state_changed(string_view name, IMetadata&, Uid interfaceId)
{
    constexpr string_view source_names[] = {"source"};
    if (has_state_changed<ITreeView>(interfaceId, name, source_names)) {
        watch_source();
    }
    constexpr string_view template_names[] = {"row_template"};
    if (has_state_changed<ITreeView>(interfaceId, name, template_names)) {
        if (auto state = read_state<ITreeView>(this)) {
            rows_.set_template(state->row_template);
        }
    }
    changed();
}

void TreeView::watch_source()
{
    // State writes name no property, so this runs for any of them: only a new source resets.
    auto s = source();
    if (s.get() == watched_.lock().get()) {
        return;
    }
    watched_ = s;
    source_changed_ = {};
    if (s) {
        source_changed_ = ScopedHandler(s->on_items_changed(), [this](const ItemChange& c) { on_items_changed(c); });
    }
    // A new source starts with its top level expanded.
    expanded_.clear();
    if (s) {
        for (size_t i = 0, n = s->child_count({}); i < n; ++i) {
            if (auto item = s->child_at({}, i)) {
                expanded_.insert(item.get());
            }
        }
    }
    rebuild();
}

void TreeView::append(IItemSource& s, const IObject::Ptr& item, uint32_t depth, vector<ShownRow>& out) const
{
    const bool has_children = s.has_children(item);
    const bool expanded = has_children && is_expanded(item);
    out.push_back({item, depth, has_children, expanded});
    if (expanded) {
        for (size_t i = 0, n = s.child_count(item); i < n; ++i) {
            append(s, s.child_at(item, i), depth + 1, out);
        }
    }
}

void TreeView::rebuild()
{
    flat_.clear();
    if (auto s = source()) {
        for (size_t i = 0, n = s->child_count({}); i < n; ++i) {
            append(*s, s->child_at({}, i), 0, flat_);
        }
    }
}

ptrdiff_t TreeView::row_of(const IObject::Ptr& parent) const
{
    if (!parent) {
        return -1;
    }
    for (size_t i = 0; i < flat_.size(); ++i) {
        if (flat_[i].item == parent) {
            return static_cast<ptrdiff_t>(i);
        }
    }
    return -2;
}

size_t TreeView::subtree_end(size_t row) const
{
    const uint32_t depth = flat_[row].depth;
    size_t i = row + 1;
    while (i < flat_.size() && flat_[i].depth > depth) {
        ++i;
    }
    return i;
}

size_t TreeView::child_start(ptrdiff_t parent_row, size_t index) const
{
    size_t i = parent_row < 0 ? 0 : static_cast<size_t>(parent_row) + 1;
    const size_t end = parent_row < 0 ? flat_.size() : subtree_end(static_cast<size_t>(parent_row));
    for (size_t child = 0; child < index && i < end; ++child) {
        i = subtree_end(i);
    }
    return i;
}

void TreeView::on_items_changed(const ItemChange& change)
{
    auto s = source();
    if (!s || change.type == ItemChange::Type::Reset) {
        rebuild();
        changed();
        return;
    }
    const ptrdiff_t parent_row = row_of(change.parent);
    if (parent_row == -2) {
        return; // Under an item that does not show.
    }
    if (parent_row >= 0 && !flat_[static_cast<size_t>(parent_row)].expanded) {
        // The parent's children do not show; only whether it has any can change. An item marked
        // expanded that gets its first child shows it.
        auto& p = flat_[static_cast<size_t>(parent_row)];
        p.has_children = s->has_children(p.item);
        p.expanded = p.has_children && is_expanded(p.item);
        if (p.expanded) {
            vector<ShownRow> rows;
            for (size_t i = 0, n = s->child_count(p.item); i < n; ++i) {
                append(*s, s->child_at(p.item, i), p.depth + 1, rows);
            }
            flat_.insert(flat_.begin() + parent_row + 1, rows.begin(), rows.end());
        }
        changed();
        return;
    }
    const uint32_t depth = parent_row < 0 ? 0 : flat_[static_cast<size_t>(parent_row)].depth + 1;
    const size_t at = child_start(parent_row, change.index);
    if (change.type == ItemChange::Type::Inserted) {
        vector<ShownRow> rows;
        for (size_t i = 0; i < change.count; ++i) {
            append(*s, s->child_at(change.parent, change.index + i), depth, rows);
        }
        flat_.insert(flat_.begin() + static_cast<ptrdiff_t>(at), rows.begin(), rows.end());
    } else {
        size_t end = at;
        for (size_t i = 0; i < change.count && end < flat_.size(); ++i) {
            end = subtree_end(end);
        }
        flat_.erase(flat_.begin() + static_cast<ptrdiff_t>(at), flat_.begin() + static_cast<ptrdiff_t>(end));
    }
    if (parent_row >= 0) {
        auto& p = flat_[static_cast<size_t>(parent_row)];
        p.has_children = s->has_children(p.item);
        p.expanded = p.has_children && is_expanded(p.item);
    }
    changed();
}

void TreeView::toggle(size_t row)
{
    auto s = source();
    auto& r = flat_[row];
    if (!s || !r.has_children) {
        return;
    }
    if (r.expanded) {
        expanded_.erase(r.item.get());
        r.expanded = false;
        flat_.erase(flat_.begin() + static_cast<ptrdiff_t>(row) + 1,
                    flat_.begin() + static_cast<ptrdiff_t>(subtree_end(row)));
    } else {
        expanded_.insert(r.item.get());
        r.expanded = true;
        vector<ShownRow> rows;
        for (size_t i = 0, n = s->child_count(r.item); i < n; ++i) {
            append(*s, s->child_at(r.item, i), r.depth + 1, rows);
        }
        flat_.insert(flat_.begin() + static_cast<ptrdiff_t>(row) + 1, rows.begin(), rows.end());
    }
    changed();
}

void TreeView::describe(LayoutBox& box) const
{
    box.height = dim::auto_size();
}

size TreeView::measure(const size& available, IElement&, ILayoutContext&)
{
    return {available.width, static_cast<float>(flat_.size()) * row_height(), 0.f};
}

void TreeView::arrange(const aabb& content, IElement& element, ILayoutContext& ctx)
{
    auto state = read_state<ITreeView>(this);
    const IObject::Ptr selected = state ? state->selected.get() : nullptr;
    const string uri = state ? state->row_template : string{};
    const float ind = indent();
    // Rows start after their indentation and the marker column.
    aabb inner = content;
    inner.position.x += ind;
    inner.extent.width = std::max(0.f, inner.extent.width - ind);
    const bool range_changed = rows_.arrange(
        inner, element, ctx, flat_.size(), row_height(), ind, selected, [&](size_t i) { return flat_[i]; }, uri,
        kDefaultRow);
    if (range_changed) {
        invoke_trait_dirty(DirtyFlags::Visual);
    }
}

vector<DrawEntry> TreeView::get_draw_entries(::velk::IRenderContext& ctx, const ::velk::size&)
{
    vector<DrawEntry> out;
    auto state = read_state<ITreeView>(this);
    auto visual = read_state<IVisual2D>(this);
    if (!state || !visual) {
        return out;
    }
    const float rh = row_height();
    const float ind = indent();
    const float left = rows_.content().position.x - ind; // The marker column of depth 0.
    const float width = rows_.content().extent.width + ind;

    rect r;
    if (rows_.slot_of(state->selected.get(), rh, r)) {
        out.push_back(row_quad(ctx, {left, r.y, width, r.height}, visual->color));
    }

    // + or - in the marker column of each shown row with children.
    const float size = std::max(4.f, std::min(ind, rh) * 0.5f);
    const float bar = std::max(1.f, size * 0.2f);
    rows_.for_each_shown([&](size_t index, const IListRow::Ptr&) {
        if (index >= flat_.size() || !flat_[index].has_children) {
            return;
        }
        const auto& row = flat_[index];
        const float cx = left + ind * static_cast<float>(row.depth) + ind * 0.5f;
        const float cy = rows_.content().position.y + static_cast<float>(index) * rh + rh * 0.5f;
        out.push_back(row_quad(ctx, {cx - size * 0.5f, cy - bar * 0.5f, size, bar}, state->marker_color));
        if (!row.expanded) {
            out.push_back(row_quad(ctx, {cx - bar * 0.5f, cy - size * 0.5f, bar, size}, state->marker_color));
        }
    });
    return out;
}

InputResult TreeView::on_pointer_event(PointerEvent& event)
{
    if (event.action != PointerAction::Down || event.button != PointerButton::Left) {
        return InputResult::Ignored;
    }
    const ptrdiff_t index = rows_.index_at(event.local_position.y, row_height());
    if (index < 0 || static_cast<size_t>(index) >= flat_.size()) {
        return InputResult::Ignored;
    }
    const auto& row = flat_[static_cast<size_t>(index)];
    const float ind = indent();
    const float marker = rows_.content().position.x - ind + ind * static_cast<float>(row.depth);
    if (row.has_children && event.local_position.x >= marker && event.local_position.x < marker + ind) {
        toggle(static_cast<size_t>(index));
        return InputResult::Consumed;
    }
    auto item = row.item;
    write_state<ITreeView>(this, [&](ITreeView::State& st) { set_object_ref(st.selected, item); });
    return InputResult::Consumed;
}

} // namespace velk::ui
