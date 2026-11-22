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
        auto state = read_state<IListView>(this);
        if (state && state->row_template != template_uri_) {
            drop_pending_ = true;
        }
    }
    invoke_trait_dirty(DirtyFlags::Layout | DirtyFlags::Visual);
}

void ListView::watch_source()
{
    source_changed_ = {};
    if (auto s = source()) {
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

void ListView::drop_rows(ILayoutContext* ctx)
{
    if (ctx) {
        for (auto& r : active_) {
            ctx->hierarchy().remove(as_object(r.element));
        }
    }
    active_.clear();
    pool_.clear();
    template_uri_ = {};
    template_text_ = {};
    template_failed_ = false;
}

bool ListView::take_row(IElement& element, ILayoutContext& ctx, Row& out)
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
        auto state = read_state<IListView>(this);
        template_uri_ = state ? state->row_template : string{};
        auto file = instance().resource_store().get_resource<IFile>(template_uri_);
        if (template_uri_.empty() || !file || failed(file->read_text(template_text_)) || template_text_.empty()) {
            VELK_LOG(E, "ListView: cannot read the row template '%s'", template_uri_.c_str());
            template_failed_ = true;
            return false;
        }
    }
    auto imported = create_json_importer().import_from(template_text_, template_uri_);
    auto root = imported.store ? store_root(*imported.store) : IObject::Ptr{};
    auto row_element = interface_pointer_cast<IElement>(root);
    auto row = row_element ? Element(row_element).find_attachment<IListRow>() : IListRow::Ptr{};
    if (!row || failed(scene->load(*imported.store, &element))) {
        VELK_LOG(E, "ListView: the row template '%s' needs an element root with a velk-ui.ListRow attachment",
                 template_uri_.c_str());
        template_failed_ = true;
        return false;
    }
    out = {row_element, row, imported.store, 0};
    return true;
}

void ListView::arrange(const aabb& content, IElement& element, ILayoutContext& ctx)
{
    VELK_PERF_SCOPE("list_view.arrange");
    if (drop_pending_) {
        drop_pending_ = false;
        drop_rows(&ctx);
    }
    content_ = content;
    auto s = source();
    const size_t count = s ? s->child_count({}) : 0;
    const float rh = row_height();

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

    // The selection's background is drawn for the rows that show.
    if (first != shown_first_ || last != shown_last_) {
        shown_first_ = first;
        shown_last_ = last;
        invoke_trait_dirty(DirtyFlags::Visual);
    }

    auto state = read_state<IListView>(this);
    const IObject::Ptr selected = state ? state->selected.get() : nullptr;
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
            if (!take_row(element, ctx, fresh)) {
                break;
            }
            fresh.index = i;
            active_.push_back(std::move(fresh));
            row = &active_.back();
        }

        // Tell the row what it shows, writing only what changed.
        auto item = s->child_at({}, i);
        auto rs = read_state<IListRow>(row->row);
        const bool is_selected = item && item == selected;
        if (rs && (rs->item.get() != item || rs->index != i || rs->selected != is_selected)) {
            write_state<IListRow>(row->row, [&](IListRow::State& st) {
                set_object_ref(st.item, item);
                st.index = static_cast<uint32_t>(i);
                st.selected = is_selected;
            });
        }

        aabb slot;
        slot.position = {content.position.x, content.position.y + static_cast<float>(i) * rh, content.position.z};
        slot.extent = {content.extent.width, rh, content.extent.depth};
        ctx.arrange(*row->element, slot, Alignment::Start, Alignment::Start);
    }
}

vector<DrawEntry> ListView::get_draw_entries(::velk::IRenderContext& ctx, const ::velk::size&)
{
    vector<DrawEntry> out;
    auto state = read_state<IListView>(this);
    auto visual = read_state<IVisual2D>(this);
    auto s = source();
    const IObject::Ptr selected = state ? state->selected.get() : nullptr;
    if (!state || !visual || !s || !selected) {
        return out;
    }
    // Only a selected row that shows has a background; its index comes from the rows.
    for (auto& r : active_) {
        auto rs = read_state<IListRow>(r.row);
        if (!rs || rs->item.get() != selected) {
            continue;
        }
        const float rh = row_height();
        DrawEntry entry{};
        entry.pipeline_key = PipelineKey::Default;
        if (auto quad = ctx.get_mesh_builder().get_rect_quad(); quad && !quad->get_primitives().empty()) {
            entry.primitive = quad->get_primitives()[0];
        }
        const float x = content_.position.x;
        const float y = content_.position.y + static_cast<float>(r.index) * rh;
        entry.bounds = {x, y, content_.extent.width, rh};
        ElementInstance inst{};
        inst.offset = {x, y, 0.f, 0.f};
        inst.size = {content_.extent.width, rh, 0.f, 0.f};
        inst.col = visual->color;
        entry.set_instance(inst);
        out.push_back(entry);
        break;
    }
    return out;
}

InputResult ListView::on_pointer_event(PointerEvent& event)
{
    if (event.action != PointerAction::Down || event.button != PointerButton::Left) {
        return InputResult::Ignored;
    }
    auto s = source();
    if (!s) {
        return InputResult::Ignored;
    }
    const float y = (event.local_position.y - content_.position.y) / row_height();
    if (y < 0.f) {
        return InputResult::Ignored;
    }
    const size_t index = static_cast<size_t>(y);
    auto item = s->child_at({}, index);
    if (!item) {
        return InputResult::Ignored;
    }
    write_state<IListView>(this, [&](IListView::State& st) { set_object_ref(st.selected, item); });
    return InputResult::Consumed;
}

} // namespace velk::ui
