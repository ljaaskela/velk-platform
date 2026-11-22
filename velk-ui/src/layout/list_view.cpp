#include "list_view.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk/interface/resource/intf_resource.h>
#include <velk/interface/resource/intf_resource_store.h>
#include <velk/plugins/importer/api/importer.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-scene/api/element.h>
#include <velk-scene/instance_types.h>
#include <velk-scene/interface/intf_scene.h>

#include <algorithm>
#include <cmath>

namespace velk::ui {

namespace {

/// The root of the first hierarchy in @p store, as Scene::load finds it.
IObject::Ptr store_root(IStore& store)
{
    static const string_view keys[] = {"hierarchy:scene", "hierarchy:main", "hierarchy:root"};
    for (auto& key : keys) {
        if (auto* h = interface_cast<IHierarchy>(store.find(key))) {
            return h->root();
        }
    }
    return {};
}

} // namespace

// RowsCore

void RowsCore::drop_rows(ILayoutContext& ctx)
{
    for (auto& r : active_) {
        ctx.hierarchy().remove(as_object(r.element));
    }
    active_.clear();
    pool_.clear();
    template_uri_ = {};
    template_text_ = {};
    template_failed_ = false;
}

bool RowsCore::take_row(IElement& element, Row& out, string_view uri, string_view fallback_text)
{
    auto scene = element.get_scene();
    if (!scene) {
        return false;
    }
    if (!pool_.empty()) {
        // Removing a row took its whole subtree out of the scene; grafting its store again
        // puts the same objects back.
        out = std::move(pool_.back());
        pool_.pop_back();
        return succeeded(scene->load(*out.store, &element));
    }
    if (template_failed_) {
        return false;
    }
    if (template_text_.empty()) {
        template_uri_ = uri;
        if (uri.empty()) {
            template_text_ = fallback_text;
        } else {
            auto file = instance().resource_store().get_resource<IFile>(template_uri_);
            if (!file || failed(file->read_text(template_text_))) {
                template_text_ = {};
            }
        }
        if (template_text_.empty()) {
            VELK_LOG(E, "row view: cannot read the row template '%s'", template_uri_.c_str());
            template_failed_ = true;
            return false;
        }
    }
    auto imported = create_json_importer().import_from(template_text_, template_uri_);
    auto root = imported.store ? store_root(*imported.store) : IObject::Ptr{};
    auto row_element = interface_pointer_cast<IElement>(root);
    auto row = row_element ? Element(row_element).find_attachment<IListRow>() : IListRow::Ptr{};
    if (!row || failed(scene->load(*imported.store, &element))) {
        VELK_LOG(E, "row view: the row template '%s' needs an element root with a velk-ui.ListRow attachment",
                 template_uri_.c_str());
        template_failed_ = true;
        return false;
    }
    out = {row_element, row, imported.store, 0};
    return true;
}

bool RowsCore::arrange(const aabb& content, IElement& element, ILayoutContext& ctx, size_t count, float rh,
                       float indent, const IObject::Ptr& selected, const std::function<ShownRow(size_t)>& entry,
                       string_view uri, string_view fallback_text)
{
    VELK_PERF_SCOPE("row_view.arrange");
    if (drop_pending_) {
        drop_pending_ = false;
        drop_rows(ctx);
    }
    content_ = content;

    // The rows that show: those crossing the visible part of the content.
    const rect vis = ctx.visible();
    size_t first = 0;
    size_t last = 0;
    if (vis.width > 0.f && vis.height > 0.f && count > 0) {
        const float top = (vis.y - content.position.y) / rh;
        const float bottom = (vis.y + vis.height - content.position.y) / rh;
        first = static_cast<size_t>(std::clamp(std::floor(top), 0.f, static_cast<float>(count)));
        last = static_cast<size_t>(std::clamp(std::ceil(bottom), 0.f, static_cast<float>(count)));
    }

    // Rows that left the range go back to the pool, out of the scene.
    vector<Row> kept;
    kept.reserve(active_.size());
    for (auto& r : active_) {
        if (r.index >= first && r.index < last) {
            kept.push_back(std::move(r));
        } else {
            ctx.hierarchy().remove(as_object(r.element));
            pool_.push_back(std::move(r));
        }
    }
    active_ = std::move(kept);

    const bool range_changed = first != shown_first_ || last != shown_last_;
    shown_first_ = first;
    shown_last_ = last;

    for (size_t i = first; i < last; ++i) {
        Row* row = nullptr;
        for (auto& r : active_) {
            if (r.index == i) {
                row = &r;
                break;
            }
        }
        if (!row) {
            Row fresh;
            if (!take_row(element, fresh, uri, fallback_text)) {
                break;
            }
            fresh.index = i;
            active_.push_back(std::move(fresh));
            row = &active_.back();
        }

        // Tell the row what it shows, writing only what changed.
        const ShownRow e = entry(i);
        auto rs = read_state<IListRow>(row->row);
        const bool is_selected = e.item && e.item == selected;
        if (rs && (rs->item.get() != e.item || rs->index != i || rs->selected != is_selected || rs->depth != e.depth
                   || rs->has_children != e.has_children || rs->expanded != e.expanded)) {
            write_state<IListRow>(row->row, [&](IListRow::State& st) {
                set_object_ref(st.item, e.item);
                st.index = static_cast<uint32_t>(i);
                st.selected = is_selected;
                st.depth = e.depth;
                st.has_children = e.has_children;
                st.expanded = e.expanded;
            });
        }

        const float x = indent * static_cast<float>(e.depth);
        aabb slot;
        slot.position = {content.position.x + x, content.position.y + static_cast<float>(i) * rh, content.position.z};
        slot.extent = {std::max(0.f, content.extent.width - x), rh, content.extent.depth};
        ctx.arrange(*row->element, slot, Alignment::Start, Alignment::Start);
    }
    return range_changed;
}

ptrdiff_t RowsCore::index_at(float local_y, float rh) const
{
    const float y = (local_y - content_.position.y) / rh;
    return y < 0.f ? -1 : static_cast<ptrdiff_t>(y);
}

bool RowsCore::slot_of(const IObject::Ptr& item, float rh, rect& out) const
{
    if (!item) {
        return false;
    }
    for (auto& r : active_) {
        auto rs = read_state<IListRow>(r.row);
        if (rs && rs->item.get() == item) {
            out = {content_.position.x, content_.position.y + static_cast<float>(r.index) * rh, content_.extent.width,
                   rh};
            return true;
        }
    }
    return false;
}

DrawEntry row_quad(::velk::IRenderContext& ctx, const rect& r, const ::velk::color& col)
{
    DrawEntry entry{};
    entry.pipeline_key = PipelineKey::Default;
    if (auto quad = ctx.get_mesh_builder().get_rect_quad(); quad && !quad->get_primitives().empty()) {
        entry.primitive = quad->get_primitives()[0];
    }
    entry.bounds = {r.x, r.y, r.width, r.height};
    ElementInstance inst{};
    inst.offset = {r.x, r.y, 0.f, 0.f};
    inst.size = {r.width, r.height, 0.f, 0.f};
    inst.col = col;
    entry.set_instance(inst);
    return entry;
}

// ListView

IItemSource::Ptr ListView::source() const
{
    auto state = read_state<IListView>(this);
    return state ? state->source.get<IItemSource>() : nullptr;
}

float ListView::row_height() const
{
    auto state = read_state<IListView>(this);
    return state ? std::max(1.f, state->row_height) : 24.f;
}

void ListView::on_state_changed(string_view name, IMetadata&, Uid interfaceId)
{
    constexpr string_view source_names[] = {"source"};
    if (has_state_changed<IListView>(interfaceId, name, source_names)) {
        watch_source();
    }
    constexpr string_view template_names[] = {"row_template"};
    if (has_state_changed<IListView>(interfaceId, name, template_names)) {
        if (auto state = read_state<IListView>(this)) {
            rows_.set_template(state->row_template);
        }
    }
    invoke_trait_dirty(DirtyFlags::Layout | DirtyFlags::Visual);
}

void ListView::watch_source()
{
    // State writes name no property, so this runs for any of them: only a new source resubscribes.
    auto s = source();
    if (s.get() == watched_.lock().get()) {
        return;
    }
    watched_ = s;
    source_changed_ = {};
    if (s) {
        // Rows are bound by index and rebound in arrange, so any change is a new layout.
        source_changed_ = ScopedHandler(s->on_items_changed(), [this](const ItemChange&) {
            invoke_trait_dirty(DirtyFlags::Layout | DirtyFlags::Visual);
        });
    }
}

void ListView::describe(LayoutBox& box) const
{
    // As tall as all the rows; as wide as offered.
    box.height = dim::auto_size();
}

size ListView::measure(const size& available, IElement&, ILayoutContext&)
{
    auto s = source();
    const size_t count = s ? s->child_count({}) : 0;
    return {available.width, static_cast<float>(count) * row_height(), 0.f};
}

void ListView::arrange(const aabb& content, IElement& element, ILayoutContext& ctx)
{
    auto s = source();
    auto state = read_state<IListView>(this);
    const size_t count = s ? s->child_count({}) : 0;
    const IObject::Ptr selected = state ? state->selected.get() : nullptr;
    const string uri = state ? state->row_template : string{};
    const bool changed = rows_.arrange(
        content, element, ctx, count, row_height(), 0.f, selected,
        [&](size_t i) { return ShownRow{s->child_at({}, i)}; }, uri, {});
    if (changed) {
        // The selection's background is drawn for the rows that show.
        invoke_trait_dirty(DirtyFlags::Visual);
    }
}

vector<DrawEntry> ListView::get_draw_entries(::velk::IRenderContext& ctx, const ::velk::size&)
{
    vector<DrawEntry> out;
    auto state = read_state<IListView>(this);
    auto visual = read_state<IVisual2D>(this);
    rect r;
    if (state && visual && rows_.slot_of(state->selected.get(), row_height(), r)) {
        out.push_back(row_quad(ctx, r, visual->color));
    }
    return out;
}

InputResult ListView::on_pointer_event(PointerEvent& event)
{
    if (event.action != PointerAction::Down || event.button != PointerButton::Left) {
        return InputResult::Ignored;
    }
    auto s = source();
    const ptrdiff_t index = rows_.index_at(event.local_position.y, row_height());
    auto item = s && index >= 0 ? s->child_at({}, static_cast<size_t>(index)) : IObject::Ptr{};
    if (!item) {
        return InputResult::Ignored;
    }
    write_state<IListView>(this, [&](IListView::State& st) { set_object_ref(st.selected, item); });
    return InputResult::Consumed;
}

} // namespace velk::ui
