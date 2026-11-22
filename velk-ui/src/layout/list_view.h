#ifndef VELK_UI_LIST_VIEW_H
#define VELK_UI_LIST_VIEW_H

#include <velk/api/event.h>
#include <velk/ext/object.h>
#include <velk/interface/intf_store.h>
#include <velk/plugins/data/interface/intf_item_source.h>
#include <velk-scene/ext/trait.h>
#include <velk-ui/interface/intf_input_trait.h>
#include <velk-ui/interface/intf_layout_trait.h>
#include <velk-ui/interface/intf_list_row.h>
#include <velk-ui/interface/trait/intf_list_view.h>
#include <velk-ui/plugin.h>

#include <functional>

namespace velk::ui {

/** @brief See IListRow. Holds values only; the view writes them. */
class ListRow : public ::velk::ext::Object<ListRow, IListRow>
{
public:
    VELK_CLASS_UID(ClassId::ListRow, "ListRow");
};

/** @brief What one shown row displays. */
struct ShownRow
{
    IObject::Ptr item;
    uint32_t depth = 0;
    bool has_children = false;
    bool expanded = false;
};

/**
 * @brief The rows of a list or tree view: created from a template only where they show, kept in
 *        a pool when they scroll out, and told what to show.
 */
class RowsCore
{
public:
    /** @brief Drops every row at the next arrange when @p uri is not the template in use. */
    void set_template(string_view uri)
    {
        if (uri != template_uri_) {
            drop_pending_ = true;
        }
    }

    /**
     * @brief Places the rows that show and tells them what to show.
     * @param count     Number of shown rows; row @c i is at @c i times @p rh.
     * @param indent    Horizontal offset per depth level; a tree's rows also leave one level for
     *                  the marker.
     * @param entry     What row @c i shows.
     * @param uri       The row template; empty uses @p fallback_text.
     * @return True when the range of shown rows changed.
     */
    bool arrange(const aabb& content, IElement& element, ILayoutContext& ctx, size_t count, float rh, float indent,
                 const IObject::Ptr& selected, const std::function<ShownRow(size_t)>& entry, string_view uri,
                 string_view fallback_text);

    /** @brief The row index under @p local_y in the element, or -1. */
    ptrdiff_t index_at(float local_y, float rh) const;

    /** @brief The rectangle of the shown row displaying @p item, if it is in the scene. */
    bool slot_of(const IObject::Ptr& item, float rh, rect& out) const;

    /** @brief Calls @p fn with the index and state of each row in the scene. */
    template <class Fn>
    void for_each_shown(Fn&& fn) const
    {
        for (auto& r : active_) {
            fn(r.index, r.row);
        }
    }

    const aabb& content() const { return content_; }

private:
    struct Row
    {
        IElement::Ptr element;
        IListRow::Ptr row;
        IStore::Ptr store; ///< The row's import; grafting it again restores the row's subtree.
        size_t index = 0;
    };

    bool take_row(IElement& element, Row& out, string_view uri, string_view fallback_text);
    void drop_rows(ILayoutContext& ctx);

    string template_uri_;  ///< The template the text below came from.
    string template_text_; ///< Read once, imported once per row created.
    bool template_failed_ = false;
    bool drop_pending_ = false;
    vector<Row> active_; ///< Rows in the scene, each showing one index.
    vector<Row> pool_;   ///< Rows out of the scene, kept for reuse.
    aabb content_{};
    size_t shown_first_ = 0;
    size_t shown_last_ = 0;
};

/** @brief A selection background quad at @p r. */
DrawEntry row_quad(::velk::IRenderContext& ctx, const rect& r, const ::velk::color& col);

/**
 * @brief See IListView. Sizes the element to its rows, creates and reuses rows for the part
 *        that shows, takes clicks and draws the selection, in one attachment.
 */
class ListView : public ::velk::ext::Visual2D<ListView, IListView, ILayoutTrait, IInputTrait>
{
public:
    VELK_CLASS_UID(ClassId::Constraint::ListView, "ListView");

    TraitPhase get_phase() const override
    {
        return TraitPhase::Constraint | TraitPhase::Layout | TraitPhase::Input | TraitPhase::Visual;
    }

    // ILayoutTrait
    void describe(LayoutBox& box) const override;
    size measure(const size& available, IElement& element, ILayoutContext& ctx) override;
    void arrange(const aabb& content, IElement& element, ILayoutContext& ctx) override;

    // IVisual: the selected row's background.
    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx, const ::velk::size& bounds) override;

    // IInputTrait
    InputResult on_intercept(PointerEvent&) override { return InputResult::Ignored; }
    InputResult on_pointer_event(PointerEvent& event) override;
    void on_pointer_enter(const PointerEvent&) override {}
    void on_pointer_leave(const PointerEvent&) override {}
    InputResult on_scroll_event(ScrollEvent&) override { return InputResult::Ignored; }
    InputResult on_key_event(KeyEvent&) override { return InputResult::Ignored; }

protected:
    void on_state_changed(string_view name, IMetadata& owner, Uid interfaceId) override;

private:
    IItemSource::Ptr source() const;
    float row_height() const;
    void watch_source();

    ScopedHandler source_changed_;
    IItemSource::WeakPtr watched_; ///< The source source_changed_ listens to.
    RowsCore rows_;
};

} // namespace velk::ui

#endif // VELK_UI_LIST_VIEW_H
