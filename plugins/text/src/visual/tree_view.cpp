#include "tree_view.h"

#include "text_visual.h"

#include <velk/api/property.h>
#include <velk/api/state.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-scene/instance_types.h>

#include <algorithm>
#include <functional>

namespace velk::ui {

namespace {

constexpr string_view kExpandedMarker = "\xe2\x88\x92"; // minus sign
constexpr string_view kCollapsedMarker = "+";
constexpr float kLabelGap = 4.f;

string label_of(const IObject::Ptr& object, string_view property)
{
    auto* meta = interface_cast<IMetadata>(object);
    auto prop = meta ? meta->get_property(property) : nullptr;
    return prop ? ConstProperty<string>(prop).get_value() : string();
}

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

void TreeView::on_state_changed(string_view name, IMetadata&, Uid interfaceId)
{
    constexpr string_view source_names[] = {"source"};
    if (has_state_changed<ITreeView>(interfaceId, name, source_names)) {
        watch_source();
    }
    rebuild_rows();
    invoke_trait_dirty(DirtyFlags::Layout | DirtyFlags::Visual);
}

void TreeView::watch_source()
{
    source_changed_ = {};
    auto state = read_state<ITreeView>(this);
    auto hierarchy = state ? state->source.get<IHierarchy>() : nullptr;
    if (hierarchy) {
        source_changed_ = ScopedHandler(hierarchy->on_changed(), [this]() {
            rebuild_rows();
            invoke_trait_dirty(DirtyFlags::Layout | DirtyFlags::Visual);
        });
    }
}

float TreeView::marker_width() const
{
    auto state = read_state<ITreeView>(this);
    return (state ? state->font_size : 14.f) * 0.8f;
}

bool TreeView::is_expanded(const IObject::Ptr& object) const
{
    for (auto& e : expanded_) {
        if (e.lock() == object) {
            return true;
        }
    }
    return false;
}

void TreeView::toggle(const IObject::Ptr& object)
{
    for (size_t i = 0; i < expanded_.size(); ++i) {
        if (expanded_[i].lock() == object) {
            expanded_.erase(expanded_.begin() + i);
            return;
        }
    }
    expanded_.push_back(object);
}

void TreeView::rebuild_rows()
{
    rows_.clear();
    content_width_ = 0.f;
    if (!font_) {
        font_ = Font(text_detail::resolve_font({}));
    }
    auto state = read_state<ITreeView>(this);
    auto hierarchy = state ? state->source.get<IHierarchy>() : nullptr;
    auto root = hierarchy ? hierarchy->root() : nullptr;
    if (!root) {
        return;
    }
    if (expanded_root_.lock() != root) {
        expanded_.clear();
        expanded_.push_back(root);
        expanded_root_ = root;
    }

    auto font = font_.as_ptr<IFont>();
    float scale = 0.f;
    if (font) {
        if (auto font_state = read_state<IFont>(font); font_state && font_state->units_per_em > 0.f) {
            scale = state->font_size / font_state->units_per_em;
        }
    }
    const string_view property(state->label_property.data(), state->label_property.size());
    const float indent = state->indent;
    const float label_x = marker_width() + kLabelGap;
    vector<IFont::GlyphPosition> glyphs;

    std::function<void(const IObject::Ptr&, uint32_t)> add = [&](const IObject::Ptr& object, uint32_t depth) {
        Row row;
        row.object = object;
        row.depth = depth;
        row.has_children = hierarchy->child_count(object) > 0;
        row.expanded = row.has_children && is_expanded(object);
        row.label = label_of(object, property);
        row.text_width = font ? font->shape_text(string_view(row.label.data(), row.label.size()), glyphs) * scale : 0.f;
        content_width_ = std::max(content_width_, depth * indent + label_x + row.text_width);
        const bool expanded = row.expanded;
        rows_.push_back(std::move(row));
        if (expanded) {
            for (auto& child : hierarchy->children_of(object)) {
                add(child, depth + 1);
            }
        }
    };
    add(root, 0);
}

size TreeView::content_size(const size&)
{
    auto state = read_state<ITreeView>(this);
    const float row_height = state ? state->row_height : 22.f;
    return {content_width_, row_height * static_cast<float>(rows_.size()), 0.f};
}

vector<DrawEntry> TreeView::get_draw_entries(::velk::IRenderContext& ctx, const ::velk::size& bounds)
{
    vector<DrawEntry> out;
    auto state = read_state<ITreeView>(this);
    auto visual = read_state<IVisual2D>(this);
    auto font = font_.as_ptr<IFont>();
    if (!state || !visual || !font) {
        return out;
    }
    auto material = interface_pointer_cast<IProgram>(font->get_material());
    auto selected = state->selected.get<IObject>();
    const float rh = state->row_height;
    const float fs = state->font_size;
    const float label_x = marker_width() + kLabelGap;
    const ::velk::size row_box{std::max(bounds.width, content_width_), rh, 0.f};
    IFont::TextLayoutResult layout;

    auto text = [&](string_view s, float x, float y) {
        if (s.empty()) {
            return;
        }
        font->layout_text(s, fs, TextLayout::MultiLine, 0.f, layout);
        text_detail::append_glyphs(layout, row_box, HAlign::Left, VAlign::Center, visual->color, material, x, y, out);
    };

    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto& row = rows_[i];
        const float y = rh * static_cast<float>(i);
        const float x = state->indent * static_cast<float>(row.depth);
        if (selected && row.object.lock() == selected) {
            out.push_back(quad_entry(ctx, 0.f, y, row_box.width, rh, state->selection_color));
        }
        if (row.has_children) {
            text(row.expanded ? kExpandedMarker : kCollapsedMarker, x, y);
        }
        text(string_view(row.label.data(), row.label.size()), x + label_x, y);
    }
    return out;
}

vector<IBuffer::Ptr> TreeView::get_gpu_resources(::velk::IRenderContext& ctx) const
{
    if (auto font = font_.as_ptr<IFont>()) {
        font->ensure_gpu_data(ctx);
    }
    return {};
}

InputResult TreeView::on_pointer_event(PointerEvent& event)
{
    if (event.button != PointerButton::Left
        || (event.action != PointerAction::Down && event.action != PointerAction::Up)) {
        return InputResult::Ignored;
    }
    auto state = read_state<ITreeView>(this);
    if (!state || state->row_height <= 0.f || event.local_position.y < 0.f) {
        return InputResult::Ignored;
    }
    const auto index = static_cast<size_t>(event.local_position.y / state->row_height);
    if (index >= rows_.size()) {
        return InputResult::Ignored;
    }
    if (event.action == PointerAction::Up) {
        return InputResult::Consumed;
    }
    const auto& row = rows_[index];
    auto object = row.object.lock();
    if (!object) {
        return InputResult::Consumed;
    }
    const float x = state->indent * static_cast<float>(row.depth);
    const float px = event.local_position.x;
    if (row.has_children && px >= x && px < x + marker_width() + kLabelGap) {
        toggle(object);
        rebuild_rows();
        invoke_trait_dirty(DirtyFlags::Layout | DirtyFlags::Visual);
    } else {
        write_state<ITreeView>(this, [&](ITreeView::State& s) { set_object_ref(s.selected, object); });
    }
    return InputResult::Consumed;
}

} // namespace velk::ui
